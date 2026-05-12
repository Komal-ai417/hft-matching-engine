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
  ./hft_tests
  ```
- **Run Benchmarks:**
  ```bash
  ./hft_bench
  ```
  Ensure benchmark metrics are consistent or improved compared to the `main` branch.

## Sanitizer Verification

Before submitting a Pull Request, you must verify your changes using memory and undefined behavior sanitizers.

1. **Configure Debug build with Sanitizers:**
   ```bash
   mkdir build-debug && cd build-debug
   cmake .. -DCMAKE_BUILD_TYPE=Debug -DUSE_SANITIZERS=ON
   ```
   *(Note: You may need to manually add `-fsanitize=address,undefined` to `CMAKE_CXX_FLAGS` if not defined in CMakeLists.txt)*

2. **Build and Run Tests with Sanitizers:**
   ```bash
   cmake --build . -j$(nproc)
   ./hft_tests
   ```
   The PR will only be accepted if ASAN/UBSAN reports 0 errors and 0 leaks.

## PR Process

1. **Branch Naming:**
   Use descriptive branch names, e.g., `feature/add-cancel-order`, `bugfix/fix-memory-leak`, `perf/optimize-matching-loop`.

2. **Commit Messages:**
   Write clear and concise commit messages. Describe *why* the change was made, not just *what* was changed.

3. **PR Template:**
   When opening a PR, carefully read and check off the items in the Pull Request Template. Ensure you have verified zero dynamic allocations on the critical matching path.
