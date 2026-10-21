# Edge Case & Correctness Matrix

Financial matching engines are zero-tolerance systems regarding mathematical correctness and memory safety. The engine is safeguarded by 30 formalized Google Test cases split into the following domains.

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

- **`AllocDeallocCycleStress`**: Exercises `MemoryPool` integrity with 1,000,000 insert+cancel cycles against a 10-slot pool, then asserts `active_order_count() == 0`, `price_level_count() == 0`, ID non-existence and that the book still accepts new orders. (Audit C-3a: the original test looped 100 times with zero assertions — the doc's claim of "1,000,000+ combinations with capacity assertions" was aspirational. The test now matches the claim; see `tests/test_ob.cpp`.)
- **`ExhaustPoolSilentDrop`**: Evaluates exhaustion handling. Upon reaching maximum active orders, asserts that new orders are safely and silently dropped without exceptions (`-fno-exceptions` safe), catching runaway Gateway feeds.

### The `libc++` Mismatch Resolution

During rigorous Clang 18 testing, the AddressSanitizer emitted an `alloc-dealloc-mismatch` false positive relating to standard library exception generation (`std::logic_error` string allocations inside the LLVM libc++ layer). 
- **The Engine Fix:** We converted the memory stress test to a controlled try-catch validation block, avoiding GTest macro exception-capturing anomalies. 
- **The CI Fix:** We integrated `ASAN_OPTIONS=alloc_dealloc_mismatch=0` into our test suites to suppress this known LLVM/Linux exception-string false-positive, maintaining absolute integrity of our core algorithmic heap checks. 

**Conclusion (scoped per build flavor):** Debug and Audit builds guarantee double-free and wild-free detection via slot state stamps (`MemoryPool` faults), bounds-check all physical inputs, and verify internal consistency via `validate_book()`. Pure Release builds trust caller invariants by design (zero-overhead contract) — run the Audit flavor in CI before shipping a Release binary. AddressSanitizer/LeakSanitizer runs over the full 111-test suite report zero leaks and zero memory errors (see `verification/ALL_TEST_RESULTS.txt`).
