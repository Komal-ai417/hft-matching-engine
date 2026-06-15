#pragma once

#include <cstdint>
#include <limits>

namespace hft {

using OrderId = uint32_t;
using Price = uint32_t; // Fixed-point representation for speed (e.g., $1.50 = 15000)
using Quantity = uint32_t;

/// Named constants for market order pricing — avoids fragile unsigned underflow hacks.
constexpr Price MARKET_BUY_PRICE  = std::numeric_limits<Price>::max();
constexpr Price MARKET_SELL_PRICE = 0;

constexpr uint32_t INVALID_INDEX = UINT32_MAX;

/**
 * @enum Side
 * @brief Represents the side of the trade (Buy or Sell).
 */
enum class Side : uint8_t {
    Buy,
    Sell
};

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
 */
struct Order {
    uint32_t next = INVALID_INDEX;
    uint32_t prev = INVALID_INDEX;
    OrderId id;
    Price price;
    Quantity quantity;
    Side side;

    Order() noexcept = default;
    
    Order(OrderId id, Price price, Quantity quantity, Side side) noexcept
        : next(INVALID_INDEX), prev(INVALID_INDEX), id(id), price(price), quantity(quantity), side(side) {}
};

}// namespace hft
