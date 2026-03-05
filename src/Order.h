#pragma once

#include <cstdint>
#include <iostream>

namespace hft {

using OrderId = uint64_t;
using Price = uint64_t; // Fixed-point representation for speed (e.g., $1.50 = 15000)
using Quantity = uint32_t;

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
 * We align to 32 bytes to ensure an `Order` never strides two 64-byte 
 * CPU cache lines, preventing cache tearing.
 */
struct alignas(32) Order {
    OrderId id;           // 8 bytes
    Price price;          // 8 bytes
    Quantity quantity;    // 4 bytes
    Side side;            // 1 byte
    // 3 bytes padding
    
    Order* next = nullptr;// 8 bytes
    Order* prev = nullptr;// 8 bytes

    Order() noexcept : id(0), price(0), quantity(0), side(Side::Buy) {}
    
    Order(OrderId id, Price price, Quantity quantity, Side side) noexcept
        : id(id), price(price), quantity(quantity), side(side) {}
};

inline std::ostream& operator<<(std::ostream& os, const Side& side) {
    return os << (side == Side::Buy ? "Buy" : "Sell");
}

} // namespace hft
