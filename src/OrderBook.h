#pragma once

#include "Order.h"
#include "PriceLevel.h"
#include "MemoryPool.h"
#include <map>
#include <unordered_map>
#include <vector>

namespace hft {

struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Price price;
    Quantity quantity;
};

class OrderBook {
public:
    explicit OrderBook(size_t max_orders) : order_pool_(max_orders) {
        order_map_.reserve(max_orders);
    }

    // Process a new incoming order. Returns a list of generated trades.
    std::vector<Trade> add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side);

    // Cancel an existing order. Returns true if successful.
    bool cancel_order(OrderId id);

private:
    std::vector<Trade> match_order(Order* taker_order);
    
    // Bid book (highest price first)
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    
    // Ask book (lowest price first)
    std::map<Price, PriceLevel, std::less<Price>> asks_;
    
    // Fast O(1) lookup for cancellations
    std::unordered_map<OrderId, Order*> order_map_;
    
    // Zero-allocation memory pool for Order structures
    MemoryPool<Order> order_pool_;
};

} // namespace hft
