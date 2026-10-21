#!/usr/bin/env bash
# ============================================================================
# bench_matrix.sh — build-and-run matrix across flavors and compilers.
# Runs, per available compiler × {Release, Audit}:
#   * the full Google Benchmark suite (short min_time)   [Release only]
#   * the RDTSC percentile harness
#   * the real-world simulation smoke (200k events)
# Results land in bench-results/<compiler>-<flavor>.txt
# ============================================================================
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/bench-results"; mkdir -p "$OUT"
MIN_TIME="${HFT_BENCH_MIN_TIME:-0.1s}"
COMPILERS=()
command -v g++ >/dev/null && COMPILERS+=("g++")
command -v clang++ >/dev/null && COMPILERS+=("clang++")

fail=0
for cc in "${COMPILERS[@]:-g++}"; do
    tag="$(basename "$cc")"
    for flavor in Release Audit; do
        dir="$ROOT/build-matrix-$tag-$flavor"
        log="$OUT/$tag-$flavor.txt"
        echo "=================================================================="
        echo "[*] $tag / $flavor"
        echo "=================================================================="
        {
        echo "# matrix: $tag $flavor  ($(date -u +%FT%TZ))"
        if ! cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE="$flavor" -DCMAKE_CXX_COMPILER="$cc"; then
            echo "[X] configure failed"; fail=1; continue
        fi
        cmake --build "$dir" -j"$(nproc)" || { echo "[X] build failed"; fail=1; continue; }
        if [ "$flavor" = "Release" ] && [ -x "$dir/hft_bench" ]; then
            echo; echo "## hft_bench (min_time=$MIN_TIME)"; echo
            "$dir/hft_bench" --benchmark_min_time="$MIN_TIME" || fail=1
        fi
        echo; echo "## hft_rdtsc_bench"; echo
        HFT_DISABLE_MLOCKALL=1 "$dir/hft_rdtsc_bench" || fail=1
        echo; echo "## hft_sim (200k events)"; echo
        "$dir/hft_sim" 200000 || fail=1
        } > "$log" 2>&1
        tail -5 "$log"
        echo "[OK] results -> $log"
    done
done
exit $fail
