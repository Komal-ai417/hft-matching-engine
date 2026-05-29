#pragma once

#include <map>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <cstdint>
#include "../src/Order.h"
#include "../src/OrderBook.h" // For Trade, RejectReason, etc.

namespace hft {

/// A deliberately standard-library-based order book for benchmarking comparison.
/// Uses std::map (Red-Black Tree), std::list (heap-allocated nodes), and
/// std::unordered_map (hash table with chaining) — all industry-standard
/// but cache-hostile data structures.
struct StdOrder {
    OrderId id;
    Price price;
    Quantity quantity;
    Side side;
    OrderType type;
    uint64_t timestamp; // Realistic field — real books need timestamps
};

class StdOrderBook {
public:
    explicit StdOrderBook(size_t /* max_orders */, Price /* min_price */, Price /* max_price */)
        : next_timestamp_(0) {}

    template <typename TradeCallback>
    void add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side, TradeCallback&& on_trade) {
        if (type == OrderType::Cancel) {
            cancel_order(id);
            return;
        }

        if (quantity == 0) return;

        // Hash table lookup for duplicate check (realistic)
        if (order_map_.find(id) != order_map_.end()) return;

        StdOrder order{id, 
                       type == OrderType::Market ? (side == Side::Buy ? MARKET_BUY_PRICE : MARKET_SELL_PRICE) : price, 
                       quantity, side, type, next_timestamp_++};

        if (side == Side::Buy) {
            // Walk the asks tree from lowest to highest (std::map iterator = tree walk)
            while (order.quantity > 0 && !asks_.empty()) {
                auto best_ask_it = asks_.begin();
                if (best_ask_it->first > order.price) break;

                auto& level = best_ask_it->second;
                auto it = level.begin();
                while (it != level.end() && order.quantity > 0) {
                    Quantity trade_qty = std::min(order.quantity, it->quantity);
                    
                    // Construct trade on stack and invoke callback
                    Trade t{it->id, id, it->price, trade_qty};
                    on_trade(t);

                    order.quantity -= trade_qty;
                    it->quantity -= trade_qty;

                    if (it->quantity == 0) {
                        // Hash table erase (triggers rehash/rebalance)
                        order_map_.erase(it->id);
                        it = level.erase(it); // Heap deallocation of list node
                    } else {
                        ++it;
                    }
                }
                if (level.empty()) {
                    asks_.erase(best_ask_it); // Tree rebalancing (Red-Black rotation)
                }
            }
            if (order.quantity > 0 && type == OrderType::Limit) {
                // Heap allocation: std::list node + tree node insertion
                bids_[order.price].push_back(order);
                order_map_[id] = std::prev(bids_[order.price].end());
                side_map_[id] = Side::Buy;
            }
        } else {
            // Walk the bids tree from highest to lowest
            while (order.quantity > 0 && !bids_.empty()) {
                auto best_bid_it = bids_.begin(); // std::greater => highest first
                if (best_bid_it->first < order.price) break;

                auto& level = best_bid_it->second;
                auto it = level.begin();
                while (it != level.end() && order.quantity > 0) {
                    Quantity trade_qty = std::min(order.quantity, it->quantity);
                    
                    Trade t{it->id, id, it->price, trade_qty};
                    on_trade(t);

                    order.quantity -= trade_qty;
                    it->quantity -= trade_qty;

                    if (it->quantity == 0) {
                        order_map_.erase(it->id);
                        side_map_.erase(it->id);
                        it = level.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (level.empty()) {
                    bids_.erase(best_bid_it);
                }
            }
            if (order.quantity > 0 && type == OrderType::Limit) {
                asks_[order.price].push_back(order);
                order_map_[id] = std::prev(asks_[order.price].end());
                side_map_[id] = Side::Sell;
            }
        }
    }

    void cancel_order(OrderId id) {
        auto it = order_map_.find(id);  // Hash lookup
        if (it == order_map_.end()) return;

        auto list_it = it->second;
        Price price = list_it->price;
        
        // Need to look up side from separate map (realistic cost)
        auto side_it = side_map_.find(id);
        if (side_it == side_map_.end()) return;
        Side side = side_it->second;

        if (side == Side::Buy) {
            auto level_it = bids_.find(price);  // Tree lookup O(log N)
            if (level_it != bids_.end()) {
                level_it->second.erase(list_it); // Heap deallocation
                if (level_it->second.empty()) bids_.erase(level_it); // Tree rebalance
            }
        } else {
            auto level_it = asks_.find(price);
            if (level_it != asks_.end()) {
                level_it->second.erase(list_it);
                if (level_it->second.empty()) asks_.erase(level_it);
            }
        }
        order_map_.erase(it);    // Hash erase
        side_map_.erase(id);     // Hash erase
    }

private:
    // Red-Black Tree: O(log N) lookup, insert, delete with pointer chasing
    std::map<Price, std::list<StdOrder>, std::greater<Price>> bids_;
    std::map<Price, std::list<StdOrder>> asks_;
    
    // Hash table with chaining: O(1) amortized but heap-allocated buckets
    std::unordered_map<OrderId, std::list<StdOrder>::iterator> order_map_;
    std::unordered_map<OrderId, Side> side_map_;  // Realistic: need side tracking
    
    uint64_t next_timestamp_;
};

} // namespace hft
