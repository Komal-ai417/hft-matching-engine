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

/**
 * @class OrderBook
 * @brief The core matching engine utilizing Price-Time priority.
 * 
 * DESIGN DECISION:
 * 1. Bids/Asks are stored in `std::map`. While an Array-backed flat map could
 *    be faster for dense prices, `std::map` handles sparse prices gracefully.
 * 2. Instant cancellations are achieved using an `std::unordered_map` that
 *    points directly to the `Order*` in memory, allowing O(1) removals.
 */
class OrderBook {
public:
    /**
     * @brief Initializes the OrderBook and pre-allocates its MemoryPool.
     * @param max_orders The total number of resting orders the engine can handle concurrently.
     */
    explicit OrderBook(size_t max_orders) : order_pool_(max_orders) {
        order_map_.reserve(max_orders);
    }

    /**
     * @brief Process an incoming order. Converts to market order if price logic applies.
     * @return A vector of Trade structures representing executed matches.
     */
    std::vector<Trade> add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side);

    /**
     * @brief Cancels an existing order from the book via its ID in O(1) time.
     * @return True if the order existed and was canceled perfectly.
     */
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
