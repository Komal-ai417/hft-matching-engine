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
    
    size_t summary_words = (bitset_words + 63) / 64;
    bids_summary_.resize(summary_words, 0);
    asks_summary_.resize(summary_words, 0);
    
    for (size_t i = 0; i < price_range; ++i) {
        bids_levels_[i].price = min_price + i;
        asks_levels_[i].price = min_price + i;
    }
}




} // namespace hft
