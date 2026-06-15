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

Defined in `OrderBook.h`. Retained for potential future use (e.g., restoring `std::expected` return types). Currently, validation failures result in silent early returns.

| Enumerator | Value | Trigger Condition |
| :--- | :--- | :--- |
| `InvalidQuantity` | `0` | `quantity == 0` for Limit or Market orders. |
| `DuplicateOrderId` | `1` | `order_map_[id]` is not `INVALID_INDEX` (already contains an active index). |
| `OutOfBoundsOrderId` | `2` | `id >= order_map_.size()` or `id == 0`. |
| `OutOfBoundsPrice` | `3` | `price < min_price` or `price > max_price` for Limit orders. |
| `CancelFailed` | `4` | `order_map_[id] == INVALID_INDEX` (order does not exist or was already filled). |
| `PoolExhausted` | `5` | Pool exhausted, order cannot be posted. |

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
| `total_quantity` | `uint64_t` | Sum of all resting order quantities at this level. |
| `order_count` | `uint32_t` | Number of orders currently queued. |
| `head` | `uint32_t` | Index of the oldest (first-in) order. |
| `tail` | `uint32_t` | Index of the newest (last-in) order. |

**Note:** The `price` is implicit from the level's index in the `bids_levels_` or `asks_levels_` vector.

#### Methods

| Signature | Complexity | Description |
| :--- | :--- | :--- |
| `void append_order(uint32_t order_idx, Order* pool_data)` | $O(1)$ | Appends an order to the tail of the intrusive linked list. |
| `void remove_order(uint32_t order_idx, Order* pool_data)` | $O(1)$ | Detaches an order from the linked list by re-wiring `prev`/`next` indices. |
| `bool is_empty() const noexcept` | $O(1)$ | Returns `true` if `head == INVALID_INDEX`. |

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

**Side Effects:** Allocates `(max_price - min_price + 1)` `PriceLevel` structs for both bids and asks. Initializes bitset arrays for hardware-accelerated price discovery. Validates `min_price <= max_price` (aborts/asserts if invalid).

#### `add_order`

```cpp
template <typename TradeCallback>
[[nodiscard]] HFT_FORCEINLINE RejectReason add_order(
    OrderId id,
    OrderType type,
    Price price,
    Quantity quantity,
    Side side,
    TradeCallback&& on_trade
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

**Throws:** Nothing. Returns `INVALID_INDEX` when out of memory, leading to silent drops rather than throwing.

**Complexity:** $O(1)$.

#### `deallocate`

```cpp
void deallocate(uint32_t idx);
```

Returns a previously allocated object index back to the free-list.

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `idx` | `uint32_t` | Must be an index previously returned by `allocate()`. |

**Throws:**
- `std::out_of_range` if `idx >= pool_data_.size()` (Debug or `HFT_AUDIT_MODE` only).
- `std::logic_error` if the slot is already free (double-free detection via allocation byte-tracking vector, Debug or `HFT_AUDIT_MODE` only).

**Complexity:** $O(1)$.

#### Utility Methods

| Signature | Returns | Description |
| :--- | :--- | :--- |
| `size_t capacity() const noexcept` | Total pool capacity. | |
| `bool is_allocated(uint32_t idx) const noexcept` | `true` if the index is currently marked as allocated. | |
| `T* data() noexcept` | Raw pointer to the underlying continuous memory array. | |

---

## 6. Macros

### `HFT_ASSUME(cond)`

Defined in `Macros.h`. A safety-aware compiler optimization hint.

| Build Mode | Expansion | Behavior |
| :--- | :--- | :--- |
| **Debug** (`!NDEBUG`) | `assert(cond)` | Hard crash with diagnostic if `cond` is false. |
| **Release** (`NDEBUG`) | `__builtin_unreachable()` / `__assume(cond)` | Compiler eliminates dead code paths assuming `cond` is always true. Undefined Behavior if violated. |

**Usage in engine:**

```cpp
HFT_ASSUME(id < order_map_.size());   // Elides bounds-check assembly in Release
HFT_ASSUME(quantity > 0);             // Elides zero-check assembly in Release
```

### `HFT_AUDIT_MODE`

A compile-time macro (`-DHFT_AUDIT_MODE`) that forces the `MemoryPool` allocation bitmap to remain active even in Release builds (`NDEBUG`). This provides $O(1)$ double-free and bounds-checking validation with minor memory and performance overhead, intended for shadow/staging environments.

---

## 7. Thread Safety Guarantees

> [!CAUTION]
> **This engine is explicitly single-threaded.** There are no mutexes, atomics, or memory fences in any code path.

This is a deliberate design decision. In production HFT systems, the matching engine runs on an isolated CPU core (`isolcpus` + `pthread_setaffinity_np`). Inter-thread communication is handled via lock-free SPSC (Single-Producer Single-Consumer) ring buffers on separate threads — never inside the matching loop itself.

**Consequence:** Calling `add_order` or `cancel_order` concurrently from multiple threads results in **undefined behavior** (data races on `order_map_`, `MemoryPool`, and `PriceLevel` linked lists).
