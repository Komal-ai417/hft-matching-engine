#!/usr/bin/env bash
# ============================================================================
# perf.sh — hardware-counter profiling recipes (audit §8.8, docs/1).
# Degrades gracefully without perf or permissions. Produces:
#   1. perf stat overview of the RDTSC harness (IPC, cache + branch misses)
#   2. perf record/report of the benchmark suite (call-graph attribution)
#   3. FlameGraph collapse (if flamegraph tools are present)
# Usage: scripts/perf.sh [build_dir]
# ============================================================================
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-rel}"
OUT="$ROOT/perf-out"; mkdir -p "$OUT"

if ! command -v perf >/dev/null; then
    echo "[skip] perf not installed"
    exit 0
fi
cd "$BUILD" || { echo "[X] build dir missing"; exit 2; }

echo "[*] 1/2 perf stat — hft_rdtsc_bench (IPC, cache, branch misses)"
perf stat -e cycles,instructions,branches,branch-misses,\
L1-dcache-load-misses,LLC-load-misses,context-switches,cpu-migrations \
    ./hft_rdtsc_bench 2>&1 | tee "$OUT/perf_stat_rdtsc.txt" || true

echo "[*] 2/2 perf record — hft_bench (call-graph attribution)"
if [ -x ./hft_bench ]; then
    perf record -g --call-graph dwarf -o "$OUT/perf.data" \
        ./hft_bench --benchmark_min_time=0.3s --benchmark_filter='BM_Matching_DeepBook|BM_SweepMultipleLevels|BM_Churn' \
        > "$OUT/perf_record.log" 2>&1 || true
    perf report -i "$OUT/perf.data" --stdio > "$OUT/perf_report.txt" 2>/dev/null || true
    echo "[OK] report: $OUT/perf_report.txt"
else
    echo "[skip] hft_bench not built in $BUILD"
fi

if command -v flamegraph-stackcollapse-perf >/dev/null; then
    perf script -i "$OUT/perf.data" | flamegraph-stackcollapse-perf | \
        flamegraph-flamegraph > "$OUT/flame.svg" 2>/dev/null && echo "[OK] $OUT/flame.svg"
else
    echo "[hint] install FlameGraph tools for $OUT/flame.svg"
fi
