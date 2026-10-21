#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace hft {

using OrderId = uint32_t;
using Price = uint32_t;  // Fixed-point representation for speed (e.g., $1.50 = 15000)
using Quantity = uint32_t;

/// Named constants for market order pricing — avoids fragile unsigned underflow hacks.
constexpr Price MARKET_BUY_PRICE = std::numeric_limits<Price>::max();
constexpr Price MARKET_SELL_PRICE = 0;

constexpr uint32_t INVALID_INDEX = UINT32_MAX;

/// The exact size Order must occupy: 24 B packs 2.67 orders per 64-byte cache
/// line (vs 2.0 for alignas(32)) — see the layout note on struct Order.
inline constexpr size_t ORDER_BYTES = 24;

/**
 * @enum Side
 * @brief Represents the side of the trade (Buy or Sell).
 */
enum class Side : uint8_t { Buy, Sell };

enum class OrderType : uint8_t {
    Limit,   // Matches at or better than a specified price
    Market,  // Matches immediately at the best available price
    Cancel   // Cancels an existing order
};

/**
 * @struct Order
 * @brief Represents a single order in the Limit Order Book.
 *
 * DESIGN DECISION: We explicitly avoid `std::list` due to its node-allocation
 * overhead and poor cache locality. Instead, we use intrusive doubly-linked
 * list pointers (`next` and `prev`). This keeps the Order struct entirely
 * self-contained, allowing dense packing inside the `MemoryPool` and
 * massively improving L1/L2 cache hit rates during order traversal.
 *
 * LAYOUT NOTE (audit finding L-5 of report #1 / §4.1 of report #3):
 * The struct is exactly 24 bytes. `next` and `prev` MUST stay as the first
 * two members: MemoryPool's intrusive free-list overlays its link in the
 * FIRST 4 BYTES of every slot (via memcpy — see MemoryPool.h), which lands
 * on `Order::next`. The static_asserts below turn that implicit contract
 * into a compile error if the layout ever drifts.
 *
 * We deliberately do NOT alignas(32) this struct: 24-byte packing yields
 * 2.67 orders per 64-byte cache line vs 2.0 for alignas(32) — a 33%
 * density win that outweighs line-straddle cost for the intrusive-list
 * traversal pattern (sequential neighbor access is prefetcher-friendly).
 */
struct Order {
    uint32_t next = INVALID_INDEX;  // MUST be the first member (free-list overlay)
    uint32_t prev = INVALID_INDEX;
    OrderId id = 0;
    Price price = 0;
    Quantity quantity = 0;
    Side side = Side::Buy;

    Order() noexcept = default;

    Order(OrderId id_, Price price_, Quantity quantity_, Side side_) noexcept
        : next(INVALID_INDEX), prev(INVALID_INDEX), id(id_), price(price_), quantity(quantity_), side(side_) {}
};

// Layout contract — enforced at compile time (audit L-5).
static_assert(sizeof(Order) == ORDER_BYTES,
              "Order must stay 24 bytes: 2.67 fit per 64B cache line and MemoryPool's free-list overlay depends on the "
              "first 4 bytes.");
static_assert(std::is_trivially_copyable_v<Order>,
              "Order must stay trivially copyable: MemoryPool memcpy-overlays free-list links into raw slots.");
static_assert(std::is_trivially_destructible_v<Order>,
              "Order must stay trivially destructible: MemoryPool never runs destructors on free.");

}  // namespace hft
