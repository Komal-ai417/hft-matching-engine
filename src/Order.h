#pragma once

#include <cstdint>
#include <iostream>

namespace hft {

using OrderId = uint64_t;
using Price = uint64_t; // Fixed-point representation for speed (e.g., $1.50 = 15000)
using Quantity = uint32_t;

enum class Side : uint8_t {
    Buy,
    Sell
};

enum class OrderType : uint8_t {
    Limit,   // Matches at or better than a specified price
    Market,  // Matches immediately at the best available price
    Cancel   // Cancels an existing order
};

// Represents a single order in the book.
// We use intrusive pointers (next/prev) to avoid standard library allocations (like std::list)
// and memory fragmentation.
struct Order {
    OrderId id;
    Price price;
    Quantity quantity;
    Side side;
    
    // Intrusive doubly-linked list pointers to keep Orders in a PriceLevel
    Order* next = nullptr;
    Order* prev = nullptr;

    Order() : id(0), price(0), quantity(0), side(Side::Buy) {}
    
    Order(OrderId id, Price price, Quantity quantity, Side side)
        : id(id), price(price), quantity(quantity), side(side) {}
};

inline std::ostream& operator<<(std::ostream& os, const Side& side) {
    return os << (side == Side::Buy ? "Buy" : "Sell");
}

} // namespace hft
