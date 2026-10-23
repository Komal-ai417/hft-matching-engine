# Edge Case & Correctness Matrix

Financial matching engines are zero-tolerance systems regarding mathematical correctness and memory safety. The engine is safeguarded by **112 Google Test cases** (87 in Release, where the debug-only cases compile out) split into the following domains.

## 1. Priority & Market Integrity Logic

The core matching algorithm guarantees Price-Time priority. 

| Feature Tested | GTest Case Identifier | Objective & Verification |
| :--- | :--- | :--- |
| **Simple Cross** | `SingleMatch` | Validates that a 1:1 taker/maker cross results in a single Trade output at the correct price. |
| **Price/Time Priority** | `PriceTimePriority` | Submits 3 passive sells at different prices, and 1 passive sell at identical price but later timestamp. Asserts that the aggressive buy sweeps the lowest price first, and identical prices in FIFO time-order. |
| **Multiple Sweeps** | `SweepAcrossMultipleLevels` | Submits a massive taker order that consumes resting liquidity across multiple disjointed price levels. Validates Bitset fast-forwarding logic. |
| **Market Orders** | `MarketBuyOrder`, `MarketSellOrder` | Submits orders without price constraints (`OrderType::Market`). Verifies execution at Best Bid/Ask, and asserts that any un-filled remaining quantity is instantly cancelled (Market orders never rest). |

## 2. Boundary & Invalid Inputs

An HFT Gateway will occasionally feed malformed packets to the engine. The engine uses a `[[nodiscard]] RejectReason` return type to bounce these synchronously.

| Attack Vector | GTest Case Identifier | Handled State |
| :--- | :--- | :--- |
| **Zero Quantity** | `ZeroQuantityRejected` | Instant return `RejectReason::InvalidQuantity`. |
| **Duplicate ID** | `DuplicateOrderIdRejected` | O(1) lookup against `order_map_`. If pointer is active, returns `RejectReason::DuplicateOrderId`. |
| **Out-of-bounds ID** | `OutOfRangeOrderIdRejected` | Protects the array index bound. Bounces with `RejectReason::OutOfBoundsOrderId`. |
| **OOB Price bounds** | `OutOfRangePriceRejected` | Evaluates $Price < Min \vert\vert Price > Max$. Bounces with `RejectReason::OutOfBoundsPrice` to prevent memory corruption in the flat price arrays. |
| **Phantom Cancels** | `CancelNonExistentOrder` | Cancelling an ID that is not currently resting returns `RejectReason::CancelFailed` seamlessly. |

## 3. Memory Safety & CI Defenses

Because the engine manages custom memory layouts, the CI pipeline integrates LLVM AddressSanitizer (ASAN) and UndefinedBehaviorSanitizer (UBSAN) into the Debug builds.

### Stress Testing

- **`AllocDeallocCycleStress`**: Exercises `MemoryPool` integrity with 1,000,000 insert+cancel cycles against a 10-slot pool, then asserts `active_order_count() == 0`, `price_level_count(Side::Sell) == 0`, ID non-existence and that the book still accepts new orders (see `tests/test_ob.cpp`).
- **`ExhaustPoolSilentDrop`**: Evaluates exhaustion handling. When the pool is full, the order is rejected with `RejectReason::PoolExhausted` — no exception, no allocation, and no residue left in the book.

### The `libc++` Mismatch Resolution

During rigorous Clang 18 testing, the AddressSanitizer emitted an
`alloc-dealloc-mismatch` false positive originating in the standard
library's exception machinery (`std::logic_error` string allocations
inside the LLVM libc++ layer), not in the engine.
- **The Engine side:** the engine and its pool are exception-free by
  design — no `throw`, no `try`/`catch`, no `std::logic_error` anywhere
  in `src/` or `tests/`. Faults are reported through a settable fault
  handler that prints a diagnostic and aborts deterministically, so the
  same code path behaves identically under Release's `-fno-exceptions`
  and does not trip the GTest exception-capturing macros. The libc++
  exception-string path that triggered the false positive is therefore
  never reached.
- **The CI side:** the `sanitizers` job runs the suite twice — Debug +
  ASan + UBSan (leak check at exit, `halt_on_error=1`) and a standalone
  LeakSanitizer build. No ASan suppression is set anywhere in the workflow;
  the job's `ASAN_OPTIONS`/`UBSAN_OPTIONS` only *raise* strictness
  (`strict_string_checks`, `detect_stack_use_after_return`, ...).

### Regression Tests (post-incident)

- **`JournalCancelEntriesRecordTrueSide`** (`tests/test_debug_aids.cpp`):
  `cancel_order_impl` used to journal the order's `side` *after* the pool
  slot was deallocated and poisoned, so Debug/Audit journal records showed
  `side = 0xDD` for every cancel — a use-after-free read that falsified
  the audit trail. The fix snapshots the side before the slot is freed;
  this test replays cancel traffic and asserts every journal record carries
  the order's true side.

**Conclusion (scoped per build flavor):** Debug and Audit builds guarantee double-free and wild-free detection via slot state stamps (`MemoryPool` faults), bounds-check all physical inputs, and verify internal consistency via `validate_book()`. Pure Release builds trust caller invariants by design (zero-overhead contract) — run the Audit flavor in CI before shipping a Release binary. AddressSanitizer/LeakSanitizer runs over the full 112-test suite report zero leaks and zero memory errors; `scripts/run_all.sh` writes the per-phase evidence (including the sanitizer legs) to `verify-results/<UTC-timestamp>/`, with a `summary.txt` roll-up.
