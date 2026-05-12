## Pull Request Description
Please provide a clear and concise description of the changes you are proposing.

## Self-Verification Checklist
Before requesting a review, please confirm the following:

- [ ] I have run `./hft_tests` and all tests pass.
- [ ] I have run the Debug build and AddressSanitizer (ASAN)/UBSAN reports 0 leaks/errors.
- [ ] I have run `./hft_bench` and verified no latency regressions compared to the `main` branch.
- [ ] My code introduces zero dynamic allocations (`new`/`malloc`) on the critical matching path.

## Additional Context
Add any other context or screenshots about the pull request here. (e.g., Benchmark output comparisons).
