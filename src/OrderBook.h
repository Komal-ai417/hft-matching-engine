#pragma once

#include "Order.h"
#include "PriceLevel.h"
#include "MemoryPool.h"
#include <vector>
#include <bit>
#include <algorithm>

namespace hft {

enum class RejectReason : uint8_t {
    InvalidQuantity,
    DuplicateOrderId,
    OutOfBoundsOrderId,
    OutOfBoundsPrice,
    CancelFailed
};

struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Price price;
    Quantity quantity;
};

#if !defined(NDEBUG) || !defined(__OPTIMIZE__)
    #include <cassert>
    #define HFT_ASSUME(cond) assert(cond)
#else
    #if defined(__clang__)
        #define HFT_ASSUME(cond) __builtin_assume(cond)
    #elif defined(__GNUC__)
        #define HFT_ASSUME(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
    #else
        #define HFT_ASSUME(cond) do { } while (0)
    #endif
#endif

class OrderBook {
public:
    explicit OrderBook(size_t max_orders, Price min_price, Price max_price);

    template <typename TradeCallback>
    [[gnu::always_inline]] inline void add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side, TradeCallback&& on_trade);

    void cancel_order(OrderId id);

private:
    template <Side side, typename TradeCallback>
    [[gnu::always_inline]] inline void match_order(Order* taker_order, TradeCallback&& on_trade);

    void update_best_bid_after_remove(Price removed_price);
    void update_best_ask_after_remove(Price removed_price);
    
    std::vector<uint64_t> bids_bitset_;
    std::vector<uint64_t> asks_bitset_;

    std::vector<PriceLevel> bids_levels_;
    std::vector<PriceLevel> asks_levels_;
    
    Price min_price_;
    Price max_price_;
    
    Price best_bid_;
    Price best_ask_;
    bool has_bids_;
    
    std::vector<Order*> order_map_;
    MemoryPool<Order> order_pool_;
};

// ====================================================================
// TEMPLATE IMPLEMENTATIONS
// ====================================================================

template <Side side, typename TradeCallback>
[[gnu::always_inline]] inline void OrderBook::match_order(Order* taker_order, TradeCallback&& on_trade) {
    if constexpr (side == Side::Buy) {
        // Cross against Asks (lowest to highest)
        while (best_ask_ <= max_price_ && taker_order->quantity > 0 && taker_order->price >= best_ask_) {
            PriceLevel& level = asks_levels_[best_ask_ - min_price_];
            
            Order* maker_order = level.head;
            while (maker_order != nullptr && taker_order->quantity > 0) {
                Quantity trade_qty = std::min(taker_order->quantity, maker_order->quantity);
                Price trade_price = maker_order->price;
                
                on_trade(Trade{maker_order->id, taker_order->id, trade_price, trade_qty});
                
                taker_order->quantity -= trade_qty;
                maker_order->quantity -= trade_qty;
                level.total_quantity -= trade_qty;
                
                Order* next_maker = maker_order->next;

                if (maker_order->quantity == 0) {
                    level.remove_order(maker_order);
                    order_map_[maker_order->id] = nullptr;
                    order_pool_.deallocate(maker_order);
                }
                maker_order = next_maker;
            }

            if (level.is_empty()) {
                // Clear the bit
                size_t bit_idx = best_ask_ - min_price_;
                asks_bitset_[bit_idx / 64] &= ~(1ULL << (bit_idx % 64));
                
                // Fast-forward best_ask_ using bitset
                uint64_t word_idx = bit_idx / 64;
                uint64_t mask = ~((1ULL << (bit_idx % 64)) - 1);
                uint64_t word = asks_bitset_[word_idx] & mask;
                
                bool found = false;
                if (word != 0) {
                    best_ask_ = min_price_ + word_idx * 64 + std::countr_zero(word);
                    found = true;
                } else {
                    for (size_t i = word_idx + 1; i < asks_bitset_.size(); ++i) {
                        if (asks_bitset_[i] != 0) {
                            best_ask_ = min_price_ + i * 64 + std::countr_zero(asks_bitset_[i]);
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    best_ask_ = max_price_ + 1; // Sentinel
                }
            }
        }
    } else {
        // Cross against Bids (highest to lowest)
        while (has_bids_ && taker_order->quantity > 0 && taker_order->price <= best_bid_) {
            PriceLevel& level = bids_levels_[best_bid_ - min_price_];
            
            Order* maker_order = level.head;
            while (maker_order != nullptr && taker_order->quantity > 0) {
                Quantity trade_qty = std::min(taker_order->quantity, maker_order->quantity);
                Price trade_price = maker_order->price;
                
                on_trade(Trade{maker_order->id, taker_order->id, trade_price, trade_qty});
                
                taker_order->quantity -= trade_qty;
                maker_order->quantity -= trade_qty;
                level.total_quantity -= trade_qty;
                
                Order* next_maker = maker_order->next;

                if (maker_order->quantity == 0) {
                    level.remove_order(maker_order);
                    order_map_[maker_order->id] = nullptr;
                    order_pool_.deallocate(maker_order);
                }
                maker_order = next_maker;
            }

            if (level.is_empty()) {
                // Clear the bit
                size_t bit_idx = best_bid_ - min_price_;
                bids_bitset_[bit_idx / 64] &= ~(1ULL << (bit_idx % 64));
                
                // Fast-forward best_bid_ using bitset (scan downwards)
                uint64_t word_idx = bit_idx / 64;
                uint64_t mask = (1ULL << (bit_idx % 64)) - 1; // bits below current
                uint64_t word = bids_bitset_[word_idx] & mask;
                
                bool found = false;
                if (word != 0) {
                    best_bid_ = min_price_ + word_idx * 64 + 63 - std::countl_zero(word);
                    found = true;
                } else {
                    for (int64_t i = word_idx - 1; i >= 0; --i) {
                        if (bids_bitset_[i] != 0) {
                            best_bid_ = min_price_ + i * 64 + 63 - std::countl_zero(bids_bitset_[i]);
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    has_bids_ = false;
                    best_bid_ = 0;
                }
            }
        }
    }
}

template <typename TradeCallback>
[[gnu::always_inline]] inline void OrderBook::add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side, TradeCallback&& on_trade) {
    if (type == OrderType::Cancel) {
        cancel_order(id);
        return;
    }

    if (quantity == 0) {
        return;
    }

    if (id >= order_map_.size()) {
        return;
    }
    
    HFT_ASSUME(id < order_map_.size());

    if (order_map_[id] != nullptr) {
        return;
    }

    if (type == OrderType::Limit) {
        if (price < min_price_ || price > max_price_) {
            return;
        }
    }

    HFT_ASSUME(quantity > 0);

    Order* order = order_pool_.allocate();
    order->id = id;
    order->price = type == OrderType::Market
                       ? (side == Side::Buy ? MARKET_BUY_PRICE : MARKET_SELL_PRICE)
                       : price;
    order->quantity = quantity;
    order->side = side;
    order->next = nullptr;
    order->prev = nullptr;

    if (side == Side::Buy) {
        match_order<Side::Buy>(order, std::forward<TradeCallback>(on_trade));
    } else {
        match_order<Side::Sell>(order, std::forward<TradeCallback>(on_trade));
    }

    if (order->quantity > 0) {
        if (type == OrderType::Market) {
            order_pool_.deallocate(order);
        } else {
            order_map_[id] = order;
            size_t bit_idx = order->price - min_price_;
            if (side == Side::Buy) {
                bids_levels_[bit_idx].append_order(order);
                bids_bitset_[bit_idx / 64] |= (1ULL << (bit_idx % 64));
                if (!has_bids_ || order->price > best_bid_) {
                    best_bid_ = order->price;
                    has_bids_ = true;
                }
            } else {
                asks_levels_[bit_idx].append_order(order);
                asks_bitset_[bit_idx / 64] |= (1ULL << (bit_idx % 64));
                if (order->price < best_ask_) {
                    best_ask_ = order->price;
                }
            }
        }
    } else {
        order_pool_.deallocate(order);
    }

    return;
}

} // namespace hft
