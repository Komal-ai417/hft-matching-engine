# Contributing to HFT Matching Engine

Thank you for your interest in contributing to the HFT Matching Engine! This document outlines the process and standards for contributing to ensure code quality and system performance.

## Local Setup

To set up the project locally for development:

1. **Clone the repository:**
   ```bash
   git clone https://github.com/Komal-ai417/hft-matching-engine.git
   cd hft-matching-engine
   ```

2. **Configure the CMake build:**
   ```bash
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   ```

3. **Build the project:**
   ```bash
   cmake --build . -j$(nproc)
   ```

## Testing Mandate

Performance and correctness are critical. All Pull Requests **must** pass the entire test suite and verify there is no latency degradation.

- **Run Unit Tests:**
  ```bash
  cmake --build build -j$(nproc)
  ctest --test-dir build --output-on-failure
  ```
  Debug/Audit builds register **112 tests**; Release registers **87**
  (debug-only cases compile out via `#if HFT_DEBUG_TOOLS`).
- **Run Benchmarks:**
  ```bash
  ./build/hft_bench
  ```
  Ensure benchmark metrics are consistent or improved compared to the `main` branch — CI additionally checks a cached 95%-confidence baseline on every run.
- **One-command local verification** (everything CI runs, and more — five
  build flavors, differential-fuzz sweeps, libFuzzer, Valgrind, the strict
  static gate, the benchmark suite, the RDTSC harness, the simulation and
  the 4-path engine smoke matrix):
  ```bash
  ./scripts/run_all.sh            # full run (~10-20 min depending on host)
  FAST=1 ./scripts/run_all.sh     # quick mode (shorter fuzz/bench/sim)
  cat verify-results/*/summary.txt
  ```

## Sanitizer Verification

Before submitting a Pull Request, you must verify your changes using memory and undefined behavior sanitizers.

1. **Configure a Debug build — sanitizers are ON by default in Debug:**
   ```bash
   cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
   ```
   AddressSanitizer and UndefinedBehaviorSanitizer are enabled by default
   in the Debug flavor. Disable either explicitly with
   `-DENABLE_ASAN=OFF` / `-DENABLE_UBSAN=OFF` — that is exactly how the CI
   `correctness` matrix keeps its cells sanitizer-free.

2. **Build and Run Tests with Sanitizers:**
   ```bash
   cmake --build build-debug -j$(nproc)
   ctest --test-dir build-debug --output-on-failure
   ```
   The PR will only be accepted if ASAN/UBSAN reports 0 errors and 0 leaks.

## Static-Analysis Gate

The tree must stay clean under the same tools CI enforces — all of them
are run locally by `scripts/run_static.sh`:

- `clang-format` — checked with `--dry-run --Werror` over `src/`, `tests/`
  and `tools/`. Run `clang-format -i` on everything you touch before
  committing.
- `clang-tidy` — strict gate: `.clang-tidy` sets `WarningsAsErrors: '*'`.
- `cppcheck` — `--enable=warning,portability,performance` with
  `--error-exitcode=1`; zero findings required.

## PR Process

1. **Branch Naming:**
   Use descriptive branch names, e.g., `feature/add-cancel-order`, `bugfix/fix-memory-leak`, `perf/optimize-matching-loop`.

2. **Commit Messages:**
   Write clear and concise commit messages. Describe *why* the change was made, not just *what* was changed.

3. **CI:** every PR runs the ten-job pipeline described in
   [docs/5_Test_Infrastructure.md](docs/5_Test_Infrastructure.md) §5 —
   the compiler-portability matrix (gcc/clang × Debug/Release/RelWithDebInfo
   + clang × Audit), the non-blocking MSVC portability probe, static
   analysis (format/tidy/analyzer/cppcheck), CodeQL, fuzz, sanitizers,
   audit mode, Valgrind and the performance legs. The MSVC probe is
   `continue-on-error`: it surfaces Windows portability findings without
   blocking the merge.

4. **PR Template:**
   When opening a PR, carefully read and check off the items in the Pull Request Template. Ensure you have verified zero dynamic allocations on the critical matching path.
