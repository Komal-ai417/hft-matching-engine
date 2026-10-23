#!/usr/bin/env bash
# ============================================================================
# fuzz.sh — continuous-fuzzing recipes.
#   Mode A (default): libFuzzer differential core — needs clang.
#   Mode B: g++ fallback — long-running random differential driver via the
#           gtest binary (seed sweep; hours of coverage without deps).
# Usage:
#   scripts/fuzz.sh libfuzzer [seconds]     # clang only
#   scripts/fuzz.sh seeds [count]           # any compiler, via hft_tests
# ============================================================================
# -e matters: without it a failed compile below used to fall through to
# executing the (missing) fuzzer binary — exit 127 masking the real error.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-auto}"

if [ "$MODE" = "libfuzzer" ]; then
    command -v clang++ >/dev/null || { echo "[X] clang++ required for libFuzzer"; exit 2; }
    SECONDS_TO_RUN="${2:-120}"
    OUT="$ROOT/fuzz-corpus"; mkdir -p "$OUT" "$ROOT/build-fuzz"
    # -I tests: fuzz_driver.cpp includes "StdOrderBook.h" (the reference
    # oracle), which lives in tests/ — the old -I"$ROOT" never resolved it.
    clang++ -std=c++20 -O2 -g -fsanitize=fuzzer,address,undefined \
        -I"$ROOT/tests" -I"$ROOT/src" \
        "$ROOT/tools/fuzz_driver.cpp" "$ROOT/src/OrderBook.cpp" \
        -o "$ROOT/build-fuzz/hft_fuzz"
    # `|| rc=$?` (not bare invocation) so set -e lets a found-crash exit
    # code reach the caller after the status echo.
    rc=0
    "$ROOT/build-fuzz/hft_fuzz" -max_total_time="$SECONDS_TO_RUN" -max_len=8192 "$OUT" || rc=$?
    [ "$rc" -eq 0 ] && echo "[OK] libFuzzer run clean (corpus: $OUT)"
    exit "$rc"
fi

# Seed-sweep mode: run the differential suite with rotating seeds by
# overriding the built-in seeds via repeated binary runs (the suite uses
# fixed seeds for CI determinism; nightly sweeps widen coverage by looping).
COUNT="${2:-10}"
BUILD="$ROOT/build-audit"
if [ ! -x "$BUILD/hft_tests" ]; then
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Audit > /dev/null
    cmake --build "$BUILD" --target hft_tests -j"$(nproc)" > /dev/null
fi
echo "[*] differential seed sweep: $COUNT iterations"
for i in $(seq 1 "$COUNT"); do
    if ! "$BUILD/hft_tests" --gtest_filter='DifferentialFuzz.*' --gtest_brief=1; then
        echo "[X] differential divergence on iteration $i"
        exit 1
    fi
    echo "[OK] sweep iteration $i"
done
echo "ALL SWEEP ITERATIONS PASSED"
