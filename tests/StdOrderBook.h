#pragma once

#include <map>
#include <unordered_map>
#include <list>
#include <algorithm>
#include "../src/Order.h"
#include "../src/OrderBook.h" // For Trade, RejectReason, etc.

namespace hft {

struct StdOrder {
    OrderId id;
    Price price;
    Quantity quantity;
    Side side;
};

class StdOrderBook {
public:
    explicit StdOrderBook(size_t /* max_orders */, Price /* min_price */, Price /* max_price */) {}

    template <typename TradeCallback>
    inline void add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side, TradeCallback&& on_trade) {
        if (type == OrderType::Cancel) {
            cancel_order(id);
            return;
        }

        if (quantity == 0) return;

        if (order_map.find(id) != order_map.end()) return;

        StdOrder order{id, type == OrderType::Market ? (side == Side::Buy ? MARKET_BUY_PRICE : MARKET_SELL_PRICE) : price, quantity, side};

        if (side == Side::Buy) {
            while (order.quantity > 0 && !asks.empty()) {
                auto best_ask_it = asks.begin();
                if (best_ask_it->first > order.price) break;

                auto& level = best_ask_it->second;
                auto it = level.begin();
                while (it != level.end() && order.quantity > 0) {
                    Quantity trade_qty = std::min(order.quantity, it->quantity);
                    on_trade(Trade{it->id, id, it->price, trade_qty});

                    order.quantity -= trade_qty;
                    it->quantity -= trade_qty;

                    if (it->quantity == 0) {
                        order_map.erase(it->id);
                        it = level.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (level.empty()) {
                    asks.erase(best_ask_it);
                }
            }
            if (order.quantity > 0 && type == OrderType::Limit) {
                bids[order.price].push_back(order);
                order_map[id] = std::prev(bids[order.price].end());
            }
        } else {
            while (order.quantity > 0 && !bids.empty()) {
                auto best_bid_it = bids.begin();
                if (best_bid_it->first < order.price) break;

                auto& level = best_bid_it->second;
                auto it = level.begin();
                while (it != level.end() && order.quantity > 0) {
                    Quantity trade_qty = std::min(order.quantity, it->quantity);
                    on_trade(Trade{it->id, id, it->price, trade_qty});

                    order.quantity -= trade_qty;
                    it->quantity -= trade_qty;

                    if (it->quantity == 0) {
                        order_map.erase(it->id);
                        it = level.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (level.empty()) {
                    bids.erase(best_bid_it);
                }
            }
            if (order.quantity > 0 && type == OrderType::Limit) {
                asks[order.price].push_back(order);
                order_map[id] = std::prev(asks[order.price].end());
            }
        }
    }

    void cancel_order(OrderId id) {
        auto it = order_map.find(id);
        if (it == order_map.end()) return;

        auto list_it = it->second;
        Price price = list_it->price;
        Side side = list_it->side;

        if (side == Side::Buy) {
            auto level_it = bids.find(price);
            if (level_it != bids.end()) {
                level_it->second.erase(list_it);
                if (level_it->second.empty()) bids.erase(level_it);
            }
        } else {
            auto level_it = asks.find(price);
            if (level_it != asks.end()) {
                level_it->second.erase(list_it);
                if (level_it->second.empty()) asks.erase(level_it);
            }
        }
        order_map.erase(it);
    }

private:
    std::map<Price, std::list<StdOrder>, std::greater<Price>> bids;
    std::map<Price, std::list<StdOrder>> asks;
    std::unordered_map<OrderId, std::list<StdOrder>::iterator> order_map;
};

} // namespace hft
