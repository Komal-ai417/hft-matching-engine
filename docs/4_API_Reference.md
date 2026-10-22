# API Reference

> Generated manually from source headers: `Order.h`, `PriceLevel.h`, `MemoryPool.h`, `OrderBook.h`
>
> **C++ Standard:** C++20 &nbsp;|&nbsp; **Namespace:** `hft`

---

## Table of Contents

1. [Type Aliases](#1-type-aliases)
2. [Constants](#2-constants)
3. [Enumerations](#3-enumerations)
4. [Structs](#4-structs)
5. [Classes](#5-classes)
6. [Macros](#6-macros)
7. [Thread Safety](#7-thread-safety-guarantees)

---

## 1. Type Aliases

Defined in `Order.h`.

| Alias | Underlying Type | Description |
| :--- | :--- | :--- |
| `OrderId` | `uint32_t` | Unique identifier for each order. Must be in range `[1, max_order_id]`. |
| `Price` | `uint32_t` | Fixed-point price representation (e.g., $1.50 → `15000`). Avoids floating-point indeterminism. |
| `Quantity` | `uint32_t` | Order quantity. Must be `> 0` for Limit/Market orders. |

---

## 2. Constants

Defined in `Order.h`.

| Constant | Type | Value | Description |
| :--- | :--- | :--- | :--- |
| `MARKET_BUY_PRICE` | `Price` | `std::numeric_limits<Price>::max()` | Sentinel price assigned to Market Buy orders, ensuring they cross any resting Ask. |
| `MARKET_SELL_PRICE` | `Price` | `0` | Sentinel price assigned to Market Sell orders, ensuring they cross any resting Bid. |

---

## 3. Enumerations

### `enum class Side : uint8_t`

Defined in `Order.h`. Represents the direction of an order.

| Enumerator | Value | Description |
| :--- | :--- | :--- |
| `Buy` | `0` | Order to purchase (crosses against resting Asks). |
| `Sell` | `1` | Order to sell (crosses against resting Bids). |

### `enum class OrderType : uint8_t`

Defined in `Order.h`. Specifies the behavior of the order.

| Enumerator | Value | Description |
| :--- | :--- | :--- |
| `Limit` | `0` | Matches at the specified price or better. If unfilled, rests in the book. |
| `Market` | `1` | Matches immediately at the best available price. Unfilled remainder is discarded (never rests). |
| `Cancel` | `2` | Cancels an existing resting order by `OrderId`. |

### `enum class RejectReason : uint8_t`

Defined in `OrderBook.h`. Returned by `add_order` / `cancel_order`
(`[[nodiscard]]`). Every validation failure maps to exactly one value.

| Enumerator | Value | Trigger Condition |
| :--- | :--- | :--- |
| `Accepted` | `0` | Operation succeeded (fully or partially). |
| `InvalidQuantity` | `1` | `quantity == 0` for Limit or Market orders. |
| `DuplicateOrderId` | `2` | `order_map_[id]` is not `INVALID_INDEX` (already contains an active index). |
| `OutOfBoundsOrderId` | `3` | `id > max_order_id` or `id == 0`. |
| `OutOfBoundsPrice` | `4` | `price < min_price` or `price > max_price` for Limit orders. |
| `CancelFailed` | `5` | `order_map_[id] == INVALID_INDEX` (order does not exist or was already filled). |
| `PoolExhausted` | `6` | Pool exhausted, order cannot be posted. |
| `MarketOnEmptyBook` | `7` | Market order against an empty opposite side (the quantity would otherwise be silently discarded). |
| `ReentrancyBlocked` | `8` | Debug/Audit latch (re-entrancy or foreign-thread call) blocked the operation. |

---

## 4. Structs

### `struct Order`

Defined in `Order.h`. Represents a single order in the Limit Order Book. Kept incredibly lean at 24 bytes to allow tight cache packing.

| Field | Type | Description |
| :--- | :--- | :--- |
| `next` | `uint32_t` | Index of the next order in the `PriceLevel` queue. |
| `prev` | `uint32_t` | Index of the previous order in the `PriceLevel` queue. |
| `id` | `OrderId` | Unique order identifier. |
| `price` | `Price` | Order price (or sentinel for Market orders). |
| `quantity` | `Quantity` | Remaining quantity (decremented during partial fills). |
| `side` | `Side` | Buy or Sell. |

**Memory Layout (24 bytes):**

```
Offset  Field       Size
------  ----------  ----
0x00    next        4 bytes
0x04    prev        4 bytes
0x08    id          4 bytes
0x0C    price       4 bytes
0x10    quantity    4 bytes
0x14    side        1 byte
0x15    (padding)   3 bytes
```

### `struct Trade`

Defined in `OrderBook.h`. Emitted via callback for every fill event during matching.

| Field | Type | Description |
| :--- | :--- | :--- |
| `maker_id` | `OrderId` | The `OrderId` of the resting (passive) order that was filled. |
| `taker_id` | `OrderId` | The `OrderId` of the incoming (aggressive) order that crossed the spread. |
| `price` | `Price` | The execution price (always the maker's price, per Price-Time priority). |
| `quantity` | `Quantity` | The quantity executed in this fill. |

### `struct PriceLevel`

Defined in `PriceLevel.h`. An aggregate of all resting orders at a specific price tick.

| Field | Type | Description |
| :--- | :--- | :--- |
| `head` | `uint32_t` | Index of the oldest (first-in) order. |
| `tail` | `uint32_t` | Index of the newest (last-in) order. |
| `total_quantity` | `uint64_t` | Sum of all resting order quantities at this level. |
| `order_count` | `uint32_t` | Number of orders currently queued. |
| `debug_canary` | `uint32_t` | Magic value in the last named member (offset 0x14); verified by `validate_book()` in Debug/Audit. Catches whole-array memset-style corruption that would zero it. |

**Note:** The `price` is implicit from the level's index in the `bids_levels_` or `asks_levels_` vector.

**Layout:** exactly 32 bytes (`alignas(32)`), so two levels fit in one 64-byte cache line without straddling.

#### Methods

| Signature | Complexity | Description |
| :--- | :--- | :--- |
| `void append_order(uint32_t order_idx, Order* pool_data)` | $O(1)$ | Appends an order to the tail of the intrusive linked list. |
| `void remove_order(uint32_t order_idx, Order* pool_data)` | $O(1)$ | Detaches an order from the linked list by re-wiring `prev`/`next` indices. |
| `bool is_empty() const noexcept` | $O(1)$ | Returns `true` if `head == INVALID_INDEX`. |
| `bool canary_intact() const noexcept` | $O(1)$ | Returns `true` if the debug canary is undamaged. |

---

## 5. Classes

### `class OrderBook`

Defined in `OrderBook.h` and `OrderBook.cpp`. The core matching engine.

#### Constructor

```cpp
explicit OrderBook(size_t max_order_id, size_t max_active_orders, Price min_price, Price max_price);
```

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `max_order_id` | `size_t` | Maximum order identifier limit (`id` must be in `[1, max_order_id]`). Pre-allocates `order_map_`. |
| `max_active_orders`| `size_t` | Maximum number of concurrent resting orders. Pre-allocates the `MemoryPool`. |
| `min_price` | `Price` | Minimum valid tick price (inclusive). |
| `max_price` | `Price` | Maximum valid tick price (inclusive). |

**Side Effects:** Allocates `(max_price - min_price + 1)` `PriceLevel` structs for both bids and asks. Initializes bitset arrays for hardware-accelerated price discovery. Invalid arguments (an inverted price range, a `max_price` that saturates the `Price` type, or a price range beyond the `2^26`-tick sanity cap) abort with a diagnostic on stderr — in every build flavor, never an exception.

#### `add_order`

```cpp
template <typename TradeCallback>
[[nodiscard]] HFT_ENGINE_INLINE RejectReason add_order(
    OrderId id,
    OrderType type,
    Price price,
    Quantity quantity,
    Side side,
    TradeCallback&& on_trade,
    Quantity* filled_out = nullptr
);
```

**The primary entry point for all order operations.**

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `id` | `OrderId` | Unique order ID. Must be in range `[1, max_order_id]`. |
| `type` | `OrderType` | `Limit`, `Market`, or `Cancel`. |
| `price` | `Price` | The limit price. Ignored for `Market` and `Cancel` types. |
| `quantity` | `Quantity` | Order size. Must be `> 0`. Ignored for `Cancel`. |
| `side` | `Side` | `Buy` or `Sell`. For `Cancel`, this parameter is ignored. |
| `on_trade` | `TradeCallback&&` | A callable with signature `void(const Trade&) noexcept`. Invoked synchronously for each fill. **WARNING:** This callback MUST NOT be re-entrant and MUST be marked `noexcept`. Throwing corrupts the order book state. |
| `filled_out` | `Quantity*` | Optional. When non-null, receives the quantity that actually traded — so a partial fill is distinguishable from a full fill without summing the callbacks. Market-order remainders are discarded (IOC) and are *not* included in this figure. |

**Returns:** `RejectReason`

Invalid inputs (zero quantity, duplicate IDs, out-of-bounds prices/IDs) return the respective `RejectReason`. Success returns `RejectReason::Accepted`.

**Example:**

```cpp
ob.add_order(
    42, hft::OrderType::Limit, 10050, 100, hft::Side::Buy,
    [](const hft::Trade& t) noexcept {
        std::cout << t.quantity << " filled @ " << t.price << "\n";
    }
);
```

#### `cancel_order`

```cpp
[[nodiscard]] inline RejectReason cancel_order(OrderId id);
```

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `id` | `OrderId` | The ID of the order to cancel. |

**Returns:** `RejectReason`

If the `id` is out of bounds or no active order exists with this ID, the function returns `RejectReason::OutOfBoundsOrderId` or `RejectReason::CancelFailed` without side effects. Otherwise returns `RejectReason::Accepted`.

**Complexity:** $O(1)$ — direct array lookup + intrusive list unlink.

---

### `class MemoryPool<T>`

Defined in `MemoryPool.h`. A pre-allocated, zero-dynamic-allocation object pool.

#### Constructor

```cpp
explicit MemoryPool(size_t max_capacity);
```

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `max_capacity` | `size_t` | Number of `T` objects to pre-allocate. |

**Side Effects:** Allocates a contiguous `std::vector<T>` of size `max_capacity` and links all slots into an intrusive LIFO free-list.

#### `allocate`

```cpp
uint32_t allocate();
```

Pops the top of the free-list stack and returns an index to the slot.

| Return | Description |
| :--- | :--- |
| `uint32_t` | Index to a pool-managed object. `INVALID_INDEX` if exhausted. |

**Throws:** Nothing. Returns `INVALID_INDEX` when the pool is exhausted;
the engine turns that into `RejectReason::PoolExhausted` for the order
rather than dropping it silently.

**Complexity:** $O(1)$.

#### `deallocate`

```cpp
void deallocate(uint32_t idx);
```

Returns a previously allocated object index back to the free-list.

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `idx` | `uint32_t` | Must be an index previously returned by `allocate()`. |

**Throws:** Nothing — never, in any build flavor. The pool is
exception-free by design so it behaves identically under Release's
`-fno-exceptions`.

- **Debug / Audit:** the index is range-checked and the slot's state
  stamp is verified. A double free, a wild free or an out-of-range
  index invokes the installed `PoolFaultHandler`; with no handler
  installed (the default) the pool prints a diagnostic and aborts
  deterministically. A handler that returns `FaultAction::SkipOperation`
  makes the call return without mutating the pool, which is what makes
  these paths unit-testable.
- **Release:** the guards are compiled out entirely. The index must come
  from book state — the release contract is documented trust. Run the
  Audit flavor to verify that invariant.

**Complexity:** $O(1)$.

#### `deallocate_chain`

```cpp
void deallocate_chain(uint32_t head, uint32_t tail);
```

Returns an entire price level's queue to the free list in a single
splice: the intrusive order chain becomes the free-list continuation.
This is the bulk-teardown path behind the batch-match speedup, and the
pool's most fragile operation.

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `head` | `uint32_t` | First slot of the chain. |
| `tail` | `uint32_t` | Last slot of the chain. |

**Debug / Audit:** both endpoints are range-checked *before* any write,
and every walked slot's state stamp is verified — so an out-of-range
index can never become a heap-buffer-overflow write. A broken chain is
reported at the slot that carries it, before the free-list splice
happens; the slots already walked are released by then, so the pool is
left partially released. The default fault handler aborts, so that
partial state is only observable under a test handler that returns
`FaultAction::SkipOperation`.

**Complexity:** $O(chain length).

#### Utility Methods

| Signature | Returns | Description |
| :--- | :--- | :--- |
| `size_t capacity() const noexcept` | Total pool capacity. | |
| `bool is_allocated(uint32_t idx) const noexcept` | `true` if the index is currently marked as allocated. In Release this degenerates to a bounds check — a real answer requires Debug/Audit. | |
| `T* data() noexcept` | Raw pointer to the underlying continuous memory array. | |

---
## 6. Macros

### `HFT_DEBUG_TOOLS`

The single switch that gates every debugging aid in the library
(`src/Macros.h`). Defined as `1` when `NDEBUG` is undefined (Debug) or
`HFT_AUDIT_MODE` is defined (Audit), and `0` in Release.

| Build Mode | Expansion | Behavior |
| :--- | :--- | :--- |
| **Debug** (`!NDEBUG`) | `1` | Slot stamps, poison patterns, generation counters, allocation tracking, the event journal, the re-entrancy/thread latches and `validate_book()` are all compiled in. |
| **Audit** (`-DHFT_AUDIT_MODE`) | `1` | The same aids, at `-O2` — memory-safety verification on optimized code. |
| **Release** (`NDEBUG`) | `0` | All aids compile out. `MemoryPool::deallocate` trusts its argument; the release contract is documented trust. |

### `HFT_AUDIT_MODE`

A compile-time macro (`-DHFT_AUDIT_MODE`) that forces the debugging aids
to stay active even though `NDEBUG` is defined. It is the Audit build
flavor: instrumented, optimized, no sanitizers — the gear that catches
memory-safety bugs in code the optimizer actually shaped.

### `HFT_FORCEINLINE`, `HFT_LIKELY`, `HFT_UNLIKELY`, `HFT_PREFETCH`

Compiler built-ins with a portable MSVC fallback (`src/Macros.h`):
`always_inline`, branch-probability hints, and a cache prefetch. On
toolchains that are neither GCC/Clang nor MSVC the prefetch compiles to
a no-op; the MSVC path uses `_mm_prefetch`, which is x86-only and
approximates the read/write and locality arguments away.

> **Note:** There is deliberately no `HFT_ASSUME` macro. An `assume`
> expands to `__builtin_unreachable()` in release builds, and a verify
> whose semantics are `return;` silently breaks value-returning
> functions. Public-API preconditions are guarded with explicit early
> returns carrying a `RejectReason` instead; the few *internal*
> preconditions use `assert()`, which compiles out under `NDEBUG`.

---

## 7. Thread Safety Guarantees

> [!CAUTION]
> **This engine is explicitly single-threaded.** There are no mutexes, atomics, or memory fences in any code path.

This is a deliberate design decision. In production HFT systems, the matching engine runs on an isolated CPU core (`isolcpus` + `pthread_setaffinity_np`). Inter-thread communication is handled via lock-free SPSC (Single-Producer Single-Consumer) ring buffers on separate threads — never inside the matching loop itself.

**Consequence:** Calling `add_order` or `cancel_order` concurrently from multiple threads results in **undefined behavior** (data races on `order_map_`, `MemoryPool`, and `PriceLevel` linked lists).

### 7.1 Re-entrancy contract

The single-thread rule is not enough on its own: a trade callback runs
*inside* the matching loop, while the loop is holding raw pointers into
pool storage. A callback that calls `add_order` or `cancel_order` on the
same book would invalidate those pointers mid-match.

**Rule:** a trade callback MUST NOT call `add_order` or `cancel_order` on
the same `OrderBook` instance, directly or indirectly.

Debug and Audit builds enforce this with a re-entrancy latch. The
violation is reported through the `BookFaultHandler`; with no handler
installed (the default) the book prints a diagnostic and aborts
deterministically. A handler that returns `FaultAction::SkipOperation`
makes the nested call return `RejectReason::ReentrancyBlocked` instead
of aborting, and the book stays internally consistent. Release trusts
the contract by design.
