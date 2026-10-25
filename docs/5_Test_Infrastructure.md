# 5. Test Infrastructure & Tooling

This document describes the build-flavor matrix, the debugging aids compiled
into each flavor, and every testing / performance tool shipped in the repo.
It replaces the informal notes that previously lived only in the README.

## 1. Build flavors

The library has three build gears. `HFT_DEBUG_TOOLS` (defined in
`src/Macros.h` as `!NDEBUG || HFT_AUDIT_MODE`) is the single switch that
governs every debugging aid.

| Flavor | Configure | Optimization | Instrumentation | Exceptions | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Debug** | `-DCMAKE_BUILD_TYPE=Debug` | `-O0 -g` | ASan + UBSan (on by default) + ALL aids | on | Correctness development; sanitizer runs |
| **Audit** | `-DCMAKE_BUILD_TYPE=Audit` | `-O2 -g` | ALL aids, NO sanitizers | on | Memory-safety verification on optimized code |
| **Release** | `-DCMAKE_BUILD_TYPE=Release` | `-O3` | NONE (documented trust) | off (`-fno-exceptions`) | Shipping binary; zero overhead |

### What the aids include (Debug + Audit only)

* **Slot state stamps** — every pool slot carries a `Live`/`Free` magic byte;
  `deallocate` verifies the stamp and faults (with slot index + pool
  statistics) on double-free / wild frees. Faults are exception-free
  (handler hook + deterministic abort), so behavior is identical in every
  flavor.
* **Poison patterns** — freed slots are filled with `0xDD` (past the 4-byte
  free-list link), freshly allocated slots with `0xDE`; stale reads pop
  immediately.
* **Generation stamps** — per-slot 8-bit allocation counters.
* **Allocation tracking** — alloc/free/live/high-water counters,
  `free_list_depth()` reconciliation, opt-in destruction-time leak report
  (`hft::set_pool_leak_report(true)`).
* **Order lifecycle journal** — fixed-size ring of 24-byte records for
  SUBMIT / REJECT / REST / TRADE / MAKER_FILLED / CANCEL / POOL_EXHAUSTED /
  BEST_BID / BEST_ASK events with RDTSC timestamps
  (`ob.journal().dump(stderr)`).
* **Re-entrancy latch** — nested `add_order`/`cancel_order` from inside a
  trade callback is blocked and reported.
* **Thread-identity latch** — public entry points assert the constructing
  thread.
* **`validate_book()`** — full cross-structural reconciliation: per-level
  totals and canaries, bidirectional link integrity, pool liveness, map
  back-references, bitset/summary parity, best-price extrema, and count
  agreement across `active_count_` / pool / map / level walks.

### Release contract

Pure Release compiles all of the above out. `MemoryPool::deallocate`
arguments are trusted (they always come from consistent book state inside
the engine); a double-free in Release silently corrupts the free list —
which is why CI runs the Audit flavor before any Release artifact ships.

## 2. Test suites (`hft_tests`, driven by ctest)

| File | Suites | What it covers |
| :--- | :--- | :--- |
| `tests/test_ob.cpp` | 33 | Matching, markets, cancels, validation, pool boundaries — assertions restored, 1M-cycle stress, repeated ID reuse |
| `tests/test_memory_pool.cpp` | 22 | Allocation order, exhaustion, LIFO, double-free/out-of-range/chain faults (incl. the out-of-range-tail reproducer), canaries, generations, stats, 10k-op shadow-model property test |
| `tests/test_price_level.cpp` | 11 | Direct coverage: append/remove semantics, links both directions, canary, 2k-op shadow-model property test |
| `tests/test_orderbook_correctness.cpp` | 24 | Nonzero `min_price`, cancel-best across gaps (both sides), queue-position cancels, multi-maker partial fills, wide-range summary scans (>4096 ticks), word-boundary straddles, pool-exhaustion invariants, market `filled_out` semantics, 5k-op reconciliation |
| `tests/test_differential_fuzz.cpp` | 7 configs | Engine vs `StdOrderBook` parity: reject codes, filled qty, full trade streams, resting counts, final-liquidity drains — with `validate_book()` every 128 ops |
| `tests/test_debug_aids.cpp` | 9 | Journal sequence fidelity (incl. the `JournalCancelEntriesRecordTrueSide` post-deallocation regression), both latches, fault-handler plumbing, validate_book across a 2k-op scenario matrix |
| `tests/test_std_reference.cpp` | 5 | The reference oracle itself: the `side_map_` leak sentinel, validation parity, market remainder, exhaustion |
| `tests/readme_example.cpp` | 1 | A compile-guard for the README API example: the same calls against the enforced noexcept-callback contract |

Counts: **112 tests in Debug/Audit**, **87 in Release** (debug-only cases
compile out via `#if HFT_DEBUG_TOOLS`).

## 3. Benchmarks

* `hft_bench` (Google Benchmark, Release-like flavors only) — 9 original
  scenarios + 6 new families: **Churn** (fragmented free list),
  **AllocationPath** (isolated alloc/dealloc), **BranchPredictability**
  (alternating cross/no-cross), **CacheLocality** (hot vs round-robin
  levels), **BookDepthScaling** (10/100/1k/10k levels),
  **WorkloadMix** (90/5, 50/25, 10/80). Every scenario runs against both the
  engine and the std::map reference.
* `hft_rdtsc_bench` (standalone, no deps) — RDTSC-calibrated per-op latency
  with p50/p90/p99/p99.9/max percentiles for insert / match / sweep /
  cancel / churn / mixed.
* `hft_engine` — the 4-workload standalone benchmark (mlockall-safe).
* `hft_sim` — 5M-event market-maker/retail/institutional simulation; event
  count is a CLI argument for CI smoke runs.

## 4. Script recipes (`scripts/`)

| Script | What it does |
| :--- | :--- |
| **`run_all.sh`** | **One-command end-to-end verification: 11 phases (env → build → tests → fuzz → valgrind → static → bench → rdtsc → sim → smoke → summary), 5 build flavors, per-phase logs + `summary.txt` into `verify-results/<UTC-timestamp>/`. `FAST=1` shortens fuzz/bench/sim; `JOBS=N` controls build parallelism. Exit code = number of failed phases.** |
| `run_sanitizers.sh` | Builds & runs the suite under ASan+UBSan, UBSan-only, LeakSanitizer and Audit flavors |
| `run_static.sh` | Header self-containment, zero-warning check, cppcheck (`--language=c++`) + strict clang-tidy gate (both compile shapes) |
| `run_valgrind.sh` | Valgrind Memcheck over the Release suite (one clean run per release) |
| `perf.sh` | `perf stat` hardware counters + `perf record` call-graph + FlameGraph (when tools present) |
| `fuzz.sh` | libFuzzer differential core (clang) or g++ seed-sweep mode |
| `bench_matrix.sh` | Compiler × flavor benchmark matrix into `bench-results/` |

### 4b. The smoke matrix (run_all.sh phase 10)

The engine binary is executed four times, each proving a different
memory-locking path — a startup regression in any of them fails the
pipeline:

| Check | Environment | Required evidence |
| :--- | :--- | :--- |
| `engine-default` | — | runs to completion (locked, or pageable with a warning on hosts that deny `mlockall`) |
| `engine-nolock` | `HFT_DISABLE_MLOCKALL=1` | `mlockall skipped` recap |
| `engine-madviseonly` | `HFT_MADVISE_ONLY=1` | madvise-only recap (pre-touch without locking) |
| `engine-gvisorsim` | `LD_PRELOAD=mlock_gvisor_sim.so` | **self-heal**: `reverting to pageable memory` + runs to completion |

`tools/mlock_gvisor_sim.c` is an `LD_PRELOAD` runtime that reproduces the
sandbox failure mode — `mlockall()` reports success but every future
locked mapping fails with `ENOMEM`. Left unprobed, this kills the engine
mid-startup (`std::bad_alloc` → terminate under `-fno-exceptions`); the
engine probes one page right after locking and self-heals to pageable
memory, which is exactly what this smoke asserts.

### 4c. The static gate

`run_static.sh` enforces a **strict** clang-tidy gate: `.clang-tidy` carries
`WarningsAsErrors: '*'` — every enabled check is a hard error. The handful
of excluded checks (`macro-usage`, `easily-swappable-parameters`, bounds/
vararg families) are documented architectural decisions, listed with
rationale inside `.clang-tidy` itself. The tree is clean in BOTH compile
shapes (Debug instrumentation and Release), and `cppcheck` runs with
`--language=c++` — without the flag some cppcheck versions guess "C" for
plain-looking headers and dies on `namespace hft`.

## 5. CI (`.github/workflows/ci.yml`)

A layered, ten-job pipeline. The design contract: every analysis tool
runs in exactly ONE job (the run-once dedup contract), `correctness`
gates the deep legs which run in parallel behind it, and static analysis /
CodeQL / the MSVC probe start immediately. Supply-chain invariants:
actions pinned to full commit SHAs, `permissions: contents: read`
(`security-events: write` for the CodeQL job only), a per-job timeout, and
concurrency cancellation of superseded runs.

| Job | What it proves |
| :--- | :--- |
| `correctness` (matrix) | gcc-13 / clang-18 × Debug / Release / **RelWithDebInfo**, plus one include cell for clang × **Audit** — all `-Werror`, sanitizers OFF (they live in their own job, so the matrix cells stay pure portability legs). One ctest run covers the functional, invariant and differential suites; every cell additionally smoke-runs `hft_engine` and a 200k-event `hft_sim` (a startup crash can never ship green — that is how the mlockall `std::bad_alloc` bug once survived). |
| `correctness-msvc` | Non-blocking portability probe (VS 2022, `continue-on-error`). Release leg; Debug leg built with `/fsanitize=address` — the MSVC ASan and Debug-CRT DLLs (`clang_rt.asan_dynamic-x86_64.dll`, the `vcruntime140d.dll` family, `ucrtbased.dll`) are copied next to the test exe before ctest, because the toolset's bin dir is not on ctest's PATH and the Windows loader searches the exe's own folder first (without this the exe dies in the loader with `0xc0000135`). |
| `static-analysis` | clang-format gate (`--dry-run --Werror` over `src/`, `tests/`, `tools/`), strict clang-tidy (`.clang-tidy` sets `WarningsAsErrors: '*'`), Clang Static Analyzer (`scan-build`, with a per-TU `clang --analyze` fallback), cppcheck (`--enable=warning,portability,performance`, zero findings, `--error-exitcode=1`). |
| `codeql` | CodeQL C/C++ security scan (default suite `security-extended`), SARIF uploaded to the repo's Code scanning tab. |
| `fuzz` | Rotating-seed differential sweeps + the libFuzzer differential core (ASan+UBSan; budget 60 s per push/PR, 900 s nightly; the corpus is cached across runs so coverage compounds) + a best-effort AFL++ leg; crash artifacts are uploaded on any outcome. |
| `sanitizers` | Build A: Debug + ASan + UBSan, leak check at exit; Build B: standalone LSan. No ASan suppression is set anywhere — the engine is exception-free, so the libc++ exception-string path that originally tripped the `alloc_dealloc_mismatch` check is never reached; the job's `ASAN_OPTIONS`/`UBSAN_OPTIONS` only *raise* strictness (`halt_on_error`, `strict_string_checks`, ...). |
| `audit-mode` | The Audit flavor under gcc, run once (the clang × Audit cell lives in the matrix above): full suite + engine smoke under the complete instrumentation gear at `-O2`. |
|| `valgrind` | Valgrind Memcheck (no compiler runtime involved) over the RelWithDebInfo test suite + engine, plus the 4-path engine smoke matrix (default / nolock / madvise-only / sandbox sim) and the 200k-event simulation. |
| `performance` | Google Benchmark (5 repetitions) + the RDTSC harness, cold- vs hot-cache, `perf stat/record/annotate`, FlameGraph, Cachegrind, Callgrind, assembly/vectorization inspection, an allocation-path proof (`LD_PRELOAD` malloc counter), and a benchmark regression check against a cached baseline (95% CI; baselines are recorded on `main` only). `perf` availability is probed first: on runners where `perf_event_paranoid` blocks counters, the perf-dependent steps degrade to logged warnings instead of failing the job. |
| `performance-lab` | Non-blocking deep-comparison lab (`continue-on-error`, gated by the `deep_perf` dispatch input): compiler / `-O` level / LTO on-off / `-march` / PGO / binary-size A/B legs. A red or timed-out lab never fails the pipeline. |

The pipeline triggers on push/PR to `main`, on a nightly schedule (02:30
UTC — deep-fuzz budget + performance trending), and via `workflow_dispatch`
with tunables (fuzz seconds, sweep iterations, `deep_perf`, baseline
refresh, CodeQL suite).

## 6. Fuzzing

`tools/fuzz_driver.cpp` is a libFuzzer entry point around the differential
core: input bytes become deterministic op scripts, and any engine/reference
divergence traps with the offending input preserved. Build with:

```bash
scripts/fuzz.sh libfuzzer 300   # 5 minutes of continuous fuzzing
scripts/fuzz.sh seeds 20        # g++ fallback: 20 seed-sweep iterations
```

## 7. Coverage

```bash
cmake -B build-cov -DCMAKE_BUILD_TYPE=Debug -DHFT_ENABLE_COVERAGE=ON
cmake --build build-cov --target hft_tests
cd build-cov && ./hft_tests && gcovr -r .. --branches   # branch coverage
```

Track branch coverage specifically on `match_order_direct`,
`update_best_*_after_remove` and `add_order` — the differential suite drives
all of them, but the metric should be watched.
