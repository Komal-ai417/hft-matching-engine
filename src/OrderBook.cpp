#include "OrderBook.h"

namespace hft {

OrderBook::OrderBook(size_t max_orders, Price min_price, Price max_price)
    : min_price_(min_price), max_price_(max_price),
      best_bid_(0), best_ask_(max_price + 1), has_bids_(false),
      order_pool_(max_orders)
{
    order_map_.resize(max_orders + 1, nullptr);
    size_t price_range = max_price - min_price + 1;
    bids_levels_.resize(price_range);
    asks_levels_.resize(price_range);
    
    size_t bitset_words = (price_range + 63) / 64;
    bids_bitset_.resize(bitset_words, 0);
    asks_bitset_.resize(bitset_words, 0);
    
    for (size_t i = 0; i < price_range; ++i) {
        bids_levels_[i].price = min_price + i;
        asks_levels_[i].price = min_price + i;
    }
}

void OrderBook::update_best_ask_after_remove(Price removed_price) {
    if (best_ask_ == removed_price && asks_levels_[removed_price - min_price_].is_empty()) {
        size_t bit_idx = removed_price - min_price_;
        asks_bitset_[bit_idx / 64] &= ~(1ULL << (bit_idx % 64));
        
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
            best_ask_ = max_price_ + 1;
        }
    }
}

void OrderBook::update_best_bid_after_remove(Price removed_price) {
    if (has_bids_ && best_bid_ == removed_price && bids_levels_[removed_price - min_price_].is_empty()) {
        size_t bit_idx = removed_price - min_price_;
        bids_bitset_[bit_idx / 64] &= ~(1ULL << (bit_idx % 64));
        
        uint64_t word_idx = bit_idx / 64;
        uint64_t mask = (1ULL << (bit_idx % 64)) - 1;
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

void OrderBook::cancel_order(OrderId id) {
    if (id >= order_map_.size()) {
        return;
    }
    if (order_map_[id] == nullptr) {
        return;
    }
    
    Order* order = order_map_[id];
    Price price = order->price;
    
    if (order->side == Side::Buy) {
        bids_levels_[price - min_price_].remove_order(order);
        update_best_bid_after_remove(price);
    } else {
        asks_levels_[price - min_price_].remove_order(order);
        update_best_ask_after_remove(price);
    }
    
    order_map_[id] = nullptr;
    order_pool_.deallocate(order);
    return;
}

} // namespace hft
