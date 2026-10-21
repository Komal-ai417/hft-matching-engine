#!/usr/bin/env bash
# ============================================================================
# run_all.sh — end-to-end verification pipeline (11 phases).
#
# Usage:
#   ./scripts/run_all.sh               # full run
#   FAST=1 ./scripts/run_all.sh        # quick mode (shorter fuzz/bench/sim)
#   JOBS=8 ./scripts/run_all.sh        # control parallel build jobs
#   RDIR=path ./scripts/run_all.sh     # resume an interrupted bundle (phases
#                                      # that already PASSED are skipped;
#                                      # remove their .phase-*.done markers
#                                      # or set FORCE=1 to re-run everything)
#
# Phases (in order):
#   env, build, tests, fuzz, valgrind, static, bench, rdtsc, sim, smoke,
#   summary
#
# Output:
#   verify-results/<UTC-timestamp>/{per-phase logs, summary.txt}
#
# Exit code: number of failed phases (0 = all green).
#
# Notes:
#   * The five build flavors share one FetchContent source checkout (the
#     first flavor clones googletest/googlebenchmark at the SHA pinned in
#     CMakeLists.txt; the others reuse it — one clone, five build trees).
#   * Tools that are not installed on this host (valgrind, cppcheck,
#     clang-tidy, clang++, cc) are skipped with a [skip] note instead of
#     failing the pipeline.
# ============================================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
FAST="${FAST:-0}"
FUZZ_SECS="${FUZZ_SECS:-30}"
SIM_EVENTS="${SIM_EVENTS:-1000000}"
[ "$FAST" = "1" ] && { FUZZ_SECS=8; SIM_EVENTS=200000; }

TS="$(date -u +%Y%m%dT%H%M%SZ)"
RDIR="${RDIR:-$ROOT/verify-results/$TS}"
FORCE="${FORCE:-0}"
mkdir -p "$RDIR"

FLAVORS="${FLAVORS:-debug audit release ubsan lsan}"

say()  { printf '\n===== [%s] %s =====\n' "$(date -u +%H:%M:%S)" "$*"; }
note() { printf '    %s\n' "$*"; }

declare -a PHASES=()
declare -a RESULTS=()
declare -a DETAILS=()
record() { PHASES+=("$1"); RESULTS+=("$2"); DETAILS+=("$3"); say "PHASE $1: $2 — $3"; }

# Resume support: a phase writes its PASS marker on success; a re-run with
# the same RDIR skips it. FORCE=1 re-runs everything.
phase_done() { [ -f "$RDIR/.phase-$1.done" ] && [ "$FORCE" != "1" ]; }
mark_done()  { printf '%s\n' "$2" > "$RDIR/.phase-$1.done"; }
resume_from() { # <phase>: re-record a previously passed phase
    local d; d="$(cat "$RDIR/.phase-$1.done" 2>/dev/null || true)"
    record "$1" PASS "${d:-resumed}"
}

# ────────────────────────────────────────────────────────────────────────────
say "verify-results → $RDIR"

# ── Phase: env ─────────────────────────────────────────────────────────────
if phase_done env; then
    resume_from env
else
    {
        echo "date:    $(date -u)"
        echo "host:    $(uname -a)"
        echo "JOBS:    $JOBS    FAST: $FAST"
        echo "flavors: $FLAVORS"
        echo
        for t in g++ gcc clang++ cmake ctest valgrind cppcheck clang-tidy clang-format gcovr python3 perf; do
            if command -v "$t" >/dev/null 2>&1; then
                printf '%-12s %s\n' "$t" "$("$t" --version 2>/dev/null | head -1)"
            else
                printf '%-12s (not installed)\n' "$t"
            fi
        done
    } > "$RDIR/env.log" 2>&1
    record env PASS "see env.log"
    mark_done env "see env.log"
fi

# ── Phase: build (5 flavors) ────────────────────────────────────────────────
if phase_done build; then
    resume_from build
else
    say "building flavors: $FLAVORS (JOBS=$JOBS)"
    build_ok=1
    dep_gtest=""
    dep_bench=""
    for f in $FLAVORS; do
        case "$f" in
            debug)   cfg=(-DCMAKE_BUILD_TYPE=Debug) ;;
            audit)   cfg=(-DCMAKE_BUILD_TYPE=Audit) ;;
            release) cfg=(-DCMAKE_BUILD_TYPE=Release) ;;
            ubsan)   cfg=(-DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=OFF -DENABLE_UBSAN=ON) ;;
            lsan)    cfg=(-DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=OFF -DENABLE_UBSAN=OFF
                          -DCMAKE_CXX_FLAGS=-fsanitize=leak -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=leak) ;;
        esac
        # Share the pinned dependency sources across flavors (one clone).
        extra=()
        [ -n "$dep_gtest" ] && extra+=(-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$dep_gtest")
        [ -n "$dep_bench" ] && extra+=(-DFETCHCONTENT_SOURCE_DIR_GOOGLEBENCHMARK="$dep_bench")
        note "flavor $f ..."
        if ! cmake -S "$ROOT" -B "$ROOT/build-$f" "${cfg[@]}" "${extra[@]}" > "$RDIR/build-$f.log" 2>&1 \
           || ! cmake --build "$ROOT/build-$f" -j"$JOBS" >> "$RDIR/build-$f.log" 2>&1; then
            build_ok=0
            note "[X] flavor $f failed — see build-$f.log"
        fi
        [ -z "$dep_gtest" ] && [ -d "$ROOT/build-$f/_deps/googletest-src" ] \
            && dep_gtest="$ROOT/build-$f/_deps/googletest-src"
        [ -z "$dep_bench" ] && [ -d "$ROOT/build-$f/_deps/googlebenchmark-src" ] \
            && dep_bench="$ROOT/build-$f/_deps/googlebenchmark-src"
    done
    grep -ihE "warning:" "$RDIR"/build-*.log > "$RDIR/build-warnings.log" 2>/dev/null || true
    n_warn=$(wc -l < "$RDIR/build-warnings.log")
    if [ "$build_ok" = "1" ] && [ "$n_warn" = "0" ]; then
        record build PASS "5 flavors, 0 warnings"
        mark_done build "5 flavors, 0 warnings"
    else
        [ "$n_warn" != "0" ] && detail="$n_warn warning(s)" || detail="see build-*.log"
        record build FAIL "$detail"
    fi
fi

# ── Phase: tests (ctest per flavor) ─────────────────────────────────────────
if phase_done tests; then
    resume_from tests
else
    say "running ctest for every flavor"
    tests_ok=1
    counts=""
    for f in $FLAVORS; do
        if ! (cd "$ROOT/build-$f" && ctest --output-on-failure -V) > "$RDIR/tests-$f.log" 2>&1; then
            tests_ok=0
            note "[X] tests failed — see tests-$f.log"
        fi
        n=$(grep -oE 'PASSED  \] [0-9]+ tests?' "$RDIR/tests-$f.log" | grep -oE '[0-9]+' | tail -1)
        counts="$counts $f=${n:-?}"
    done
    if [ "$tests_ok" = "1" ]; then
        record tests PASS "$counts"
        mark_done tests "$counts"
    else
        record tests FAIL "see tests-*.log"
    fi
fi

# ── Phase: fuzz (2 differential sweeps + libFuzzer) ─────────────────────────
if phase_done fuzz; then
    resume_from fuzz
else
    say "differential sweeps + libFuzzer"
    fuzz_ok=1
    for i in 1 2; do
        [ "$i" = "1" ] && fb="audit" || fb="release"
        {
            echo "[sweep $i] differential suite ($fb build, DifferentialFuzz.* configs)"
            "$ROOT/build-$fb/hft_tests" --gtest_filter='DifferentialFuzz.*' && \
                echo "[OK] zero divergence (all configs, $fb build)"
        } > "$RDIR/fuzz-sweep-$i.log" 2>&1 || fuzz_ok=0
    done
    fuzz_detail="2 sweeps + libFuzzer"
    if command -v clang++ >/dev/null 2>&1; then
        if clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined \
            -I"$ROOT/tests" "$ROOT/tools/fuzz_driver.cpp" "$ROOT/src/OrderBook.cpp" \
            -o "$RDIR/hft_fuzz" > "$RDIR/fuzz-libfuzzer-build.log" 2>&1; then
            : > "$RDIR/fuzz-libfuzzer.log"
            mkdir -p "$ROOT/fuzz-corpus"   # libFuzzer requires an existing corpus dir
            if ! "$RDIR/hft_fuzz" -max_total_time="$FUZZ_SECS" -rss_limit_mb=2560 \
                 "$ROOT/fuzz-corpus" >> "$RDIR/fuzz-libfuzzer.log" 2>&1; then
                fuzz_ok=0
                note "[X] libFuzzer found something — see fuzz-libfuzzer.log"
            fi
        else
            fuzz_ok=0
            note "[X] libFuzzer build failed — see fuzz-libfuzzer-build.log"
        fi
    else
        note "[skip] clang++ not installed — libFuzzer leg skipped (sweeps still ran)"
        fuzz_detail="2 sweeps (libFuzzer skipped: clang++ not installed)"
    fi
    if [ "$fuzz_ok" = "1" ]; then
        record fuzz PASS "$fuzz_detail"
        mark_done fuzz "$fuzz_detail"
    else
        record fuzz FAIL "see fuzz-*.log"
    fi
fi

# ── Phase: valgrind ─────────────────────────────────────────────────────────
if phase_done valgrind; then
    resume_from valgrind
else
    if command -v valgrind >/dev/null 2>&1; then
        say "valgrind: suite + engine"
        vg_ok=1
        valgrind --quiet --error-exitcode=9 --leak-check=full \
            "$ROOT/build-release/hft_tests" > "$RDIR/valgrind-tests.log" 2>&1 || vg_ok=0
        HFT_DISABLE_MLOCKALL=1 valgrind --quiet --error-exitcode=9 --leak-check=full \
            "$ROOT/build-release/hft_engine" > "$RDIR/valgrind-engine.log" 2>&1 || vg_ok=0
        if [ "$vg_ok" = "1" ]; then
            record valgrind PASS "suite + engine clean"
            mark_done valgrind "suite + engine clean"
        else
            record valgrind FAIL "see valgrind-*.log"
        fi
    else
        note "[skip] valgrind not installed on this host"
        record valgrind PASS "skipped (valgrind not installed)"
        mark_done valgrind "skipped (valgrind not installed)"
    fi
fi

# ── Phase: static ───────────────────────────────────────────────────────────
if phase_done static; then
    resume_from static
else
    say "static analysis (run_static.sh)"
    if bash "$ROOT/scripts/run_static.sh" > "$RDIR/static.log" 2>&1; then
        record static PASS "see static.log"
        mark_done static "see static.log"
    else
        record static FAIL "see static.log"
    fi
fi

# ── Phase: bench (google benchmark, release build) ──────────────────────────
if phase_done bench; then
    resume_from bench
else
    say "42-scenario benchmark suite"
    bench_args=()
    [ "$FAST" = "1" ] && bench_args=(--benchmark_min_time=0.1s)
    if "$ROOT/build-release/hft_bench" ${bench_args[@]+"${bench_args[@]}"} > "$RDIR/bench.log" 2>&1; then
        n_scen=$(grep -cE '^BM_' "$RDIR/bench.log")
        record bench PASS "$n_scen scenarios"
        mark_done bench "$n_scen scenarios"
    else
        record bench FAIL "see bench.log"
    fi
fi

# ── Phase: rdtsc ────────────────────────────────────────────────────────────
if phase_done rdtsc; then
    resume_from rdtsc
else
    say "RDTSC latency harness"
    if "$ROOT/build-release/hft_rdtsc_bench" > "$RDIR/rdtsc.log" 2>&1; then
        record rdtsc PASS "see rdtsc.log"
        mark_done rdtsc "see rdtsc.log"
    else
        record rdtsc FAIL "see rdtsc.log"
    fi
fi

# ── Phase: sim (real-world simulation) ──────────────────────────────────────
if phase_done sim; then
    resume_from sim
else
    say "real-world simulation ($SIM_EVENTS events)"
    if "$ROOT/build-release/hft_sim" "$SIM_EVENTS" > "$RDIR/sim.log" 2>&1; then
        d="$(grep -oE 'TIME DIFFERENCE:.*' "$RDIR/sim.log" | head -1)"
        record sim PASS "$d"
        mark_done sim "$d"
    else
        record sim FAIL "see sim.log"
    fi
fi

# ── Phase: smoke (4 engine paths incl. the gVisor self-heal) ────────────────
if phase_done smoke; then
    resume_from smoke
else
    say "engine smoke matrix"
    smoke_ok=1
    smoke_n=0
    run_smoke() { # name, expected-grep, command...
        local name="$1" expect="$2"; shift 2
        smoke_n=$((smoke_n + 1))
        if "$@" > "$RDIR/smoke-$name.log" 2>&1 && grep -q "$expect" "$RDIR/smoke-$name.log"; then
            note "[OK] $name"
        else
            smoke_ok=0
            note "[X] $name — see smoke-$name.log"
        fi
    }
    run_smoke engine-nolock "mlockall skipped" \
        env HFT_DISABLE_MLOCKALL=1 "$ROOT/build-release/hft_engine"
    run_smoke engine-madviseonly "madvise-only mode" \
        env HFT_MADVISE_ONLY=1 "$ROOT/build-release/hft_engine"
    if command -v cc >/dev/null 2>&1; then
        if cc -shared -fPIC -O2 -Wall -Wextra -o "$RDIR/mlock_gvisor_sim.so" \
              "$ROOT/tools/mlock_gvisor_sim.c" -ldl > "$RDIR/smoke-gvisorsim-build.log" 2>&1; then
            run_smoke engine-gvisorsim "reverting to pageable" \
                env LD_PRELOAD="$RDIR/mlock_gvisor_sim.so" "$ROOT/build-release/hft_engine"
        else
            smoke_ok=0
            note "[X] gvisor sim build failed — see smoke-gvisorsim-build.log"
        fi
    else
        note "[skip] cc not installed — gvisor-sim leg skipped"
    fi
    run_smoke engine-default "Engine run complete" \
        "$ROOT/build-release/hft_engine"
    if [ "$smoke_ok" = "1" ]; then
        record smoke PASS "$smoke_n checks"
        mark_done smoke "$smoke_n checks"
    else
        record smoke FAIL "see smoke-*.log"
    fi
fi

# ── Phase: summary ──────────────────────────────────────────────────────────
{
    echo "PHASE      RESULT DETAIL"
    echo "-----      ------ ------"
    for i in "${!PHASES[@]}"; do
        printf '%-10s %-6s %s\n' "${PHASES[$i]}" "${RESULTS[$i]}" "${DETAILS[$i]}"
    done
} > "$RDIR/summary.txt"

say "summary"
cat "$RDIR/summary.txt"

failed=0
for r in "${RESULTS[@]}"; do [ "$r" = "FAIL" ] && failed=$((failed + 1)); done
say "results → $RDIR"
if [ "$failed" = "0" ]; then
    say "ALL PHASES PASSED"
else
    say "$failed PHASE(S) FAILED"
fi
exit "$failed"
