#pragma once

#include "Order.h"
#include "PriceLevel.h"
#include "MemoryPool.h"
#include <vector>
#include <bit>
#include <algorithm>
#include <cassert>

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

#include "Macros.h"

class OrderBook {
public:
    explicit OrderBook(size_t max_orders, Price min_price, Price max_price);

    template <typename TradeCallback>
    HFT_FORCEINLINE void add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side, TradeCallback&& on_trade);

    inline void cancel_order(OrderId id);

private:
    template <Side side, typename TradeCallback>
    HFT_FORCEINLINE void match_order(Order* taker_order, TradeCallback&& on_trade);

    HFT_FORCEINLINE void update_best_bid_after_remove(Price removed_price);
    HFT_FORCEINLINE void update_best_ask_after_remove(Price removed_price);
    
    std::vector<uint64_t> bids_bitset_;
    std::vector<uint64_t> asks_bitset_;
    std::vector<uint64_t> bids_summary_;
    std::vector<uint64_t> asks_summary_;

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
HFT_FORCEINLINE void OrderBook::match_order(Order* taker_order, TradeCallback&& on_trade) {
    if constexpr (side == Side::Buy) {
        // Cross against Asks (lowest to highest)
        while (best_ask_ <= max_price_ && taker_order->quantity > 0 && taker_order->price >= best_ask_) {
            PriceLevel& level = asks_levels_[best_ask_ - min_price_];

            // ── Fast path: taker can consume the ENTIRE level ──
            if (taker_order->quantity >= level.total_quantity) {
                Quantity level_qty = level.total_quantity;
                Order* maker_order = level.head;
                if (maker_order) HFT_PREFETCH(&order_map_[maker_order->id], 1, 3);
                while (maker_order != nullptr) {
                    Order* next_maker = maker_order->next;
                    if (next_maker) {
                        HFT_PREFETCH(next_maker, 0, 3);
                        HFT_PREFETCH(&order_map_[next_maker->id], 1, 3);
                    }
                    on_trade(Trade{maker_order->id, taker_order->id, maker_order->price, maker_order->quantity});
                    order_map_[maker_order->id] = nullptr;
                    order_pool_.deallocate(maker_order);
                    maker_order = next_maker;
                }
                taker_order->quantity -= level_qty;
                // Bulk-reset level (avoids N individual remove_order calls)
                level.head = nullptr;
                level.tail = nullptr;
                level.total_quantity = 0;
                level.order_count = 0;
            } else {
                // ── Slow path: partial fill within this level ──
                Order* maker_order = level.head;
                if (maker_order) HFT_PREFETCH(&order_map_[maker_order->id], 1, 3);
                while (maker_order != nullptr && taker_order->quantity > 0) {
                    Quantity trade_qty = std::min(taker_order->quantity, maker_order->quantity);
                    on_trade(Trade{maker_order->id, taker_order->id, maker_order->price, trade_qty});

                    taker_order->quantity -= trade_qty;
                    maker_order->quantity -= trade_qty;
                    level.total_quantity -= trade_qty;

                    Order* next_maker = maker_order->next;
                    if (next_maker) {
                        HFT_PREFETCH(next_maker, 0, 3);
                        HFT_PREFETCH(&order_map_[next_maker->id], 1, 3);
                    }

                    if (maker_order->quantity == 0) [[likely]] {
                        // Unlink from list: we know prev was already handled
                        order_map_[maker_order->id] = nullptr;
                        order_pool_.deallocate(maker_order);
                        --level.order_count;
                        // Fixup head — next_maker becomes the new head of remaining orders
                        level.head = next_maker;
                        if (next_maker) {
                            next_maker->prev = nullptr;
                        } else {
                            level.tail = nullptr;
                        }
                    }
                    maker_order = next_maker;
                }
            }

            if (HFT_UNLIKELY(level.is_empty())) {
                size_t bit_idx = best_ask_ - min_price_;
                asks_bitset_[bit_idx / 64] &= ~(1ULL << (bit_idx % 64));
                if (asks_bitset_[bit_idx / 64] == 0) {
                    asks_summary_[(bit_idx / 64) / 64] &= ~(1ULL << ((bit_idx / 64) % 64));
                }

                // Fast-forward best_ask_ using bitset
                uint64_t word_idx = bit_idx / 64;
                uint64_t mask = ~((1ULL << (bit_idx % 64)) - 1);
                uint64_t word = asks_bitset_[word_idx] & mask;

                if (word != 0) {
                    best_ask_ = min_price_ + word_idx * 64 + std::countr_zero(word);
                } else {
                    best_ask_ = max_price_ + 1; // assume not found
                    uint64_t summary_idx = word_idx / 64;
                    uint64_t summary_mask = ~(((1ULL << (word_idx % 64)) << 1) - 1);
                    uint64_t summary_word = asks_summary_[summary_idx] & summary_mask;
                    
                    if (summary_word != 0) {
                        size_t next_word_idx = summary_idx * 64 + std::countr_zero(summary_word);
                        best_ask_ = min_price_ + next_word_idx * 64 + std::countr_zero(asks_bitset_[next_word_idx]);
                    } else {
                        for (size_t i = summary_idx + 1; i < asks_summary_.size(); ++i) {
                            if (asks_summary_[i] != 0) {
                                size_t next_word_idx = i * 64 + std::countr_zero(asks_summary_[i]);
                                best_ask_ = min_price_ + next_word_idx * 64 + std::countr_zero(asks_bitset_[next_word_idx]);
                                break;
                            }
                        }
                    }
                }
            }
        }
    } else {
        // Cross against Bids (highest to lowest)
        while (has_bids_ && taker_order->quantity > 0 && taker_order->price <= best_bid_) {
            PriceLevel& level = bids_levels_[best_bid_ - min_price_];

            // ── Fast path: taker can consume the ENTIRE level ──
            if (taker_order->quantity >= level.total_quantity) {
                Quantity level_qty = level.total_quantity;
                Order* maker_order = level.head;
                if (maker_order) HFT_PREFETCH(&order_map_[maker_order->id], 1, 3);
                while (maker_order != nullptr) {
                    Order* next_maker = maker_order->next;
                    if (next_maker) {
                        HFT_PREFETCH(next_maker, 0, 3);
                        HFT_PREFETCH(&order_map_[next_maker->id], 1, 3);
                    }
                    on_trade(Trade{maker_order->id, taker_order->id, maker_order->price, maker_order->quantity});
                    order_map_[maker_order->id] = nullptr;
                    order_pool_.deallocate(maker_order);
                    maker_order = next_maker;
                }
                taker_order->quantity -= level_qty;
                // Bulk-reset level
                level.head = nullptr;
                level.tail = nullptr;
                level.total_quantity = 0;
                level.order_count = 0;
            } else {
                // ── Slow path: partial fill within this level ──
                Order* maker_order = level.head;
                if (maker_order) HFT_PREFETCH(&order_map_[maker_order->id], 1, 3);
                while (maker_order != nullptr && taker_order->quantity > 0) {
                    Quantity trade_qty = std::min(taker_order->quantity, maker_order->quantity);
                    on_trade(Trade{maker_order->id, taker_order->id, maker_order->price, trade_qty});

                    taker_order->quantity -= trade_qty;
                    maker_order->quantity -= trade_qty;
                    level.total_quantity -= trade_qty;

                    Order* next_maker = maker_order->next;
                    if (next_maker) {
                        HFT_PREFETCH(next_maker, 0, 3);
                        HFT_PREFETCH(&order_map_[next_maker->id], 1, 3);
                    }

                    if (maker_order->quantity == 0) [[likely]] {
                        order_map_[maker_order->id] = nullptr;
                        order_pool_.deallocate(maker_order);
                        --level.order_count;
                        level.head = next_maker;
                        if (next_maker) {
                            next_maker->prev = nullptr;
                        } else {
                            level.tail = nullptr;
                        }
                    }
                    maker_order = next_maker;
                }
            }

            if (HFT_UNLIKELY(level.is_empty())) {
                size_t bit_idx = best_bid_ - min_price_;
                bids_bitset_[bit_idx / 64] &= ~(1ULL << (bit_idx % 64));
                if (bids_bitset_[bit_idx / 64] == 0) {
                    bids_summary_[(bit_idx / 64) / 64] &= ~(1ULL << ((bit_idx / 64) % 64));
                }

                // Fast-forward best_bid_ using bitset (scan downwards)
                uint64_t word_idx = bit_idx / 64;
                uint64_t mask = (1ULL << (bit_idx % 64)) - 1;
                uint64_t word = bids_bitset_[word_idx] & mask;

                if (word != 0) {
                    best_bid_ = min_price_ + word_idx * 64 + 63 - std::countl_zero(word);
                } else {
                    has_bids_ = false;
                    best_bid_ = 0;
                    uint64_t summary_idx = word_idx / 64;
                    uint64_t summary_mask = (1ULL << (word_idx % 64)) - 1;
                    uint64_t summary_word = bids_summary_[summary_idx] & summary_mask;
                    
                    if (summary_word != 0) {
                        size_t next_word_idx = summary_idx * 64 + 63 - std::countl_zero(summary_word);
                        best_bid_ = min_price_ + next_word_idx * 64 + 63 - std::countl_zero(bids_bitset_[next_word_idx]);
                        has_bids_ = true;
                    } else {
                        for (int64_t i = summary_idx - 1; i >= 0; --i) {
                            if (bids_summary_[i] != 0) {
                                size_t next_word_idx = i * 64 + 63 - std::countl_zero(bids_summary_[i]);
                                best_bid_ = min_price_ + next_word_idx * 64 + 63 - std::countl_zero(bids_bitset_[next_word_idx]);
                                has_bids_ = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

HFT_FORCEINLINE void OrderBook::update_best_ask_after_remove(Price removed_price) {
    assert(best_ask_ == removed_price && "update_best_ask_after_remove must be called from the best ask level");
    size_t bit_idx = removed_price - min_price_;
    uint64_t word_idx = bit_idx / 64;
    uint64_t mask = ~((1ULL << (bit_idx % 64)) - 1);
    uint64_t word = asks_bitset_[word_idx] & mask;
    
    if (word != 0) {
        best_ask_ = min_price_ + word_idx * 64 + std::countr_zero(word);
        return;
    }
    
    uint64_t summary_idx = word_idx / 64;
    uint64_t summary_mask = ~(((1ULL << (word_idx % 64)) << 1) - 1);
    uint64_t summary_word = asks_summary_[summary_idx] & summary_mask;
    
    if (summary_word != 0) {
        size_t next_word_idx = summary_idx * 64 + std::countr_zero(summary_word);
        best_ask_ = min_price_ + next_word_idx * 64 + std::countr_zero(asks_bitset_[next_word_idx]);
        return;
    }
    
    for (size_t i = summary_idx + 1; i < asks_summary_.size(); ++i) {
        if (asks_summary_[i] != 0) {
            size_t next_word_idx = i * 64 + std::countr_zero(asks_summary_[i]);
            best_ask_ = min_price_ + next_word_idx * 64 + std::countr_zero(asks_bitset_[next_word_idx]);
            return;
        }
    }
    best_ask_ = max_price_ + 1;
}

HFT_FORCEINLINE void OrderBook::update_best_bid_after_remove(Price removed_price) {
    assert(has_bids_ && best_bid_ == removed_price && "update_best_bid_after_remove must be called from the best bid level");
    size_t bit_idx = removed_price - min_price_;
    uint64_t word_idx = bit_idx / 64;
    uint64_t mask = (1ULL << (bit_idx % 64)) - 1;
    uint64_t word = bids_bitset_[word_idx] & mask;
    
    if (word != 0) {
        best_bid_ = min_price_ + word_idx * 64 + 63 - std::countl_zero(word);
        has_bids_ = true;
        return;
    }
    
    uint64_t summary_idx = word_idx / 64;
    uint64_t summary_mask = (1ULL << (word_idx % 64)) - 1;
    uint64_t summary_word = bids_summary_[summary_idx] & summary_mask;
    
    if (summary_word != 0) {
        size_t next_word_idx = summary_idx * 64 + 63 - std::countl_zero(summary_word);
        best_bid_ = min_price_ + next_word_idx * 64 + 63 - std::countl_zero(bids_bitset_[next_word_idx]);
        return;
    }
    
    for (int64_t i = summary_idx - 1; i >= 0; --i) {
        if (bids_summary_[i] != 0) {
            size_t next_word_idx = i * 64 + 63 - std::countl_zero(bids_summary_[i]);
            best_bid_ = min_price_ + next_word_idx * 64 + 63 - std::countl_zero(bids_bitset_[next_word_idx]);
            return;
        }
    }
    has_bids_ = false;
    best_bid_ = 0;
}

inline void OrderBook::cancel_order(OrderId id) {
    if (HFT_UNLIKELY(id >= order_map_.size())) {
        return;
    }
    if (HFT_UNLIKELY(order_map_[id] == nullptr)) {
        return;
    }
    
    Order* order = order_map_[id];
    Price price = order->price;
    size_t bit_idx = price - min_price_;
    
    if (order->side == Side::Buy) {
        bids_levels_[bit_idx].remove_order(order);
        if (HFT_UNLIKELY(bids_levels_[bit_idx].is_empty())) {
            bids_bitset_[bit_idx / 64] &= ~(1ULL << (bit_idx % 64));
            if (bids_bitset_[bit_idx / 64] == 0) {
                bids_summary_[(bit_idx / 64) / 64] &= ~(1ULL << ((bit_idx / 64) % 64));
            }
            if (has_bids_ && best_bid_ == price) {
                update_best_bid_after_remove(price);
            }
        }
    } else {
        asks_levels_[bit_idx].remove_order(order);
        if (HFT_UNLIKELY(asks_levels_[bit_idx].is_empty())) {
            asks_bitset_[bit_idx / 64] &= ~(1ULL << (bit_idx % 64));
            if (asks_bitset_[bit_idx / 64] == 0) {
                asks_summary_[(bit_idx / 64) / 64] &= ~(1ULL << ((bit_idx / 64) % 64));
            }
            if (best_ask_ == price) {
                update_best_ask_after_remove(price);
            }
        }
    }
    
    order_map_[id] = nullptr;
    order_pool_.deallocate(order);
}

template <typename TradeCallback>
HFT_FORCEINLINE void OrderBook::add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side, TradeCallback&& on_trade) {
    if (type == OrderType::Cancel) [[unlikely]] {
        cancel_order(id);
        return;
    }

    if (quantity == 0) [[unlikely]] {
        return;
    }

    const size_t map_size = order_map_.size();
    if (id >= map_size) [[unlikely]] {
        return;
    }
    
    HFT_ASSUME(id < map_size);

    if (order_map_[id] != nullptr) [[unlikely]] {
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
                bids_summary_[(bit_idx / 64) / 64] |= (1ULL << ((bit_idx / 64) % 64));
                
                if (!has_bids_ || order->price > best_bid_) {
                    best_bid_ = order->price;
                    has_bids_ = true;
                }
            } else {
                asks_levels_[bit_idx].append_order(order);
                asks_bitset_[bit_idx / 64] |= (1ULL << (bit_idx % 64));
                asks_summary_[(bit_idx / 64) / 64] |= (1ULL << ((bit_idx / 64) % 64));
                
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
