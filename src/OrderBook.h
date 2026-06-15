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
    Accepted = 0,
    InvalidQuantity,
    DuplicateOrderId,
    OutOfBoundsOrderId,
    OutOfBoundsPrice,
    CancelFailed,
    PoolExhausted
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
    OrderBook(size_t max_order_id, size_t max_active_orders, Price min_price, Price max_price);
    OrderBook(size_t max_orders, Price min_price, Price max_price)
        : OrderBook(max_orders, max_orders, min_price, max_price) {}

    /**
     * @brief Adds an order to the book.
     * @param on_trade Callback invoked when a trade occurs.
     * 
     * WARNING: Trade callbacks MUST NOT be re-entrant. You cannot call add_order or 
     * cancel_order from within the on_trade callback on the same OrderBook instance.
     */
    template <typename TradeCallback>
    [[nodiscard]] HFT_FORCEINLINE RejectReason add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side, TradeCallback&& on_trade);

    [[nodiscard]] inline RejectReason cancel_order(OrderId id);

private:
    template <Side side, typename TradeCallback>
    HFT_FORCEINLINE void match_order_direct(OrderId taker_id, Price taker_price, Quantity& taker_quantity, TradeCallback&& on_trade);

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
    size_t max_order_id_;
    
    Price best_bid_;
    Price best_ask_;
    size_t best_bid_idx_;
    size_t best_ask_idx_;
    bool has_bids_;
    bool has_asks_;
    
    std::vector<uint32_t> order_map_;
    MemoryPool<Order> order_pool_;
};

// ====================================================================
// TEMPLATE IMPLEMENTATIONS
// ====================================================================

template <Side side, typename TradeCallback>
HFT_FORCEINLINE void OrderBook::match_order_direct(OrderId taker_id, Price taker_price, Quantity& taker_quantity, TradeCallback&& on_trade) {
    // Enforce noexcept contract on trade callback (B-6).
    // If the callback throws after partial matching, the book is left in an
    // inconsistent state with no recovery path.
    static_assert(
        noexcept(std::declval<TradeCallback>()(std::declval<const Trade&>())),
        "Trade callback must be noexcept — throwing during matching corrupts the order book"
    );

    Order* const pool_data = order_pool_.data();
    uint32_t* const order_map_ptr = order_map_.data();
    const size_t asks_summary_size = asks_summary_.size();
    const size_t bids_summary_size = bids_summary_.size();

    if constexpr (side == Side::Buy) {
        PriceLevel* const asks_levels_ptr = asks_levels_.data();
        uint64_t* const asks_bitset_ptr = asks_bitset_.data();
        uint64_t* const asks_summary_ptr = asks_summary_.data();

        // Cross against Asks (lowest to highest)
        while (has_asks_ && taker_quantity > 0 && taker_price >= best_ask_) {
            PriceLevel& level = asks_levels_ptr[best_ask_idx_];

            // ── Fast path: taker can consume the ENTIRE level ──
            if (taker_quantity >= level.total_quantity) {
                Quantity level_qty = level.total_quantity;
                uint32_t maker_order_idx = level.head;

                if (level.order_count == 1) {
                    // One-order fast path
                    Order& maker_order = pool_data[maker_order_idx];
                    on_trade(Trade{maker_order.id, taker_id, maker_order.price, maker_order.quantity});
                    order_map_ptr[maker_order.id] = INVALID_INDEX;
                    order_pool_.deallocate(maker_order_idx);
                } else {
                    if (maker_order_idx != INVALID_INDEX) HFT_PREFETCH(&order_map_ptr[pool_data[maker_order_idx].id], 1, 3);
                    while (maker_order_idx != INVALID_INDEX) {
                        Order& maker_order = pool_data[maker_order_idx];
                        uint32_t next_maker_idx = maker_order.next;
                        if (next_maker_idx != INVALID_INDEX) {
                            HFT_PREFETCH(&pool_data[next_maker_idx], 0, 3);
                            HFT_PREFETCH(&order_map_ptr[pool_data[next_maker_idx].id], 1, 3);
                        }
                        on_trade(Trade{maker_order.id, taker_id, maker_order.price, maker_order.quantity});
                        order_map_ptr[maker_order.id] = INVALID_INDEX;
                        maker_order_idx = next_maker_idx;
                    }
                    order_pool_.deallocate_chain(level.head, level.tail);
                }
                
                taker_quantity -= level_qty;
                
                // Bulk-reset level (avoids N individual remove_order calls)
                level.head = INVALID_INDEX;
                level.tail = INVALID_INDEX;
                level.total_quantity = 0;
                level.order_count = 0;
            } else {
                // ── Slow path: partial fill within this level ──
                uint32_t maker_order_idx = level.head;
                if (maker_order_idx != INVALID_INDEX) HFT_PREFETCH(&order_map_ptr[pool_data[maker_order_idx].id], 1, 3);
                while (maker_order_idx != INVALID_INDEX && taker_quantity > 0) {
                    Order& maker_order = pool_data[maker_order_idx];
                    Quantity trade_qty = taker_quantity < maker_order.quantity ? taker_quantity : maker_order.quantity;
                    on_trade(Trade{maker_order.id, taker_id, maker_order.price, trade_qty});

                    taker_quantity -= trade_qty;
                    maker_order.quantity -= trade_qty;
                    level.total_quantity -= trade_qty;

                    uint32_t next_maker_idx = maker_order.next;
                    if (next_maker_idx != INVALID_INDEX) {
                        HFT_PREFETCH(&pool_data[next_maker_idx], 0, 3);
                        HFT_PREFETCH(&order_map_ptr[pool_data[next_maker_idx].id], 1, 3);
                    }

                    if (maker_order.quantity == 0) [[likely]] {
                        order_map_ptr[maker_order.id] = INVALID_INDEX;
                        order_pool_.deallocate(maker_order_idx);
                        --level.order_count;
                        level.head = next_maker_idx;
                        if (next_maker_idx != INVALID_INDEX) {
                            pool_data[next_maker_idx].prev = INVALID_INDEX;
                        } else {
                            level.tail = INVALID_INDEX;
                        }
                    }
                    maker_order_idx = next_maker_idx;
                }
            }

            if (HFT_UNLIKELY(level.is_empty())) {
                size_t bit_idx = best_ask_ - min_price_;
                size_t word_offset = bit_idx / 64;
                uint64_t bit_mask = 1ULL << (bit_idx % 64);
                asks_bitset_ptr[word_offset] &= ~bit_mask;
                if (asks_bitset_ptr[word_offset] == 0) {
                    asks_summary_ptr[word_offset / 64] &= ~(1ULL << (word_offset % 64));
                }

                Price next_ask = best_ask_ + 1;
                size_t next_bit_idx = bit_idx + 1;
                if (next_ask <= max_price_ && !asks_levels_ptr[next_bit_idx].is_empty()) {
                    best_ask_ = next_ask;
                    best_ask_idx_ = next_bit_idx;
                } else {
                    // Fast-forward best_ask_ using bitset
                    uint64_t word_idx = bit_idx / 64;
                    uint64_t mask = ~((1ULL << (bit_idx % 64)) - 1);
                    uint64_t word = asks_bitset_ptr[word_idx] & mask;

                    if (word != 0) {
                        best_ask_idx_ = word_idx * 64 + std::countr_zero(word);
                        best_ask_ = min_price_ + best_ask_idx_;
                    } else {
                        has_asks_ = false;
                        best_ask_ = max_price_ + 1; // assume not found
                        best_ask_idx_ = max_price_ + 1 - min_price_;
                        uint64_t summary_idx = word_idx / 64;
                        uint64_t k = word_idx % 64;
                        uint64_t summary_mask = ~((((1ULL << k) - 1) << 1) | 1);
                        uint64_t summary_word = asks_summary_ptr[summary_idx] & summary_mask;
                        
                        if (summary_word != 0) {
                            size_t next_word_idx = summary_idx * 64 + std::countr_zero(summary_word);
                            best_ask_idx_ = next_word_idx * 64 + std::countr_zero(asks_bitset_ptr[next_word_idx]);
                            best_ask_ = min_price_ + best_ask_idx_;
                            has_asks_ = true;
                        } else {
                            for (size_t i = summary_idx + 1; i < asks_summary_size; ++i) {
                                if (asks_summary_ptr[i] != 0) {
                                    size_t next_word_idx = i * 64 + std::countr_zero(asks_summary_ptr[i]);
                                    best_ask_idx_ = next_word_idx * 64 + std::countr_zero(asks_bitset_ptr[next_word_idx]);
                                    best_ask_ = min_price_ + best_ask_idx_;
                                    has_asks_ = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    } else {
        PriceLevel* const bids_levels_ptr = bids_levels_.data();
        uint64_t* const bids_bitset_ptr = bids_bitset_.data();
        uint64_t* const bids_summary_ptr = bids_summary_.data();

        // Cross against Bids (highest to lowest)
        while (has_bids_ && taker_quantity > 0 && taker_price <= best_bid_) {
            PriceLevel& level = bids_levels_ptr[best_bid_idx_];

            // ── Fast path: taker can consume the ENTIRE level ──
            if (taker_quantity >= level.total_quantity) {
                Quantity level_qty = level.total_quantity;
                uint32_t maker_order_idx = level.head;
                
                if (level.order_count == 1) {
                    // One-order fast path
                    Order& maker_order = pool_data[maker_order_idx];
                    on_trade(Trade{maker_order.id, taker_id, maker_order.price, maker_order.quantity});
                    order_map_ptr[maker_order.id] = INVALID_INDEX;
                    order_pool_.deallocate(maker_order_idx);
                } else {
                    if (maker_order_idx != INVALID_INDEX) HFT_PREFETCH(&order_map_ptr[pool_data[maker_order_idx].id], 1, 3);
                    while (maker_order_idx != INVALID_INDEX) {
                        Order& maker_order = pool_data[maker_order_idx];
                        uint32_t next_maker_idx = maker_order.next;
                        if (next_maker_idx != INVALID_INDEX) {
                            HFT_PREFETCH(&pool_data[next_maker_idx], 0, 3);
                            HFT_PREFETCH(&order_map_ptr[pool_data[next_maker_idx].id], 1, 3);
                        }
                        on_trade(Trade{maker_order.id, taker_id, maker_order.price, maker_order.quantity});
                        order_map_ptr[maker_order.id] = INVALID_INDEX;
                        maker_order_idx = next_maker_idx;
                    }
                    // Bulk deallocate the level's orders!
                    order_pool_.deallocate_chain(level.head, level.tail);
                }
                
                taker_quantity -= level_qty;
                
                // Bulk-reset level
                level.head = INVALID_INDEX;
                level.tail = INVALID_INDEX;
                level.total_quantity = 0;
                level.order_count = 0;
            } else {
                // ── Slow path: partial fill within this level ──
                uint32_t maker_order_idx = level.head;
                if (maker_order_idx != INVALID_INDEX) HFT_PREFETCH(&order_map_ptr[pool_data[maker_order_idx].id], 1, 3);
                while (maker_order_idx != INVALID_INDEX && taker_quantity > 0) {
                    Order& maker_order = pool_data[maker_order_idx];
                    Quantity trade_qty = taker_quantity < maker_order.quantity ? taker_quantity : maker_order.quantity;
                    on_trade(Trade{maker_order.id, taker_id, maker_order.price, trade_qty});

                    taker_quantity -= trade_qty;
                    maker_order.quantity -= trade_qty;
                    level.total_quantity -= trade_qty;

                    uint32_t next_maker_idx = maker_order.next;
                    if (next_maker_idx != INVALID_INDEX) {
                        HFT_PREFETCH(&pool_data[next_maker_idx], 0, 3);
                        HFT_PREFETCH(&order_map_ptr[pool_data[next_maker_idx].id], 1, 3);
                    }

                    if (maker_order.quantity == 0) [[likely]] {
                        order_map_ptr[maker_order.id] = INVALID_INDEX;
                        order_pool_.deallocate(maker_order_idx);
                        --level.order_count;
                        level.head = next_maker_idx;
                        if (next_maker_idx != INVALID_INDEX) {
                            pool_data[next_maker_idx].prev = INVALID_INDEX;
                        } else {
                            level.tail = INVALID_INDEX;
                        }
                    }
                    maker_order_idx = next_maker_idx;
                }
            }

            if (HFT_UNLIKELY(level.is_empty())) {
                size_t bit_idx = best_bid_ - min_price_;
                size_t word_offset = bit_idx / 64;
                uint64_t bit_mask = 1ULL << (bit_idx % 64);
                bids_bitset_ptr[word_offset] &= ~bit_mask;
                if (bids_bitset_ptr[word_offset] == 0) {
                    bids_summary_ptr[word_offset / 64] &= ~(1ULL << (word_offset % 64));
                }

                if (best_bid_ > min_price_ && !bids_levels_ptr[bit_idx - 1].is_empty()) {
                    best_bid_--;
                    best_bid_idx_--;
                } else {
                    // Fast-forward best_bid_ using bitset (scan downwards)
                    uint64_t word_idx = bit_idx / 64;
                    uint64_t mask = (1ULL << (bit_idx % 64)) - 1;
                    uint64_t word = bids_bitset_ptr[word_idx] & mask;

                    if (word != 0) {
                        best_bid_idx_ = word_idx * 64 + 63 - std::countl_zero(word);
                        best_bid_ = min_price_ + best_bid_idx_;
                    } else {
                        has_bids_ = false;
                        best_bid_ = 0;
                        best_bid_idx_ = 0;
                        uint64_t summary_idx = word_idx / 64;
                        uint64_t summary_mask = (1ULL << (word_idx % 64)) - 1;
                        uint64_t summary_word = bids_summary_ptr[summary_idx] & summary_mask;
                        
                        if (summary_word != 0) {
                            size_t next_word_idx = summary_idx * 64 + 63 - std::countl_zero(summary_word);
                            best_bid_idx_ = next_word_idx * 64 + 63 - std::countl_zero(bids_bitset_ptr[next_word_idx]);
                            best_bid_ = min_price_ + best_bid_idx_;
                            has_bids_ = true;
                        } else {
                            for (int64_t i = static_cast<int64_t>(summary_idx) - 1; i >= 0; --i) {
                                if (bids_summary_ptr[i] != 0) {
                                    size_t next_word_idx = i * 64 + 63 - std::countl_zero(bids_summary_ptr[i]);
                                    best_bid_idx_ = next_word_idx * 64 + 63 - std::countl_zero(bids_bitset_ptr[next_word_idx]);
                                    best_bid_ = min_price_ + best_bid_idx_;
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
}

HFT_FORCEINLINE void OrderBook::update_best_ask_after_remove(Price removed_price) {
    assert(has_asks_ && best_ask_ == removed_price && "update_best_ask_after_remove must be called from the best ask level");
    size_t bit_idx = removed_price - min_price_;
    uint64_t word_idx = bit_idx / 64;
    uint64_t mask = ~((1ULL << (bit_idx % 64)) - 1);
    const uint64_t* const asks_bitset_ptr = asks_bitset_.data();
    uint64_t word = asks_bitset_ptr[word_idx] & mask;
    
    if (word != 0) {
        best_ask_idx_ = word_idx * 64 + std::countr_zero(word);
        best_ask_ = min_price_ + best_ask_idx_;
        has_asks_ = true;
        return;
    }
    
    uint64_t summary_idx = word_idx / 64;
    uint64_t k = word_idx % 64;
    uint64_t summary_mask = ~((((1ULL << k) - 1) << 1) | 1);
    const uint64_t* const asks_summary_ptr = asks_summary_.data();
    uint64_t summary_word = asks_summary_ptr[summary_idx] & summary_mask;
    const size_t asks_summary_size = asks_summary_.size();
    
    if (summary_word != 0) {
        size_t next_word_idx = summary_idx * 64 + std::countr_zero(summary_word);
        best_ask_idx_ = next_word_idx * 64 + std::countr_zero(asks_bitset_ptr[next_word_idx]);
        best_ask_ = min_price_ + best_ask_idx_;
        has_asks_ = true;
        return;
    }
    
    for (size_t i = summary_idx + 1; i < asks_summary_size; ++i) {
        if (asks_summary_ptr[i] != 0) {
            size_t next_word_idx = i * 64 + std::countr_zero(asks_summary_ptr[i]);
            best_ask_idx_ = next_word_idx * 64 + std::countr_zero(asks_bitset_ptr[next_word_idx]);
            best_ask_ = min_price_ + best_ask_idx_;
            has_asks_ = true;
            return;
        }
    }
    has_asks_ = false;
    best_ask_ = max_price_ + 1;
    best_ask_idx_ = max_price_ + 1 - min_price_;
}

HFT_FORCEINLINE void OrderBook::update_best_bid_after_remove(Price removed_price) {
    assert(has_bids_ && best_bid_ == removed_price && "update_best_bid_after_remove must be called from the best bid level");
    size_t bit_idx = removed_price - min_price_;
    uint64_t word_idx = bit_idx / 64;
    uint64_t mask = (1ULL << (bit_idx % 64)) - 1;
    const uint64_t* const bids_bitset_ptr = bids_bitset_.data();
    uint64_t word = bids_bitset_ptr[word_idx] & mask;
    
    if (word != 0) {
        best_bid_idx_ = word_idx * 64 + 63 - std::countl_zero(word);
        best_bid_ = min_price_ + best_bid_idx_;
        has_bids_ = true;
        return;
    }
    
    uint64_t summary_idx = word_idx / 64;
    uint64_t summary_mask = (1ULL << (word_idx % 64)) - 1;
    const uint64_t* const bids_summary_ptr = bids_summary_.data();
    uint64_t summary_word = bids_summary_ptr[summary_idx] & summary_mask;
    
    if (summary_word != 0) {
        size_t next_word_idx = summary_idx * 64 + 63 - std::countl_zero(summary_word);
        best_bid_idx_ = next_word_idx * 64 + 63 - std::countl_zero(bids_bitset_ptr[next_word_idx]);
        best_bid_ = min_price_ + best_bid_idx_;
        return;
    }
    
    for (int64_t i = static_cast<int64_t>(summary_idx) - 1; i >= 0; --i) {
        if (bids_summary_ptr[i] != 0) {
            size_t next_word_idx = i * 64 + 63 - std::countl_zero(bids_summary_ptr[i]);
            best_bid_idx_ = next_word_idx * 64 + 63 - std::countl_zero(bids_bitset_ptr[next_word_idx]);
            best_bid_ = min_price_ + best_bid_idx_;
            return;
        }
    }
    has_bids_ = false;
    best_bid_ = 0;
    best_bid_idx_ = 0;
}

[[nodiscard]] inline RejectReason OrderBook::cancel_order(OrderId id) {
    if (HFT_UNLIKELY(id == 0 || id > max_order_id_)) {
        return RejectReason::OutOfBoundsOrderId;
    }
    uint32_t* const order_map_ptr = order_map_.data();
    if (HFT_UNLIKELY(order_map_ptr[id] == INVALID_INDEX)) {
        return RejectReason::CancelFailed;
    }
    
    // Prefetch map location for nearby ID to reduce cache miss penalty in sequential patterns
    if (id + 16 <= max_order_id_) {
        HFT_PREFETCH(&order_map_ptr[id + 16], 1, 3);
    }
    
    uint32_t order_idx = order_map_ptr[id];
    Order* const pool_ptr = order_pool_.data();
    Order& order = pool_ptr[order_idx];
    Price price = order.price;
    size_t bit_idx = price - min_price_;
    
    if (order.side == Side::Buy) {
        bids_levels_[bit_idx].remove_order(order_idx, pool_ptr);
        if (HFT_UNLIKELY(bids_levels_[bit_idx].is_empty())) {
            size_t word_offset = bit_idx / 64;
            uint64_t bit_mask = 1ULL << (bit_idx % 64);
            uint64_t* const bids_bitset_ptr = bids_bitset_.data();
            bids_bitset_ptr[word_offset] &= ~bit_mask;
            if (bids_bitset_ptr[word_offset] == 0) {
                bids_summary_.data()[word_offset / 64] &= ~(1ULL << (word_offset % 64));
            }
            if (has_bids_ && best_bid_ == price) {
                update_best_bid_after_remove(price);
            }
        }
    } else {
        asks_levels_[bit_idx].remove_order(order_idx, pool_ptr);
        if (HFT_UNLIKELY(asks_levels_[bit_idx].is_empty())) {
            size_t word_offset = bit_idx / 64;
            uint64_t bit_mask = 1ULL << (bit_idx % 64);
            uint64_t* const asks_bitset_ptr = asks_bitset_.data();
            asks_bitset_ptr[word_offset] &= ~bit_mask;
            if (asks_bitset_ptr[word_offset] == 0) {
                asks_summary_.data()[word_offset / 64] &= ~(1ULL << (word_offset % 64));
            }
            if (best_ask_ == price) {
                update_best_ask_after_remove(price);
            }
        }
    }
    
    order_map_ptr[id] = INVALID_INDEX;
    order_pool_.deallocate(order_idx);
    
    return RejectReason::Accepted;
}

template <typename TradeCallback>
[[nodiscard]] HFT_FORCEINLINE RejectReason OrderBook::add_order(OrderId id, OrderType type, Price price, Quantity quantity, Side side, TradeCallback&& on_trade) {
    if (type == OrderType::Cancel) [[unlikely]] {
        return cancel_order(id);
    }

    if (quantity == 0) [[unlikely]] {
        return RejectReason::InvalidQuantity;
    }

    if (id == 0 || id > max_order_id_) [[unlikely]] {
        return RejectReason::OutOfBoundsOrderId;
    }
    
    // Removed: guard already handled by early return above.
    // HFT_ASSUME was redundant and introduced UB risk if guard is ever refactored.

    uint32_t* const order_map_ptr = order_map_.data();
    if (order_map_ptr[id] != INVALID_INDEX) [[unlikely]] {
        return RejectReason::DuplicateOrderId;
    }

    // Prefetch map location for sequential inserts
    if (id + 16 <= max_order_id_) {
        HFT_PREFETCH(&order_map_ptr[id + 16], 1, 3);
    }

    if (type == OrderType::Limit) {
        if (price < min_price_ || price > max_price_) {
            return RejectReason::OutOfBoundsPrice;
        }
    }

    // Removed: guard already handled by early return above.

    Price resolved_price = type == OrderType::Market
                       ? (side == Side::Buy ? MARKET_BUY_PRICE : MARKET_SELL_PRICE)
                       : price;
    Quantity remaining_qty = quantity;

    if (side == Side::Buy) {
        match_order_direct<Side::Buy>(id, resolved_price, remaining_qty, std::forward<TradeCallback>(on_trade));
    } else {
        match_order_direct<Side::Sell>(id, resolved_price, remaining_qty, std::forward<TradeCallback>(on_trade));
    }

    if (remaining_qty > 0) {
        if (type == OrderType::Market) {
            // Market order cannot be posted, so remaining is discarded
        } else {
            uint32_t order_idx = order_pool_.allocate();
            if (HFT_UNLIKELY(order_idx == INVALID_INDEX)) {
                return RejectReason::PoolExhausted; // Pool exhausted, order cannot be posted.
            }
            Order* const pool_data = order_pool_.data();
            Order& order = pool_data[order_idx];
            order.id = id;
            order.price = resolved_price;
            order.quantity = remaining_qty;
            order.side = side;
            order.next = INVALID_INDEX;
            order.prev = INVALID_INDEX;

            order_map_ptr[id] = order_idx;
            size_t bit_idx = order.price - min_price_;
            size_t word_offset = bit_idx / 64;
            uint64_t bit_mask = 1ULL << (bit_idx % 64);
            if (side == Side::Buy) {
                bool was_empty = bids_levels_[bit_idx].is_empty();
                bids_levels_[bit_idx].append_order(order_idx, pool_data);
                if (was_empty) {
                    uint64_t* const bids_bitset_ptr = bids_bitset_.data();
                    bids_bitset_ptr[word_offset] |= bit_mask;
                    bids_summary_.data()[word_offset / 64] |= (1ULL << (word_offset % 64));
                }
                
                if (!has_bids_ || order.price > best_bid_) {
                    best_bid_ = order.price;
                    best_bid_idx_ = bit_idx;
                    has_bids_ = true;
                }
            } else {
                bool was_empty = asks_levels_[bit_idx].is_empty();
                asks_levels_[bit_idx].append_order(order_idx, pool_data);
                if (was_empty) {
                    uint64_t* const asks_bitset_ptr = asks_bitset_.data();
                    asks_bitset_ptr[word_offset] |= bit_mask;
                    asks_summary_.data()[word_offset / 64] |= (1ULL << (word_offset % 64));
                }
                
                if (!has_asks_ || order.price < best_ask_) {
                    best_ask_ = order.price;
                    best_ask_idx_ = bit_idx;
                    has_asks_ = true;
                }
            }
        }
    }

    return RejectReason::Accepted;
}

} // namespace hft
