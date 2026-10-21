#!/usr/bin/env bash
# ============================================================================
# run_static.sh — static analysis sweep (audit L-1 / M-9a / H-4b / §8.8).
# Degrades gracefully when a tool is missing. Checks:
#   1. header self-containment (every header must compile alone)
#   2. zero-warning build (-Wall -Wextra -Wpedantic, Release + Debug shapes)
#   3. cppcheck (--language=c++ — Round-5: without the flag cppcheck 2.13
#      guesses "C" for plain-looking headers such as Macros.h/Order.h and
#      dies with "Code 'namespacehft{' is invalid C code" [syntaxError])
#   4. clang-tidy (strict gate: WarningsAsErrors comes from .clang-tidy;
#      run in BOTH the debug and the release shape — the HFT_JOURNAL macro
#      and the latches compile to different code in each)
# ============================================================================
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0

echo "[*] 1/4 Header self-containment (audit L-1)"
for h in "$ROOT"/src/*.h; do
    base="$(basename "$h")"
    printf '#include "%s"\nint main() { return 0; }\n' "$base" > /tmp/hft_hdr_check.cpp
    if g++ -std=c++20 -I"$ROOT/src" -fsyntax-only /tmp/hft_hdr_check.cpp 2>/tmp/hft_hdr_err.log; then
        echo "[OK] $base"
    else
        echo "[X] $base is not self-contained:"
        sed 's/^/    /' /tmp/hft_hdr_err.log
        fail=1
    fi
done

echo "[*] 2/4 Zero-warning build (-Wall -Wextra -Wpedantic, audit M-9a)"
printf '#include "src/OrderBook.h"\nint main() { return 0; }\n' > /tmp/hft_warn_check.cpp
: > /tmp/hft_warn.log
if g++ -std=c++20 -Wall -Wextra -Wpedantic -O2 -DNDEBUG -I"$ROOT" -fsyntax-only /tmp/hft_warn_check.cpp 2>>/tmp/hft_warn.log \
&& g++ -std=c++20 -Wall -Wextra -Wpedantic -O0 -g -I"$ROOT" -fsyntax-only /tmp/hft_warn_check.cpp 2>>/tmp/hft_warn.log; then
    if [ -s /tmp/hft_warn.log ]; then
        echo "[X] warnings present:"
        sed 's/^/    /' /tmp/hft_warn.log
        fail=1
    else
        echo "[OK] zero warnings in Release and Debug shapes"
    fi
else
    echo "[X] compile error:"
    sed 's/^/    /' /tmp/hft_warn.log
    fail=1
fi

echo "[*] 3/4 cppcheck"
if command -v cppcheck >/dev/null 2>&1; then
    # --language=c++ is REQUIRED (Round-5): without it cppcheck parses
    # .h files by heuristic, treats Macros.h/Order.h as C, and aborts on
    # `namespace hft {` with a syntaxError. The flag is a no-op on hosts
    # whose cppcheck guesses correctly.
    if cppcheck --enable=warning,portability,performance --std=c++20 --language=c++ \
        --inline-suppr -I"$ROOT/src" "$ROOT"/src/*.cpp "$ROOT"/src/*.h 2>/tmp/hft_cppcheck.log >/dev/null; then
        echo "[OK] cppcheck clean"
    else
        echo "[X] cppcheck findings:"; sed 's/^/    /' /tmp/hft_cppcheck.log; fail=1
    fi
else
    echo "[skip] cppcheck not installed on this host"
fi

echo "[*] 4/4 clang-tidy (strict gate: WarningsAsErrors '*' from .clang-tidy)"
if command -v clang-tidy >/dev/null 2>&1; then
    for shape in debug release; do
        shape_flags=""
        [ "$shape" = "release" ] && shape_flags="-DNDEBUG"
        if clang-tidy "$ROOT"/src/OrderBook.cpp "$ROOT"/src/main.cpp -- -std=c++20 $shape_flags -I"$ROOT"/src > /tmp/hft_tidy.log 2>&1; then
            echo "[OK] clang-tidy clean ($shape shape)"
        else
            echo "[X] clang-tidy findings ($shape shape):"
            grep -E 'error:|warning:' /tmp/hft_tidy.log | head -60 | sed 's/^/    /'
            echo "    ($(grep -cE 'error:|warning:' /tmp/hft_tidy.log) finding(s) total; full list in /tmp/hft_tidy.log)"
            fail=1
        fi
    done
else
    echo "[skip] clang-tidy not installed on this host"
fi

exit $fail
