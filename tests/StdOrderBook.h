#pragma once

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>

#include "../src/Order.h"
#include "../src/OrderBook.h"  // For Trade, RejectReason, etc.

namespace hft {

/// A deliberately standard-library-based order book for benchmarking AND as
/// the differential-testing oracle (audit H-5).
///
/// AUDIT FIXES in this file:
///  * M-7: the buy-side fill loop erased order_map_ but NOT side_map_ — a
///    slow leak that inflated the reference's memory footprint and quietly
///    flattered the engine's memory benchmark ratios. Mirrored the sell side.
///  * H-5:  the reference now performs the SAME input validation as the
///    engine (zero qty, id bounds, duplicate id, price bounds, market-on-
///    empty-book, filled-quantity out-param), so the differential fuzzer can
///    feed it arbitrary streams — previously it accepted everything, so the
///    fuzzer had to generate only in-range values and could never certify
///    rejection paths.
///  * The trade callback now carries the same noexcept static_assert as the
///    engine, so the oracle cannot drift from the enforced contract.
struct StdOrder {
    OrderId id;
    Price price;
    Quantity quantity;
    Side side;
    OrderType type;
    uint64_t timestamp;  // Realistic field — real books need timestamps
};

class StdOrderBook {
   public:
    explicit StdOrderBook(size_t max_order_id, size_t max_active_orders, Price min_price, Price max_price)
        : min_price_(min_price),
          max_price_(max_price),
          max_order_id_(max_order_id),
          max_active_orders_(max_active_orders),
          next_timestamp_(0) {}

    explicit StdOrderBook(size_t max_orders, Price min_price, Price max_price)
        : StdOrderBook(max_orders, max_orders, min_price, max_price) {}

    template<typename TradeCallback>
    [[nodiscard]] RejectReason add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side,
                                         TradeCallback&& on_trade, Quantity* filled_out = nullptr) {
        static_assert(noexcept(std::declval<TradeCallback>()(std::declval<const Trade&>())),
                      "Trade callback must be noexcept — mirrors the engine's contract");

        if (filled_out) *filled_out = 0;

        if (type == OrderType::Cancel) {
            return cancel_order(id);
        }

        if (quantity == 0) return RejectReason::InvalidQuantity;

        // Engine-parity validation (audit H-5 fix).
        if (id == 0 || id > max_order_id_) return RejectReason::OutOfBoundsOrderId;
        if (order_map_.find(id) != order_map_.end()) return RejectReason::DuplicateOrderId;
        if (type == OrderType::Limit && (price < min_price_ || price > max_price_)) {
            return RejectReason::OutOfBoundsPrice;
        }
        if (type == OrderType::Market) {
            // Opposite side of a BUY is the ASKS (you buy against offers);
            // the opposite of a SELL is the BIDS. (An earlier port inverted
            // this — caught by the StdReference parity tests, which is exactly
            // what they are for.)
            const bool opposite_empty = (side == Side::Buy) ? asks_.empty() : bids_.empty();
            if (opposite_empty) return RejectReason::MarketOnEmptyBook;
        }

        StdOrder order{
            id,       type == OrderType::Market ? (side == Side::Buy ? MARKET_BUY_PRICE : MARKET_SELL_PRICE) : price,
            quantity, side,
            type,     next_timestamp_++};

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
                        // AUDIT M-7 FIX: erase side_map_ too — the sell side always did;
                        // the missing erase here leaked one entry per fully-filled ask.
                        order_map_.erase(it->id);
                        side_map_.erase(it->id);
                        it = level.erase(it);  // Heap deallocation of list node
                        --live_count_;         // maker left the book (parity with sell side)
                    } else {
                        ++it;
                    }
                }
                if (level.empty()) {
                    asks_.erase(best_ask_it);  // Tree rebalancing (Red-Black rotation)
                }
            }
            if (order.quantity > 0 && type == OrderType::Limit) {
                if (live_count_ + 1 > max_active_orders_) return RejectReason::PoolExhausted;
                // Heap allocation: std::list node + tree node insertion
                bids_[order.price].push_back(order);
                order_map_[id] = std::prev(bids_[order.price].end());
                side_map_[id] = Side::Buy;
                ++live_count_;
            }
        } else {
            // Walk the bids tree from highest to lowest
            while (order.quantity > 0 && !bids_.empty()) {
                auto best_bid_it = bids_.begin();  // std::greater => highest first
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
                        --live_count_;
                    } else {
                        ++it;
                    }
                }
                if (level.empty()) {
                    bids_.erase(best_bid_it);
                }
            }
            if (order.quantity > 0 && type == OrderType::Limit) {
                if (live_count_ + 1 > max_active_orders_) return RejectReason::PoolExhausted;
                asks_[order.price].push_back(order);
                order_map_[id] = std::prev(asks_[order.price].end());
                side_map_[id] = Side::Sell;
                ++live_count_;
            }
        }

        if (filled_out) *filled_out = quantity - order.quantity;
        return RejectReason::Accepted;
    }

    [[nodiscard]] RejectReason cancel_order(OrderId id) {
        auto it = order_map_.find(id);  // Hash lookup
        if (it == order_map_.end()) return RejectReason::CancelFailed;

        auto list_it = it->second;
        Price price = list_it->price;

        // Need to look up side from separate map (realistic cost)
        auto side_it = side_map_.find(id);
        if (side_it == side_map_.end()) return RejectReason::CancelFailed;
        Side side = side_it->second;

        if (side == Side::Buy) {
            auto level_it = bids_.find(price);  // Tree lookup O(log N)
            if (level_it != bids_.end()) {
                level_it->second.erase(list_it);                      // Heap deallocation
                if (level_it->second.empty()) bids_.erase(level_it);  // Tree rebalance
            }
        } else {
            auto level_it = asks_.find(price);
            if (level_it != asks_.end()) {
                level_it->second.erase(list_it);
                if (level_it->second.empty()) asks_.erase(level_it);
            }
        }
        order_map_.erase(it);  // Hash erase
        side_map_.erase(id);   // Hash erase
        --live_count_;
        return RejectReason::Accepted;
    }

    // ── Introspection parity with the engine (audit M-5/M-7 test needs) ──
    [[nodiscard]] size_t active_order_count() const noexcept { return live_count_; }
    [[nodiscard]] bool order_exists(OrderId id) const { return order_map_.find(id) != order_map_.end(); }
    [[nodiscard]] size_t side_map_size() const noexcept { return side_map_.size(); }  // leak sentinel
    [[nodiscard]] size_t price_level_count(Side side) const { return side == Side::Buy ? bids_.size() : asks_.size(); }

   private:
    // Red-Black Tree: O(log N) lookup, insert, delete with pointer chasing
    std::map<Price, std::list<StdOrder>, std::greater<Price>> bids_;
    std::map<Price, std::list<StdOrder>> asks_;

    // Hash table with chaining: O(1) amortized but heap-allocated buckets
    std::unordered_map<OrderId, std::list<StdOrder>::iterator> order_map_;
    std::unordered_map<OrderId, Side> side_map_;  // Realistic: need side tracking

    Price min_price_;
    Price max_price_;
    size_t max_order_id_;
    size_t max_active_orders_;
    size_t live_count_ = 0;
    uint64_t next_timestamp_;
};

}  // namespace hft
