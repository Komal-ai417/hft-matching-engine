// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Karyampudi Komal
//
// ============================================================================
// OrderBook.cpp — construction, validation and the cross-structural
// invariant checker (validate_book). The matching hot path itself lives in
// the header so it can be inlined into the caller's translation unit.
// ============================================================================
#include "OrderBook.h"
#if defined(__linux__)
#include <sys/mman.h>
#elif defined(_WIN32)
// windows.h's minwindef.h min/max macros break std::numeric_limits::max(),
// std::min and std::max in every translation unit that sees them — suppress
// the macros at the source. CMakeLists.txt also defines NOMINMAX project-wide
// (this guard protects non-CMake consumers of the public headers).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace hft {

namespace {
/// Sanity cap on the price range: ~1 GiB of PriceLevel storage (32 B each).
/// Beyond that the range is almost certainly a unit mix-up (ticks vs cents).
constexpr size_t kMaxPriceRangeTicks = size_t{1} << 26;
constexpr size_t kBytesPerMiB = size_t{1} << 20;
/// Sanity cap on the order-id space: ids are uint32_t and INVALID_INDEX
/// (0xFFFFFFFF) is reserved, so a larger limit can never be addressable.
/// This also closes the `max_order_id + 1 == 0` wraparound (SIZE_MAX would
/// resize the order map to 0 while every id still passes the bounds check).
constexpr size_t kMaxOrderIds = 0xFFFF'FFFE;
}  // namespace

OrderBook::OrderBook(size_t max_order_id, size_t max_active_orders, Price min_price, Price max_price)
    : min_price_(min_price),
      max_price_(max_price),
      max_order_id_(max_order_id),
      best_bid_(0),
      best_ask_(max_price),
      best_bid_idx_(0),
      best_ask_idx_(0),
      has_bids_(false),
      has_asks_(false),
      order_pool_(max_active_orders) {
    // ── Unified fatal-error policy ─────────────────────────────────────
    // Every construction-time validation aborts with a DISTINCT diagnostic
    // message. All of them abort (never throw), so the observable behaviour is
    // the same in every build flavor, including Release's -fno-exceptions.
    if (min_price > max_price) [[unlikely]] {
        std::fprintf(stderr, "OrderBook: fatal - min_price (%u) > max_price (%u)\n", static_cast<unsigned>(min_price),
                     static_cast<unsigned>(max_price));
        std::fflush(stderr);
        std::abort();
    }
    // Parenthesized (max)(): immune to a minwindef.h `max` macro that a
    // consumer's TU may still have active (windows.h included before our
    // headers, without NOMINMAX). Function-like macros cannot expand
    // through the enclosing parentheses.
    if (max_price == (std::numeric_limits<Price>::max)()) [[unlikely]] {
        std::fprintf(stderr,
                     "OrderBook: fatal - max_price saturates the Price type "
                     "(UINT32_MAX is reserved for the empty-ask sentinel)\n");
        std::fflush(stderr);
        std::abort();
    }
    if (max_order_id > kMaxOrderIds) [[unlikely]] {
        std::fprintf(stderr,
                     "OrderBook: fatal - max_order_id (%zu) exceeds the uint32 id-space cap "
                     "(0xFFFF'FFFE); order_map would need ~%zu MB\n",
                     max_order_id, (max_order_id * sizeof(uint32_t)) / kBytesPerMiB);
        std::fflush(stderr);
        std::abort();
    }
    const size_t price_range = static_cast<size_t>(max_price) - static_cast<size_t>(min_price) + 1;
    // Sanity cap: ~1 GiB of PriceLevel storage (32 B each). Beyond that the
    // range is almost certainly a unit mix-up (e.g. ticks vs. cents).
    if (price_range > kMaxPriceRangeTicks) [[unlikely]] {
        std::fprintf(stderr,
                     "OrderBook: fatal - price range %zu ticks exceeds the "
                     "sanity cap (2^26); refusing to allocate ~%zu MB of levels\n",
                     price_range, (price_range * sizeof(PriceLevel)) / kBytesPerMiB);
        std::fflush(stderr);
        std::abort();
    }

    best_ask_ = max_price + 1;
    best_ask_idx_ = price_range - 1;  // always a VALID index
    best_bid_idx_ = 0;

#if HFT_DEBUG_TOOLS
    owner_thread_ = std::this_thread::get_id();  // thread-identity latch
#endif

    order_map_.resize(max_order_id + 1, INVALID_INDEX);
    // resize(n) already yields capacity() == n, so no shrink_to_fit() is
    // needed — and each one would add an allocate/copy/free that also
    // reorders page-faulting relative to the pre-touch pass.

    bids_levels_.resize(price_range);
    asks_levels_.resize(price_range);

    const size_t bitset_words = (price_range + detail::kWordBits - 1) / detail::kWordBits;
    bids_bitset_.resize(bitset_words, 0);
    asks_bitset_.resize(bitset_words, 0);

    const size_t summary_words = (bitset_words + detail::kWordBits - 1) / detail::kWordBits;
    bids_summary_.resize(summary_words, 0);
    asks_summary_.resize(summary_words, 0);

    // Memory Pre-Touching: prevent page faults upon first access during live
    // trading. vector::resize() value-initializes (WRITES) every page already,
    // so this block is belt-and-braces for any future refactor to
    // lazily-allocated structures — but the calls are (a) the correct WRITE
    // policy and (b) checked, with a deterministic per-page touch fallback
    // instead of silent no-ops on old kernels.
    const size_t map_bytes = order_map_.size() * sizeof(uint32_t);
    const size_t levels_bytes = price_range * sizeof(PriceLevel);
    const size_t bitset_bytes = bitset_words * sizeof(uint64_t);
    const size_t summary_bytes = summary_words * sizeof(uint64_t);
#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
    if (madvise(order_map_.data(), map_bytes, MADV_POPULATE_WRITE) != 0 ||
        madvise(bids_levels_.data(), levels_bytes, MADV_POPULATE_WRITE) != 0 ||
        madvise(asks_levels_.data(), levels_bytes, MADV_POPULATE_WRITE) != 0 ||
        madvise(bids_bitset_.data(), bitset_bytes, MADV_POPULATE_WRITE) != 0 ||
        madvise(asks_bitset_.data(), bitset_bytes, MADV_POPULATE_WRITE) != 0 ||
        madvise(bids_summary_.data(), summary_bytes, MADV_POPULATE_WRITE) != 0 ||
        madvise(asks_summary_.data(), summary_bytes, MADV_POPULATE_WRITE) != 0) {
        // Kernel < 5.14 or cgroup pressure: fault every page deterministically
        // here, at construction, instead of on the first trade.
        touch_fallback(order_map_.data(), map_bytes);
        touch_fallback(bids_levels_.data(), levels_bytes);
        touch_fallback(asks_levels_.data(), levels_bytes);
        touch_fallback(bids_bitset_.data(), bitset_bytes);
        touch_fallback(asks_bitset_.data(), bitset_bytes);
        touch_fallback(bids_summary_.data(), summary_bytes);
        touch_fallback(asks_summary_.data(), summary_bytes);
    }
#elif defined(_WIN32)
    if (!VirtualLock(order_map_.data(), map_bytes)) {
        touch_fallback(order_map_.data(), map_bytes);
    }
    if (!VirtualLock(bids_levels_.data(), levels_bytes)) {
        touch_fallback(bids_levels_.data(), levels_bytes);
    }
    if (!VirtualLock(asks_levels_.data(), levels_bytes)) {
        touch_fallback(asks_levels_.data(), levels_bytes);
    }
    if (!VirtualLock(bids_bitset_.data(), bitset_bytes)) {
        touch_fallback(bids_bitset_.data(), bitset_bytes);
    }
    if (!VirtualLock(asks_bitset_.data(), bitset_bytes)) {
        touch_fallback(asks_bitset_.data(), bitset_bytes);
    }
#else
    touch_fallback(order_map_.data(), map_bytes);
    touch_fallback(bids_levels_.data(), levels_bytes);
    touch_fallback(asks_levels_.data(), levels_bytes);
    touch_fallback(bids_bitset_.data(), bitset_bytes);
    touch_fallback(asks_bitset_.data(), bitset_bytes);
#endif
}

// static
void OrderBook::touch_fallback(void* base, size_t bytes) noexcept {
    // The fallback must be READ-based. A write-based touch (`p[offset] = 0`)
    // DESTRUCTIVELY zeroes the first byte of every page-aligned object —
    // corrupting PriceLevel::head and pool free-list links.
    // vector::resize() already WROTE every page; the fallback only needs to
    // guarantee the pages are resident, which a volatile read achieves.
    const auto* p = static_cast<volatile const unsigned char*>(base);
    unsigned char sink = 0;
    for (size_t offset = 0; offset < bytes; offset += detail::kPageSize) {
        sink ^= p[offset];
    }
    (void)sink;
}

#if HFT_DEBUG_TOOLS
bool OrderBook::validate_book() const {
    bool ok = true;
    const auto fail = [&ok](const char* what, size_t a, size_t b) {
        std::fprintf(stderr, "[HFT_VALIDATE] invariant violated: %s (%zu vs %zu)\n", what, a, b);
        ok = false;
    };

    const Order* const pool_data = order_pool_.data();
    const uint32_t* const map_ptr = order_map_.data();
    const size_t price_range = asks_levels_.size();

    size_t resting_count = 0;  // orders found by walking levels

    constexpr size_t kBookSides = 2;  // validate_book walks bids then asks
    for (size_t pass = 0; pass < kBookSides; ++pass) {
        const bool bid_side = (pass == 0);
        const auto& levels = bid_side ? bids_levels_ : asks_levels_;
        const auto& bitset = bid_side ? bids_bitset_ : asks_bitset_;
        const auto& summary = bid_side ? bids_summary_ : asks_summary_;

        for (size_t bit_idx = 0; bit_idx < price_range; ++bit_idx) {
            const PriceLevel& level = levels[bit_idx];

            // (1) level canary intact
            if (!level.canary_intact()) {
                fail("level canary corrupted at price index", bit_idx, 0);
            }

            // Walk the queue: totals, links, pool liveness, map back-reference.
            uint64_t walked_qty = 0;
            uint32_t walked_count = 0;
            uint32_t cur = level.head;
            uint32_t prv = INVALID_INDEX;
            while (cur != INVALID_INDEX) {
                if (!order_pool_.is_allocated(cur)) {
                    fail("level references a non-live pool slot", bit_idx, cur);
                    break;
                }
                const Order& o = pool_data[cur];
                if (o.prev != prv) {
                    fail("intrusive prev link broken at price index", bit_idx, cur);
                }
                const uint32_t nxt = o.next;
                if (!order_pool_.is_allocated(nxt) && nxt != INVALID_INDEX) {
                    fail("intrusive next link points to a non-live slot", bit_idx, nxt);
                }
                if (map_ptr[o.id] != cur) {
                    fail("order_map does not back-resolve to the level's slot", o.id, cur);
                }
                if (o.price != min_price_ + static_cast<Price>(bit_idx)) {
                    fail("order price does not match its level index", o.price, bit_idx);
                }
                if (o.side != (bid_side ? Side::Buy : Side::Sell)) {
                    fail("order side does not match its level side", bit_idx, static_cast<size_t>(o.side));
                }
                if (nxt == INVALID_INDEX && level.tail != cur) {
                    fail("walk end does not match level.tail", cur, level.tail);
                }
                walked_qty += o.quantity;
                ++walked_count;
                prv = cur;
                cur = nxt;
            }
            if (level.head != INVALID_INDEX && level.head < order_pool_.capacity() &&
                order_pool_.is_allocated(level.head) && pool_data[level.head].prev != INVALID_INDEX) {
                fail("head->prev is not INVALID_INDEX", level.head, 0);
            }
            if (level.total_quantity != walked_qty) {
                fail("level.total_quantity != walked sum", static_cast<size_t>(level.total_quantity), walked_qty);
            }
            if (level.order_count != walked_count) {
                fail("level.order_count != walked count", level.order_count, walked_count);
            }
            resting_count += walked_count;

            // (5) bitset bit iff level non-empty
            const size_t word = bit_idx / detail::kWordBits, bit = bit_idx % detail::kWordBits;
            const bool bit_set = (bitset[word] >> bit) & 1ULL;
            if (bit_set != !level.is_empty()) {
                fail("bitset bit does not match level emptiness", bit_idx, bit_set);
            }
        }
        // (5b) summary bit iff bitset word non-zero
        for (size_t w = 0; w < bitset.size(); ++w) {
            const bool sum_set = (summary[w / detail::kWordBits] >> (w % detail::kWordBits)) & 1ULL;
            if (sum_set != (bitset[w] != 0)) {
                fail("summary bit does not match bitset word", w, sum_set);
            }
        }
    }

    // (4) order_map entries resolve into levels (count cross-check; the
    // per-slot membership was already verified from the level side).
    size_t map_count = 0;
    for (size_t id = 1; id <= max_order_id_; ++id) {
        const uint32_t idx = map_ptr[id];
        if (idx == INVALID_INDEX) continue;
        ++map_count;
        if (!order_pool_.is_allocated(idx)) {
            fail("order_map entry points to a non-live slot", id, idx);
        }
        const Order& o = pool_data[idx];
        if (o.id != id) {
            fail("slot id does not match order_map key", o.id, id);
        }
    }

    // (6) best price == true extrema when the side is non-empty
    if (has_bids_) {
        bool found = false;
        for (size_t i = asks_levels_.size(); i-- > 0;) {
            if (!bids_levels_[i].is_empty()) {
                if (best_bid_ != min_price_ + i || best_bid_idx_ != i) {
                    fail("best_bid_ is not the true maximum", best_bid_, i);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            fail("has_bids_ true but no bid level is non-empty", 1, 0);
        }
    } else if (best_bid_idx_ >= bids_levels_.size()) {
        fail("empty-side best_bid_idx_ out of range", best_bid_idx_, bids_levels_.size());
    }
    if (has_asks_) {
        bool found = false;
        for (size_t i = 0; i < asks_levels_.size(); ++i) {
            if (!asks_levels_[i].is_empty()) {
                if (best_ask_ != min_price_ + i || best_ask_idx_ != i) {
                    fail("best_ask_ is not the true minimum", best_ask_, i);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            fail("has_asks_ true but no ask level is non-empty", 1, 0);
        }
    } else if (best_ask_idx_ >= asks_levels_.size()) {
        fail("empty-side best_ask_idx_ out of range", best_ask_idx_, asks_levels_.size());
    }

    // (7) counts: active_count_ == pool live == map entries; free-list depth
#if HFT_DEBUG_TOOLS
    const auto st = order_pool_.stats();
    if (active_count_ != st.live_count) fail("active_count_ != pool live count", active_count_, st.live_count);
    if (map_count != st.live_count) fail("order_map live entries != pool live count", map_count, st.live_count);
    if (resting_count != st.live_count)
        fail("level-walked resting orders != pool live count", resting_count, st.live_count);
    if (st.free_list_depth != st.capacity - st.live_count) {
        fail("pool free-list depth != capacity - live (SIZE_MAX = cycle/wild link)", st.free_list_depth,
             st.capacity - st.live_count);
    }
#endif

    if (!ok) {
        std::fflush(stderr);
    }
    return ok;
}
#endif  // HFT_DEBUG_TOOLS

}  // namespace hft
