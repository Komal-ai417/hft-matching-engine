#!/usr/bin/env bash
# ============================================================================
# run_valgrind.sh — Valgrind Memcheck pass over the test suite and the
# standalone harnesses (audit §8.8: "one clean run per release"). Requires
# the Release config so pool instrumentation doesn't generate its own noise.
# ============================================================================
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-rel}"

if ! command -v valgrind >/dev/null; then
    echo "[skip] valgrind not installed on this host"
    exit 0
fi

cd "$BUILD" || { echo "[X] build dir $BUILD missing (run cmake first)"; exit 2; }

echo "[*] Valgrind memcheck: hft_tests"
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
    --track-origins=yes --error-exitcode=99 \
    ./hft_tests --gtest_brief=1
rc=$?
if [ $rc -eq 99 ]; then
    echo "[X] valgrind reported errors"
    exit 1
elif [ $rc -ne 0 ]; then
    echo "[X] tests failed under valgrind (rc=$rc)"
    exit 1
fi
echo "[OK] valgrind memcheck clean"
