#include "OrderBook.h"
#if defined(__linux__)
#include <sys/mman.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <cassert>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace hft {

OrderBook::OrderBook(size_t max_order_id, size_t max_active_orders, Price min_price, Price max_price)
    : min_price_(min_price), max_price_(max_price), max_order_id_(max_order_id),
      best_bid_(0), best_ask_(max_price), best_bid_idx_(0), best_ask_idx_(0), has_bids_(false), has_asks_(false),
      order_pool_(max_active_orders)
{
    assert(min_price <= max_price && "min_price cannot be greater than max_price");
    if (min_price > max_price) [[unlikely]] {
        std::abort(); // OrderBook: max_price < min_price
    }
    if (max_price == std::numeric_limits<Price>::max()) [[unlikely]] {
        std::abort(); // OrderBook: max_price saturates Price type (cannot use UINT32_MAX)
    }
    best_ask_ = max_price + 1;
    best_ask_idx_ = max_price + 1 - min_price;
    
    order_map_.resize(max_order_id + 1, INVALID_INDEX);
    order_map_.shrink_to_fit();

    size_t price_range = static_cast<size_t>(max_price) - static_cast<size_t>(min_price) + 1;
    bids_levels_.resize(price_range);
    bids_levels_.shrink_to_fit();
    asks_levels_.resize(price_range);
    asks_levels_.shrink_to_fit();
    
    size_t bitset_words = (price_range + 63) / 64;
    bids_bitset_.resize(bitset_words, 0);
    bids_bitset_.shrink_to_fit();
    asks_bitset_.resize(bitset_words, 0);
    asks_bitset_.shrink_to_fit();
    
    size_t summary_words = (bitset_words + 63) / 64;
    bids_summary_.resize(summary_words, 0);
    bids_summary_.shrink_to_fit();
    asks_summary_.resize(summary_words, 0);
    asks_summary_.shrink_to_fit();

    // Memory Pre-Touching
    // Prevents page faults upon first access of map and levels during live trading
    const size_t map_bytes = order_map_.size() * sizeof(uint32_t);
    const size_t levels_bytes = price_range * sizeof(PriceLevel);
    const size_t bitset_bytes = bitset_words * sizeof(uint64_t);
#if defined(__linux__)
    madvise(order_map_.data(), map_bytes, MADV_POPULATE_READ);
    madvise(bids_levels_.data(), levels_bytes, MADV_POPULATE_READ);
    madvise(asks_levels_.data(), levels_bytes, MADV_POPULATE_READ);
    madvise(bids_bitset_.data(), bitset_bytes, MADV_POPULATE_READ);
    madvise(asks_bitset_.data(), bitset_bytes, MADV_POPULATE_READ);
#elif defined(_WIN32)
    // Windows: Lock pages into physical RAM to prevent page faults
    VirtualLock(order_map_.data(), map_bytes);
    VirtualLock(bids_levels_.data(), levels_bytes);
    VirtualLock(asks_levels_.data(), levels_bytes);
    VirtualLock(bids_bitset_.data(), bitset_bytes);
    VirtualLock(asks_bitset_.data(), bitset_bytes);
    // Also pre-touch all pages by reading, not writing!
    volatile char* p_map = reinterpret_cast<volatile char*>(order_map_.data());
    char dummy = 0;
    for (size_t offset = 0; offset < map_bytes; offset += 4096) {
        dummy ^= p_map[offset];
    }
    volatile char* p_bids = reinterpret_cast<volatile char*>(bids_levels_.data());
    volatile char* p_asks = reinterpret_cast<volatile char*>(asks_levels_.data());
    for (size_t offset = 0; offset < levels_bytes; offset += 4096) {
        dummy ^= p_bids[offset];
        dummy ^= p_asks[offset];
    }
    (void)dummy;
#else
    // Generic fallback: manually pre-touch every page by reading
    volatile char* p_map = reinterpret_cast<volatile char*>(order_map_.data());
    char dummy = 0;
    for (size_t offset = 0; offset < map_bytes; offset += 4096) {
        dummy ^= p_map[offset];
    }
    volatile char* p_bids = reinterpret_cast<volatile char*>(bids_levels_.data());
    volatile char* p_asks = reinterpret_cast<volatile char*>(asks_levels_.data());
    for (size_t offset = 0; offset < levels_bytes; offset += 4096) {
        dummy ^= p_bids[offset];
        dummy ^= p_asks[offset];
    }
    (void)dummy;
#endif
}

} // namespace hft
