#pragma once

#include <cstdint>
#include <iostream>
#include <limits>

namespace hft {

using OrderId = uint32_t;
using Price = uint32_t; // Fixed-point representation for speed (e.g., $1.50 = 15000)
using Quantity = uint32_t;

/// Named constants for market order pricing — avoids fragile unsigned underflow hacks.
constexpr Price MARKET_BUY_PRICE  = std::numeric_limits<Price>::max();
constexpr Price MARKET_SELL_PRICE = 0;

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
 * 
 * CACHE OPTIMIZATION: The struct is perfectly packed to exactly 32 bytes.
 * This guarantees that exactly two orders fit into a 64-byte CPU cache line
 * without any split-load overhead. Padding is minimized.
 */
struct Order {
    Order* next = nullptr;// 8 bytes
    Order* prev = nullptr;// 8 bytes
    
    OrderId id;           // 4 bytes
    Price price;          // 4 bytes
    Quantity quantity;    // 4 bytes
    Side side;            // 1 byte
    // 3 bytes padding for 8-byte boundary alignment


    Order() noexcept : id(0), price(0), quantity(0), side(Side::Buy) {}
    
    Order(OrderId id, Price price, Quantity quantity, Side side) noexcept
        : id(id), price(price), quantity(quantity), side(side) {}
};

inline std::ostream& operator<<(std::ostream& os, const Side& side) {
    return os << (side == Side::Buy ? "Buy" : "Sell");
}

}// namespace hft
