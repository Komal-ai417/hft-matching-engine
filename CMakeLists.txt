cmake_minimum_required(VERSION 3.14)
project(HFT_MatchingEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ══════════════════════════════════════════════════════════════════
# Build flavors: Debug / Audit / Release
#
#   Debug   (-DCMAKE_BUILD_TYPE=Debug)  : -O0 -g, ASan+UBSan, full
#          instrumentation (HFT_DEBUG_TOOLS via !NDEBUG), exceptions on.
#   Audit   (-DCMAKE_BUILD_TYPE=Audit)  : -O2 -g -DNDEBUG -DHFT_AUDIT_MODE,
#          full instrumentation (canaries, journal, latches, validate_book),
#          exceptions on. The memory-safety verification gear.
#   Release (-DCMAKE_BUILD_TYPE=Release): -O3, -fno-exceptions -fno-rtti,
#          ZERO instrumentation (documented-trust fast path).
#
# Each flavor is internally consistent: the diagnostics that throw are only
# built where exceptions are enabled, and HFT_AUDIT_MODE is a real build
# type rather than dead configuration.
# ══════════════════════════════════════════════════════════════════
set(CMAKE_CXX_FLAGS_AUDIT "-O2 -g -DNDEBUG -DHFT_AUDIT_MODE -fno-omit-frame-pointer"
    CACHE STRING "Flags used by the C++ compiler during Audit builds." FORCE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)

    # Release: maximum optimization for HFT-grade performance.
    # Hardening: -fstack-protector-strong costs one guard
    # load/store per outlined function and inlined hot-path code pays nothing.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -fno-rtti -fno-exceptions -fmerge-all-constants -fno-math-errno -funroll-loops -falign-functions=32 -falign-loops=32 -ftree-vectorize -fstack-protector-strong")
    # _FORTIFY_SOURCE needs optimization to be effective; enable at -O2+.
    add_compile_definitions($<$<CONFIG:Release>:_FORTIFY_SOURCE=2>)

    # Debug: sanitizers (optional) + debug symbols.
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g -fno-omit-frame-pointer")

    # RelWithDebInfo: used for profiling (keeps frame pointers + symbols).
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O3 -fno-rtti -fno-exceptions -fmerge-all-constants -fno-math-errno -funroll-loops -falign-functions=32 -falign-loops=32 -g -fno-omit-frame-pointer")

    option(ENABLE_ASAN "Enable AddressSanitizer in Debug builds" ON)
    option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer in Debug builds" ON)
    if(ENABLE_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options($<$<CONFIG:Debug>:-fsanitize=address>)
        add_link_options($<$<CONFIG:Debug>:-fsanitize=address>)
    endif()
    if(ENABLE_UBSAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options($<$<CONFIG:Debug>:-fsanitize=undefined>)
        add_link_options($<$<CONFIG:Debug>:-fsanitize=undefined>)
    endif()

    # -march=native: great for local profiling, non-reproducible
    # for shipped artifacts. Option, default OFF; CI stays baseline.
    option(HFT_MARCH_NATIVE "Build with -march=native (non-portable binaries)" OFF)
    if(HFT_MARCH_NATIVE)
        add_compile_options($<$<CONFIG:Release,RelWithDebInfo,Audit>:-march=native>)
    endif()

    option(HFT_WERROR "Treat warnings as errors (CI)" OFF)
    if(HFT_WERROR)
        add_compile_options(-Werror)
    endif()

    option(HFT_ENABLE_COVERAGE "Build with --coverage (gcov) for branch-coverage runs" OFF)
    if(HFT_ENABLE_COVERAGE)
        add_compile_options(--coverage)
        add_link_options(--coverage)
    endif()
elseif(MSVC)
    # /GS (stack cookie) stays on — no benchmarked
    # reason to ship without stack protection.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Oi /GL /arch:AVX2 /GS")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /Od /Zi /fsanitize=address")
endif()

# ══════════════════════════════════════════════════════════════════
# Dependencies — pinned to commit SHAs for reproducible/offline builds
# (mutable tag floats were a supply-chain risk).
# ══════════════════════════════════════════════════════════════════
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        f8d7d77c06936315286eb55f8de22cd23c188571 # v1.14.0 (pinned)
)
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        344117638c8ff7e239044fd0fa7085839fc03021 # v1.8.3 (pinned)
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

# google benchmark is opt-in via HFT_BUILD_BENCHMARK, and additionally
# skipped for Debug builds (sanitizer-instrumented timings are meaningless).
#
# Under MSVC the default flips to OFF: benchmark v1.8.3 configures through
# try_run() feature checks (std/GNU/POSIX regex) whose check programs
# compile but FAIL TO RUN on the GitHub windows-2022 image ("compiled but
# failed to run"), so CMake configure aborts before a single target exists.
# A local MSVC build that wants benchmarks passes -DHFT_BUILD_BENCHMARK=ON.
if(NOT DEFINED HFT_BUILD_BENCHMARK AND MSVC)
    set(HFT_BUILD_BENCHMARK OFF)
endif()
option(HFT_BUILD_BENCHMARK
    "Fetch google benchmark and build hft_bench (auto-OFF under MSVC: its try_run() configure checks fail on CI images)" ON)

set(HFT_NEED_BENCHMARK OFF)
if(HFT_BUILD_BENCHMARK AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(HFT_NEED_BENCHMARK ON)
endif()

if(HFT_NEED_BENCHMARK)
    FetchContent_MakeAvailable(googletest googlebenchmark)
else()
    FetchContent_MakeAvailable(googletest)
endif()

# ══════════════════════════════════════════════════════════════════
# LTO / IPO — Release-like flavors only (a directory-wide setting would also
# LTO the sanitizer Debug build, slowing it for no benefit).
#
# Two silent-failure traps fixed here (both proven on cmake 3.28):
#
#   1. check_ipo_supported() without LANGUAGES checks every language in the
#      global ENABLED_LANGUAGES list — and by this point FetchContent
#      (googletest) has already enabled C as a side effect of its own
#      project() call. The spurious C leg then fails ("CMake doesn't support
#      IPO for current C compiler"), the whole check reports NO, and LTO
#      stays off in every Release / Audit / RelWithDebInfo build even though
#      the block above claims it is on. We are a CXX-only project:
#      pass LANGUAGES CXX, nothing else.
#
#   2. The INTERPROCEDURAL_OPTIMIZATION *directory property* no longer
#      exists ("This directory property does not exist anymore" —
#      cmake --help-property INTERPROCEDURAL_OPTIMIZATION), so the previous
#      set_property(DIRECTORY ...) was a silent no-op: even when the check
#      passed, it enabled LTO exactly nowhere. The supported mechanism is
#      the CMAKE_INTERPROCEDURAL_OPTIMIZATION *variable*, which initializes
#      the target property of every target created after it — i.e. ours,
#      not the already-created googletest/benchmark (no point LTO-ing
#      third-party code).
#
# HFT_DISABLE_IPO lets CI (and curious developers) measure what LTO is
# actually worth: the default build has it ON, and
# `cmake -DHFT_DISABLE_IPO=ON` produces the A/B comparison leg.
# ══════════════════════════════════════════════════════════════════
option(HFT_DISABLE_IPO
    "Force IPO/LTO off even when supported (A/B performance comparison leg)" OFF)

if(NOT MINGW AND NOT HFT_DISABLE_IPO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES CXX)
    if(ipo_supported)
        message(STATUS "IPO/LTO is supported and enabled for ${CMAKE_BUILD_TYPE}.")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(STATUS "IPO/LTO is not supported: ${ipo_error}")
    endif()
elseif(HFT_DISABLE_IPO)
    message(STATUS "IPO/LTO disabled by HFT_DISABLE_IPO (A/B comparison leg).")
else()
    message(STATUS "IPO/LTO skipped for ${CMAKE_BUILD_TYPE}.")
endif()

# ══════════════════════════════════════════════════════════════════
# Targets
# ══════════════════════════════════════════════════════════════════

# Main executable (CI smoke-runs it — a crash here must never ship green).
add_executable(hft_engine src/main.cpp src/OrderBook.cpp)

# Complete test suite: core cases (test_ob.cpp) + the memory-pool, price-level,
# correctness, differential-fuzz, debug-aids and reference suites + the
# README compile-guard.
enable_testing()
add_executable(hft_tests
    tests/test_ob.cpp
    tests/test_memory_pool.cpp
    tests/test_price_level.cpp
    tests/test_orderbook_correctness.cpp
    tests/test_differential_fuzz.cpp
    tests/test_debug_aids.cpp
    tests/test_std_reference.cpp
    tests/readme_example.cpp
    src/OrderBook.cpp
)
target_include_directories(hft_tests PRIVATE tests) # StdOrderBook.h
target_link_libraries(hft_tests GTest::gtest_main)
add_test(NAME hft_tests COMMAND hft_tests)

# Standalone RDTSC latency harness (no Google Benchmark dependency — runs
# anywhere, reports p50/p90/p99/p99.9/max cycle latencies and CPI estimate).
add_executable(hft_rdtsc_bench tools/rdtsc_bench.cpp src/OrderBook.cpp)

# Real-world simulation.
add_executable(hft_sim tests/real_world_sim.cpp src/OrderBook.cpp)
target_include_directories(hft_sim PRIVATE tests)
# 200k events by default in CTest smoke; pass HFT_SIM_EVENTS to override.

# Benchmarks: Release-like flavors only.
if(HFT_NEED_BENCHMARK)
    add_executable(hft_bench tests/benchmark_ob.cpp src/OrderBook.cpp)
    target_include_directories(hft_bench PRIVATE tests)
    target_link_libraries(hft_bench benchmark::benchmark)
endif()

# Optional continuous fuzzing target (clang + libFuzzer only).
option(HFT_ENABLE_FUZZING "Build the libFuzzer differential core (requires clang)" OFF)
if(HFT_ENABLE_FUZZING)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_executable(hft_fuzz tools/fuzz_driver.cpp src/OrderBook.cpp)
        target_include_directories(hft_fuzz PRIVATE tests src)
        target_compile_options(hft_fuzz PRIVATE -fsanitize=fuzzer-no-link)
        target_link_options(hft_fuzz PRIVATE -fsanitize=fuzzer)
    else()
        message(STATUS "HFT_ENABLE_FUZZING requires clang; skipping.")
    endif()
endif()

# Convenience: per-config message (plain text — a raw $<IN_LIST:...> generator
# expression here would print unexpanded).
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(HFT_FLAVOR_INSTRUMENTATION "off (documented trust)")
else()
    set(HFT_FLAVOR_INSTRUMENTATION "on (HFT_DEBUG_TOOLS — see Macros.h)")
endif()
message(STATUS "HFT build flavor: ${CMAKE_BUILD_TYPE} (instrumentation: ${HFT_FLAVOR_INSTRUMENTATION})")
