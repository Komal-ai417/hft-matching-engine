#!/usr/bin/env bash
# ============================================================================
# run_sanitizers.sh — build & run the full test suite under every sanitizer
# flavor available on this host (audit §8.8 external tooling recipes).
#   1. Debug      (ASan + UBSan)   — memory errors + UB
#   2. Debug-UB   (UBSan only)     — faster UB-focused pass
#   3. Audit      (instrumented -O2, no sanitizers) — optimized-canary pass
#   4. LSan       (leak sanitizer via ASan) — leak pass
# Usage: scripts/run_sanitizers.sh [build_dir]
# ============================================================================
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-san}"
command -v cmake >/dev/null || { echo "[!] cmake not found"; exit 2; }

fail=0
run_flavor() {
    local name="$1"; shift
    local dir="$BUILD/$name"
    echo "=================================================================="
    echo "[*] Flavor: $name"
    echo "=================================================================="
    if ! cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=Debug "$@" > "$dir-cfg.log" 2>&1; then
        echo "[X] configure failed — see $dir-cfg.log"; fail=1; return
    fi
    if ! cmake --build "$dir" --target hft_tests -j"$(nproc)" > "$dir-build.log" 2>&1; then
        echo "[X] build failed — see $dir-build.log"; fail=1; return
    fi
    if ! (cd "$dir" && ctest --output-on-failure > "$dir-test.log" 2>&1); then
        echo "[X] tests failed — see $dir/$dir-test.log"; fail=1; return
    fi
    echo "[OK] $name: $(grep -oE '[0-9]+% tests passed' "$dir-test.log" | head -1)"
}

# 1. ASan + UBSan
run_flavor asan-ubsan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
# 2. UBSan only
run_flavor ubsan -DENABLE_ASAN=OFF -DENABLE_UBSAN=ON
# 3. Audit (instrumented, optimized — catches compiler-assumption races)
run_flavor_audit() {
    local dir="$BUILD/audit"
    echo "=================================================================="
    echo "[*] Flavor: Audit (HFT_AUDIT_MODE, -O2)"
    echo "=================================================================="
    if ! cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=Audit > "$dir-cfg.log" 2>&1; then
        echo "[X] configure failed"; fail=1; return
    fi
    if ! cmake --build "$dir" --target hft_tests -j"$(nproc)" > "$dir-build.log" 2>&1; then
        echo "[X] build failed"; fail=1; return
    fi
    if ! (cd "$dir" && ctest --output-on-failure > "$dir-test.log" 2>&1); then
        echo "[X] tests failed"; fail=1; return
    fi
    echo "[OK] audit: $(grep -oE '[0-9]+% tests passed' "$dir-test.log" | head -1)"
}
run_flavor_audit

# 4. Leak sanitizer (ASan's leak detection, full tests)
run_flavor lsan -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF -DCMAKE_CXX_FLAGS="-fsanitize=leak" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=leak"

echo "=================================================================="
[ $fail -eq 0 ] && echo "ALL SANITIZER FLAVORS PASSED" || echo "FAILURES DETECTED"
exit $fail
