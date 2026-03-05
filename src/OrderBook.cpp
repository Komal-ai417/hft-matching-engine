#include "OrderBook.h"
#include <algorithm>

namespace hft {

const std::vector<Trade>& OrderBook::add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side) {
    trades_.clear(); // O(1) clear without freeing memory capacity

    if (type == OrderType::Cancel) {
        cancel_order(id);
        return trades_;
    }

    // Allocate order from pool
    Order* order = order_pool_.allocate();
    order->id = id;
    order->price = type == OrderType::Market ? (side == Side::Buy ? static_cast<Price>(-1) : 0) : price;
    order->quantity = quantity;
    order->side = side;

    match_order(order);

    if (order->quantity > 0) {
        if (type == OrderType::Market) {
            // Market orders don't rest in the book.
            order_pool_.deallocate(order);
        } else {
            // Rest in the book
            order_map_[id] = order;
            if (side == Side::Buy) {
                auto result = bids_.emplace(order->price, order->price);
                result.first->second.append_order(order);
            } else {
                auto result = asks_.emplace(order->price, order->price);
                result.first->second.append_order(order);
            }
        }
    } else {
        // Taker order fully filled
        order_pool_.deallocate(order);
    }

    return trades_;
}

void OrderBook::match_order(Order* taker_order) {
    
    if (taker_order->side == Side::Buy) {
        // Cross against Asks (lowest to highest)
        auto it = asks_.begin();
        while (it != asks_.end() && taker_order->quantity > 0) {
            PriceLevel& level = it->second;
            
            if (taker_order->price < level.price) break; // No more matching prices

            Order* maker_order = level.head;
            while (maker_order != nullptr && taker_order->quantity > 0) {
                Quantity trade_qty = std::min(taker_order->quantity, maker_order->quantity);
                Price trade_price = maker_order->price; // Price improvement (Maker's price)
                
                trades_.push_back({maker_order->id, taker_order->id, trade_price, trade_qty});
                
                taker_order->quantity -= trade_qty;
                maker_order->quantity -= trade_qty;
                level.total_quantity -= trade_qty;
                
                Order* next_maker = maker_order->next;

                if (maker_order->quantity == 0) {
                    // Fully filled, remove from book
                    level.remove_order(maker_order);
                    order_map_.erase(maker_order->id);
                    order_pool_.deallocate(maker_order);
                }
                
                maker_order = next_maker;
            }

            if (level.is_empty()) {
                it = asks_.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        // Cross against Bids (highest to lowest)
        auto it = bids_.begin();
        while (it != bids_.end() && taker_order->quantity > 0) {
            PriceLevel& level = it->second;
            
            if (taker_order->price > level.price) break;

            Order* maker_order = level.head;
            while (maker_order != nullptr && taker_order->quantity > 0) {
                Quantity trade_qty = std::min(taker_order->quantity, maker_order->quantity);
                Price trade_price = maker_order->price;
                
                trades_.push_back({maker_order->id, taker_order->id, trade_price, trade_qty});
                
                taker_order->quantity -= trade_qty;
                maker_order->quantity -= trade_qty;
                level.total_quantity -= trade_qty;
                
                Order* next_maker = maker_order->next;

                if (maker_order->quantity == 0) {
                    level.remove_order(maker_order);
                    order_map_.erase(maker_order->id);
                    order_pool_.deallocate(maker_order);
                }
                
                maker_order = next_maker;
            }

            if (level.is_empty()) {
                it = bids_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return false;
    
    Order* order = it->second;
    Price price = order->price;
    
    if (order->side == Side::Buy) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            level_it->second.remove_order(order);
            if (level_it->second.is_empty()) bids_.erase(level_it);
        }
    } else {
         auto level_it = asks_.find(price);
         if (level_it != asks_.end()) {
             level_it->second.remove_order(order);
             if (level_it->second.is_empty()) asks_.erase(level_it);
         }
    }
    
    order_map_.erase(it);
    order_pool_.deallocate(order);
    return true;
}

} // namespace hft
