#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <vector>

#include "Journal.h"  // lifecycle tracking (Debug/Audit only)
#include "Macros.h"
#include "MemoryPool.h"
#include "Order.h"
#include "PriceLevel.h"
#if HFT_DEBUG_TOOLS
#include <thread>  // thread-identity latch
#endif

namespace hft {

enum class RejectReason : uint8_t {
    Accepted = 0,
    InvalidQuantity,
    DuplicateOrderId,
    OutOfBoundsOrderId,
    OutOfBoundsPrice,
    CancelFailed,
    PoolExhausted,
    MarketOnEmptyBook,  ///< market order against an empty opposite side.
    ReentrancyBlocked   ///< debug latch (re-entrancy or cross-thread call) blocked the operation.
};

struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Price price;
    Quantity quantity;
};

/// Fault handler for OrderBook debug latches (re-entrancy / thread contract).
/// Same shape as the pool's handler; returning SkipOperation makes the
/// blocked call return RejectReason::ReentrancyBlocked instead of aborting —
/// this is what makes the latches unit-testable.
using BookFaultHandler = FaultAction (*)(const char* message) noexcept;

/// Install the OrderBook fault handler; returns the previous one.
inline BookFaultHandler set_book_fault_handler(BookFaultHandler handler) noexcept {
    static BookFaultHandler current = nullptr;  // NOLINT: single-threaded engine contract
    BookFaultHandler previous = current;
    current = handler;
    return previous;
}

namespace detail {

// Two-level bitset geometry: every 64-bit word of the level-0 bitset covers
// 64 price ticks; every 64-bit summary word tracks 64 level-0 words. All
// scan/fast-forward arithmetic below flows from these three constants.
inline constexpr uint64_t kWordBits = 64;                    // bits per bitset/summary word
inline constexpr uint64_t kTopBitIndex = kWordBits - 1;      // 63: highest bit position in a word
inline constexpr uint64_t kPrefetchDistance = 16;            // order-map prefetch stride (in ids)
inline constexpr size_t kJournalCapacity = size_t{1} << 14;  // lifecycle ring entries
inline constexpr size_t kFaultMessageBytes = 128;            // latch-fault message buffer

[[nodiscard]] inline FaultAction book_fault(const char* message) noexcept {
    BookFaultHandler handler = set_book_fault_handler(nullptr);
    set_book_fault_handler(handler);
    std::fprintf(stderr, "[HFT_BOOK_FAULT] %s\n", message);
    std::fflush(stderr);
    if (handler != nullptr && handler(message) == FaultAction::SkipOperation) {
        return FaultAction::SkipOperation;
    }
    std::abort();
}
}  // namespace detail

// Engine-internal inlining policy: full instrumentation (journal writes, latch
// objects) exceeds a profitable always_inline budget, so instrumented builds
// use plain inline and let the compiler decide. Pure Release keeps forced
// inlining for the hot path.
#if HFT_DEBUG_TOOLS
#define HFT_ENGINE_INLINE inline
#else
#define HFT_ENGINE_INLINE HFT_FORCEINLINE
#endif

// Journal recording: zero-cost no-op in Release (arguments not evaluated).
// The no-op is a plain expression statement — NOT do{}while(0): every call
// site would otherwise trip cppcoreguidelines-avoid-do-while in Release.
#if HFT_DEBUG_TOOLS
#define HFT_JOURNAL(EV, ID, PX, QTY, SIDE, ...) \
    journal_.record(JournalEvent::EV, (ID), (PX), (QTY), static_cast<uint8_t>(SIDE) __VA_OPT__(, ) __VA_ARGS__)
#else
#define HFT_JOURNAL(EV, ID, PX, QTY, SIDE, ...) ((void)0)
#endif

/**
 * @class OrderBook
 * @brief Price-time-priority limit order book with O(1) price-level access,
 * two-level bitset best-price tracking and a zero-allocation memory pool.
 *
 * THREAD SAFETY CONTRACT: this class is SINGLE-THREADED. All
 * public entry points must be called from the constructing thread. Debug and
 * Audit builds enforce this (thread-identity latch) and also enforce the
 * re-entrancy contract below; Release trusts both by design.
 *
 * RE-ENTRANCY CONTRACT: trade callbacks MUST NOT call add_order
 * or cancel_order on the same OrderBook instance, directly or indirectly.
 * The matching loops cache raw pointers into pool storage; a nested mutation
 * invalidates them. Debug/Audit builds turn violations into an immediate,
 * diagnosable fault instead of silent corruption.
 *
 * COPY SEMANTICS: copying is deleted — two books sharing
 * one order-ID space is a logic error in a trading system. Moving is allowed
 * (vector relocation keeps all pool indices valid).
 */
class OrderBook {
   public:
    OrderBook(size_t max_order_id, size_t max_active_orders, Price min_price, Price max_price);
    OrderBook(size_t max_orders, Price min_price, Price max_price)
        : OrderBook(max_orders, max_orders, min_price, max_price) {}

    OrderBook(const OrderBook&) = delete;  // two books sharing an ID space is a logic error
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;  // vectors relocate wholesale — indices stay valid
    OrderBook& operator=(OrderBook&&) noexcept = default;
    ~OrderBook() = default;  // rule of five: every member is RAII (vectors + pool)

    /**
     * @brief Adds an order to the book (Limit/Market) or cancels one (Cancel).
     * @param on_trade  Callback invoked per fill. Must be noexcept (static_assert).
     * @param filled_out Optional out-parameter: how much of `quantity` actually
     *        traded, so a partial fill is distinguishable from a full fill
     *        without summing the callbacks.
     *
     * WARNING: Trade callbacks MUST NOT be re-entrant. You cannot call add_order or
     * cancel_order from within the on_trade callback on the same OrderBook instance.
     * Debug/Audit builds abort (or return ReentrancyBlocked with a fault handler).
     */
    template<typename TradeCallback>
    [[nodiscard]] HFT_ENGINE_INLINE RejectReason add_order(OrderId id, OrderType type, Price price, Quantity quantity,
                                                           Side side, TradeCallback&& on_trade,
                                                           Quantity* filled_out = nullptr);

    [[nodiscard]] HFT_ENGINE_INLINE RejectReason cancel_order(OrderId id);

    // ────────────────────────────────────────────────────────────────
    // Introspection API — cheap accessors available in every build flavor.
    // ────────────────────────────────────────────────────────────────
    [[nodiscard]] bool has_bids() const noexcept { return has_bids_; }
    [[nodiscard]] bool has_asks() const noexcept { return has_asks_; }

    /// Best bid price. Valid only when has_bids(); returns 0 otherwise.
    [[nodiscard]] Price best_bid() const noexcept { return best_bid_; }
    /// Best ask price. Valid only when has_asks(); returns max_price()+1 otherwise.
    [[nodiscard]] Price best_ask() const noexcept { return best_ask_; }

    [[nodiscard]] bool order_exists(OrderId id) const noexcept {
        return id != 0 && id <= max_order_id_ && order_map_[id] != INVALID_INDEX;
    }

    /// Number of resting orders (maintained incrementally; also cross-checked
    /// by validate_book() against the pool and the order map).
    [[nodiscard]] size_t active_order_count() const noexcept { return active_count_; }

    /// Number of distinct non-empty price levels on one side (bitset popcount).
    [[nodiscard]] size_t price_level_count(Side side) const noexcept {
        const auto& bitset = side == Side::Buy ? bids_bitset_ : asks_bitset_;
        size_t count = 0;
        for (uint64_t word : bitset) {
            count += static_cast<size_t>(std::popcount(word));
        }
        return count;
    }

    /// Resting order count at one price level (0 if out of range or empty).
    [[nodiscard]] uint32_t level_order_count(Side side, Price price) const noexcept {
        if (price < min_price_ || price > max_price_) return 0;
        const auto& levels = side == Side::Buy ? bids_levels_ : asks_levels_;
        return levels[price - min_price_].order_count;
    }

    /// Resting total quantity at one price level (0 if out of range or empty).
    [[nodiscard]] uint64_t level_total_quantity(Side side, Price price) const noexcept {
        if (price < min_price_ || price > max_price_) return 0;
        const auto& levels = side == Side::Buy ? bids_levels_ : asks_levels_;
        return levels[price - min_price_].total_quantity;
    }

#if HFT_DEBUG_TOOLS
    /// Cross-structural invariant checker. Verifies, in order:
    ///  1. per-level totals vs walked sums; level canaries intact
    ///  2. intrusive links bidirectional; head->prev / tail->next INVALID
    ///  3. every level-referenced slot is pool-live and order_map back-resolves
    ///  4. every order_map entry resolves to a slot whose level contains it
    ///  5. bitset bits iff non-empty levels; summary bits iff non-zero words
    ///  6. best bid/ask equal the true extrema when the side is non-empty
    ///  7. active_count_ == pool live count == map entry count; free-list
    ///     depth == capacity - live
    /// Returns true when every invariant holds; prints the first failure(s).
    [[nodiscard]] bool validate_book() const;

    /// Direct access to the lifecycle journal.
    [[nodiscard]] const Journal& journal() const noexcept { return journal_; }
    Journal& journal() noexcept { return journal_; }
#endif

   private:
    /// Ungated cancel implementation — public entry points apply the debug
    /// latches first; add_order's Cancel redirect calls this directly (the
    /// internal hand-off is single-threaded and legal, and must not trip the
    /// re-entrancy latch).
    HFT_ENGINE_INLINE RejectReason cancel_order_impl(OrderId id);

    template<Side side, typename TradeCallback>
    HFT_ENGINE_INLINE void match_order_direct(OrderId taker_id, Price taker_price, Quantity& taker_quantity,
                                              TradeCallback&& on_trade);

    HFT_ENGINE_INLINE void update_best_bid_after_remove(Price removed_price);
    HFT_ENGINE_INLINE void update_best_ask_after_remove(Price removed_price);

    /// Deterministic per-page read used when madvise/VirtualLock pre-touch
    /// fails — faults pages HERE, never mid-trade.
    static void touch_fallback(void* base, size_t bytes) noexcept;

#if HFT_DEBUG_TOOLS
    /// Entry gate: enforces the thread contract and the re-entrancy contract.
    /// Returns false when a handler chose SkipOperation (caller must return
    /// ReentrancyBlocked); aborts (or propagates the handler's decision) otherwise.
    [[nodiscard]] bool debug_entry_gate(const char* entry_point) noexcept;

    struct EntryLatch {
        explicit EntryLatch(bool& f) noexcept : flag_(&f) { *flag_ = true; }
        ~EntryLatch() { *flag_ = false; }
        EntryLatch(const EntryLatch&) = delete;  // a copied latch would clear the flag twice
        EntryLatch& operator=(const EntryLatch&) = delete;
        EntryLatch(EntryLatch&&) = delete;  // moving would leave a stale pointer
        EntryLatch& operator=(EntryLatch&&) = delete;

       private:
        bool* flag_;  // pointer, not reference: keeps the latch rule-of-five clean
    };
#endif

    std::vector<uint64_t> bids_bitset_;
    std::vector<uint64_t> asks_bitset_;
    std::vector<uint64_t> bids_summary_;
    std::vector<uint64_t> asks_summary_;

    std::vector<PriceLevel> bids_levels_;
    std::vector<PriceLevel> asks_levels_;

    Price min_price_;
    Price max_price_;
    size_t max_order_id_;

    // Sentinel semantics: when a side is empty, the PRICE
    // sentinel is best_ask_ = max_price_+1 (safe: the constructor aborts on
    // max_price == UINT32_MAX, and every read is gated by has_asks_) or
    // best_bid_ = 0 (gated by has_bids_). The INDEX sentinels are always
    // kept INSIDE [0, levels.size()) so no future getter can read OOB.
    Price best_bid_;
    Price best_ask_;
    size_t best_bid_idx_;
    size_t best_ask_idx_;
    bool has_bids_;
    bool has_asks_;

    std::vector<uint32_t> order_map_;
    MemoryPool<Order> order_pool_;
    size_t active_count_ = 0;  ///< resting orders (introspection; verified by validate_book)

#if HFT_DEBUG_TOOLS
    mutable Journal journal_{detail::kJournalCapacity};
    bool in_public_entry_ = false;
    std::thread::id owner_thread_{};
#endif
};

// ====================================================================
// TEMPLATE IMPLEMENTATIONS
// ====================================================================

#if HFT_DEBUG_TOOLS
inline bool OrderBook::debug_entry_gate(const char* entry_point) noexcept {
    if (HFT_UNLIKELY(std::this_thread::get_id() != owner_thread_)) {
        std::array<char, detail::kFaultMessageBytes> msg{};
        std::snprintf(msg.data(), msg.size(), "OrderBook thread-contract violation: %s called from a foreign thread",
                      entry_point);
        if (detail::book_fault(msg.data()) == FaultAction::SkipOperation) {
            return false;
        }
    }
    if (HFT_UNLIKELY(in_public_entry_)) {
        std::array<char, detail::kFaultMessageBytes> msg{};
        std::snprintf(msg.data(), msg.size(), "OrderBook re-entrancy violation: %s called from inside a trade callback",
                      entry_point);
        if (detail::book_fault(msg.data()) == FaultAction::SkipOperation) {
            return false;
        }
    }
    return true;
}
#endif

template<Side side, typename TradeCallback>
HFT_ENGINE_INLINE void OrderBook::match_order_direct(
    OrderId taker_id, Price taker_price, Quantity& taker_quantity,
    TradeCallback&&
        on_trade) {  // NOLINT(cppcoreguidelines-missing-std-forward): the callback is invoked (often many times) as an
                     // lvalue — forwarding would double-move move-only callbacks on the second fill
    // Enforce noexcept contract on trade callback.
    // If the callback throws after partial matching, the book is left in an
    // inconsistent state with no recovery path.
    static_assert(noexcept(std::declval<TradeCallback>()(std::declval<const Trade&>())),
                  "Trade callback must be noexcept — throwing during matching corrupts the order book");

    Order* const pool_data = order_pool_.data();
    uint32_t* const order_map_ptr = order_map_.data();
    const size_t asks_summary_size = asks_summary_.size();

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
                const uint32_t maker_count = level.order_count;
                uint32_t maker_order_idx = level.head;

                if (level.order_count == 1) {
                    // One-order fast path
                    Order& maker_order = pool_data[maker_order_idx];
                    on_trade(Trade{.maker_id = maker_order.id,
                                   .taker_id = taker_id,
                                   .price = maker_order.price,
                                   .quantity = maker_order.quantity});
                    HFT_JOURNAL(TradeExecuted, taker_id, maker_order.price, maker_order.quantity, Side::Buy);
                    HFT_JOURNAL(MakerFilled, maker_order.id, maker_order.price, 0, Side::Sell);
                    order_map_ptr[maker_order.id] = INVALID_INDEX;
                    order_pool_.deallocate(maker_order_idx);
                    active_count_ -= 1;
                } else {
                    if (maker_order_idx != INVALID_INDEX)
                        HFT_PREFETCH(&order_map_ptr[pool_data[maker_order_idx].id], 1, 3);
                    while (maker_order_idx != INVALID_INDEX) {
                        Order& maker_order = pool_data[maker_order_idx];
                        uint32_t next_maker_idx = maker_order.next;
                        if (next_maker_idx != INVALID_INDEX) {
                            HFT_PREFETCH(&pool_data[next_maker_idx], 0, 3);
                            HFT_PREFETCH(&order_map_ptr[pool_data[next_maker_idx].id], 1, 3);
                        }
                        on_trade(Trade{.maker_id = maker_order.id,
                                       .taker_id = taker_id,
                                       .price = maker_order.price,
                                       .quantity = maker_order.quantity});
                        HFT_JOURNAL(TradeExecuted, taker_id, maker_order.price, maker_order.quantity, Side::Buy);
                        HFT_JOURNAL(MakerFilled, maker_order.id, maker_order.price, 0, Side::Sell);
                        order_map_ptr[maker_order.id] = INVALID_INDEX;
                        maker_order_idx = next_maker_idx;
                    }
                    order_pool_.deallocate_chain(level.head, level.tail);
                    active_count_ -= maker_count;
                }

                taker_quantity -= level_qty;

                // Bulk-reset level (avoids N individual remove_order calls)
                level.head = INVALID_INDEX;
                level.tail = INVALID_INDEX;
                level.total_quantity = 0;
                level.order_count = 0;
            } else {
                // ── Slow path: partial fill within this level ──
                // INVARIANT: taker_quantity < level.total_quantity on entry, so
                // the level always retains a positive quantity here — the loop
                // can never drain it.
                uint32_t maker_order_idx = level.head;
                if (maker_order_idx != INVALID_INDEX) HFT_PREFETCH(&order_map_ptr[pool_data[maker_order_idx].id], 1, 3);
                while (maker_order_idx != INVALID_INDEX && taker_quantity > 0) {
                    Order& maker_order = pool_data[maker_order_idx];
                    Quantity trade_qty = taker_quantity < maker_order.quantity ? taker_quantity : maker_order.quantity;
                    on_trade(Trade{.maker_id = maker_order.id,
                                   .taker_id = taker_id,
                                   .price = maker_order.price,
                                   .quantity = trade_qty});
                    HFT_JOURNAL(TradeExecuted, taker_id, maker_order.price, trade_qty, Side::Buy);

                    taker_quantity -= trade_qty;
                    maker_order.quantity -= trade_qty;
                    level.total_quantity -= trade_qty;

                    uint32_t next_maker_idx = maker_order.next;
                    if (next_maker_idx != INVALID_INDEX) {
                        HFT_PREFETCH(&pool_data[next_maker_idx], 0, 3);
                        HFT_PREFETCH(&order_map_ptr[pool_data[next_maker_idx].id], 1, 3);
                    }

                    if (maker_order.quantity == 0) [[likely]] {
                        HFT_JOURNAL(MakerFilled, maker_order.id, maker_order.price, 0, Side::Sell);
                        order_map_ptr[maker_order.id] = INVALID_INDEX;
                        order_pool_.deallocate(maker_order_idx);
                        --level.order_count;
                        --active_count_;
                        level.head = next_maker_idx;
                        if (next_maker_idx != INVALID_INDEX) {
                            pool_data[next_maker_idx].prev = INVALID_INDEX;
                        }
                    }
                    maker_order_idx = next_maker_idx;
                }
            }

            if (HFT_UNLIKELY(level.is_empty())) {
                size_t bit_idx = best_ask_ - min_price_;
                size_t word_offset = bit_idx / detail::kWordBits;
                uint64_t bit_mask = 1ULL << (bit_idx % detail::kWordBits);
                asks_bitset_ptr[word_offset] &= ~bit_mask;
                if (asks_bitset_ptr[word_offset] == 0) {
                    asks_summary_ptr[word_offset / detail::kWordBits] &= ~(1ULL << (word_offset % detail::kWordBits));
                }

                Price next_ask = best_ask_ + 1;
                size_t next_bit_idx = bit_idx + 1;
                if (next_ask <= max_price_ && !asks_levels_ptr[next_bit_idx].is_empty()) {
                    best_ask_ = next_ask;
                    best_ask_idx_ = next_bit_idx;
                } else {
                    // Fast-forward best_ask_ using bitset
                    uint64_t word_idx = bit_idx / detail::kWordBits;
                    uint64_t mask = ~((1ULL << (bit_idx % detail::kWordBits)) - 1);
                    uint64_t word = asks_bitset_ptr[word_idx] & mask;

                    if (word != 0) {
                        best_ask_idx_ = word_idx * detail::kWordBits + std::countr_zero(word);
                        best_ask_ = min_price_ + best_ask_idx_;
                    } else {
                        has_asks_ = false;
                        best_ask_ = max_price_ + 1;               // price sentinel (guarded by has_asks_)
                        best_ask_idx_ = asks_levels_.size() - 1;  // keep index in-range (stale, never dereferenced)
                        uint64_t summary_idx = word_idx / detail::kWordBits;
                        uint64_t k = word_idx % detail::kWordBits;
                        uint64_t summary_mask = ~((((1ULL << k) - 1) << 1) | 1);
                        uint64_t summary_word = asks_summary_ptr[summary_idx] & summary_mask;

                        if (summary_word != 0) {
                            size_t next_word_idx = summary_idx * detail::kWordBits + std::countr_zero(summary_word);
                            best_ask_idx_ =
                                next_word_idx * detail::kWordBits + std::countr_zero(asks_bitset_ptr[next_word_idx]);
                            best_ask_ = min_price_ + best_ask_idx_;
                            has_asks_ = true;
                        } else {
                            for (size_t i = summary_idx + 1; i < asks_summary_size; ++i) {
                                if (asks_summary_ptr[i] != 0) {
                                    size_t next_word_idx =
                                        i * detail::kWordBits + std::countr_zero(asks_summary_ptr[i]);
                                    best_ask_idx_ = next_word_idx * detail::kWordBits +
                                                    std::countr_zero(asks_bitset_ptr[next_word_idx]);
                                    best_ask_ = min_price_ + best_ask_idx_;
                                    has_asks_ = true;
                                    break;
                                }
                            }
                        }
                    }
                    HFT_JOURNAL(BestAskChanged, 0, best_ask_, 0, Side::Sell);
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
                const uint32_t maker_count = level.order_count;
                uint32_t maker_order_idx = level.head;

                if (level.order_count == 1) {
                    // One-order fast path
                    Order& maker_order = pool_data[maker_order_idx];
                    on_trade(Trade{.maker_id = maker_order.id,
                                   .taker_id = taker_id,
                                   .price = maker_order.price,
                                   .quantity = maker_order.quantity});
                    HFT_JOURNAL(TradeExecuted, taker_id, maker_order.price, maker_order.quantity, Side::Sell);
                    HFT_JOURNAL(MakerFilled, maker_order.id, maker_order.price, 0, Side::Buy);
                    order_map_ptr[maker_order.id] = INVALID_INDEX;
                    order_pool_.deallocate(maker_order_idx);
                    active_count_ -= 1;
                } else {
                    if (maker_order_idx != INVALID_INDEX)
                        HFT_PREFETCH(&order_map_ptr[pool_data[maker_order_idx].id], 1, 3);
                    while (maker_order_idx != INVALID_INDEX) {
                        Order& maker_order = pool_data[maker_order_idx];
                        uint32_t next_maker_idx = maker_order.next;
                        if (next_maker_idx != INVALID_INDEX) {
                            HFT_PREFETCH(&pool_data[next_maker_idx], 0, 3);
                            HFT_PREFETCH(&order_map_ptr[pool_data[next_maker_idx].id], 1, 3);
                        }
                        on_trade(Trade{.maker_id = maker_order.id,
                                       .taker_id = taker_id,
                                       .price = maker_order.price,
                                       .quantity = maker_order.quantity});
                        HFT_JOURNAL(TradeExecuted, taker_id, maker_order.price, maker_order.quantity, Side::Sell);
                        HFT_JOURNAL(MakerFilled, maker_order.id, maker_order.price, 0, Side::Buy);
                        order_map_ptr[maker_order.id] = INVALID_INDEX;
                        maker_order_idx = next_maker_idx;
                    }
                    // Bulk deallocate the level's orders!
                    order_pool_.deallocate_chain(level.head, level.tail);
                    active_count_ -= maker_count;
                }

                taker_quantity -= level_qty;

                // Bulk-reset level
                level.head = INVALID_INDEX;
                level.tail = INVALID_INDEX;
                level.total_quantity = 0;
                level.order_count = 0;
            } else {
                // ── Slow path: partial fill within this level ──
                // (see the Buy-side proof comment)
                uint32_t maker_order_idx = level.head;
                if (maker_order_idx != INVALID_INDEX) HFT_PREFETCH(&order_map_ptr[pool_data[maker_order_idx].id], 1, 3);
                while (maker_order_idx != INVALID_INDEX && taker_quantity > 0) {
                    Order& maker_order = pool_data[maker_order_idx];
                    Quantity trade_qty = taker_quantity < maker_order.quantity ? taker_quantity : maker_order.quantity;
                    on_trade(Trade{.maker_id = maker_order.id,
                                   .taker_id = taker_id,
                                   .price = maker_order.price,
                                   .quantity = trade_qty});
                    HFT_JOURNAL(TradeExecuted, taker_id, maker_order.price, trade_qty, Side::Sell);

                    taker_quantity -= trade_qty;
                    maker_order.quantity -= trade_qty;
                    level.total_quantity -= trade_qty;

                    uint32_t next_maker_idx = maker_order.next;
                    if (next_maker_idx != INVALID_INDEX) {
                        HFT_PREFETCH(&pool_data[next_maker_idx], 0, 3);
                        HFT_PREFETCH(&order_map_ptr[pool_data[next_maker_idx].id], 1, 3);
                    }

                    if (maker_order.quantity == 0) [[likely]] {
                        HFT_JOURNAL(MakerFilled, maker_order.id, maker_order.price, 0, Side::Buy);
                        order_map_ptr[maker_order.id] = INVALID_INDEX;
                        order_pool_.deallocate(maker_order_idx);
                        --level.order_count;
                        --active_count_;
                        level.head = next_maker_idx;
                        if (next_maker_idx != INVALID_INDEX) {
                            pool_data[next_maker_idx].prev = INVALID_INDEX;
                        }
                    }
                    maker_order_idx = next_maker_idx;
                }
            }

            if (HFT_UNLIKELY(level.is_empty())) {
                size_t bit_idx = best_bid_ - min_price_;
                size_t word_offset = bit_idx / detail::kWordBits;
                uint64_t bit_mask = 1ULL << (bit_idx % detail::kWordBits);
                bids_bitset_ptr[word_offset] &= ~bit_mask;
                if (bids_bitset_ptr[word_offset] == 0) {
                    bids_summary_ptr[word_offset / detail::kWordBits] &= ~(1ULL << (word_offset % detail::kWordBits));
                }

                if (best_bid_ > min_price_ && !bids_levels_ptr[bit_idx - 1].is_empty()) {
                    best_bid_--;
                    best_bid_idx_--;
                } else {
                    // Fast-forward best_bid_ using bitset (scan downwards)
                    uint64_t word_idx = bit_idx / detail::kWordBits;
                    uint64_t mask = (1ULL << (bit_idx % detail::kWordBits)) - 1;
                    uint64_t word = bids_bitset_ptr[word_idx] & mask;

                    if (word != 0) {
                        best_bid_idx_ = word_idx * detail::kWordBits + detail::kTopBitIndex - std::countl_zero(word);
                        best_bid_ = min_price_ + best_bid_idx_;
                    } else {
                        has_bids_ = false;
                        best_bid_ = 0;      // price sentinel (guarded by has_bids_)
                        best_bid_idx_ = 0;  // valid (stale) index — never dereferenced
                        uint64_t summary_idx = word_idx / detail::kWordBits;
                        uint64_t summary_mask = (1ULL << (word_idx % detail::kWordBits)) - 1;
                        uint64_t summary_word = bids_summary_ptr[summary_idx] & summary_mask;

                        if (summary_word != 0) {
                            size_t next_word_idx =
                                summary_idx * detail::kWordBits + detail::kTopBitIndex - std::countl_zero(summary_word);
                            best_bid_idx_ = next_word_idx * detail::kWordBits + detail::kTopBitIndex -
                                            std::countl_zero(bids_bitset_ptr[next_word_idx]);
                            best_bid_ = min_price_ + best_bid_idx_;
                            has_bids_ = true;
                        } else {
                            for (int64_t i = static_cast<int64_t>(summary_idx) - 1; i >= 0; --i) {
                                if (bids_summary_ptr[i] != 0) {
                                    size_t next_word_idx = i * detail::kWordBits + detail::kTopBitIndex -
                                                           std::countl_zero(bids_summary_ptr[i]);
                                    best_bid_idx_ = next_word_idx * detail::kWordBits + detail::kTopBitIndex -
                                                    std::countl_zero(bids_bitset_ptr[next_word_idx]);
                                    best_bid_ = min_price_ + best_bid_idx_;
                                    has_bids_ = true;
                                    break;
                                }
                            }
                        }
                    }
                    HFT_JOURNAL(BestBidChanged, 0, best_bid_, 0, Side::Buy);
                }
            }
        }
    }
}

HFT_ENGINE_INLINE void OrderBook::update_best_ask_after_remove(Price removed_price) {
    assert(has_asks_ && best_ask_ == removed_price &&
           "update_best_ask_after_remove must be called from the best ask level");
    size_t bit_idx = removed_price - min_price_;
    uint64_t word_idx = bit_idx / detail::kWordBits;
    uint64_t mask = ~((1ULL << (bit_idx % detail::kWordBits)) - 1);
    const uint64_t* const asks_bitset_ptr = asks_bitset_.data();
    uint64_t word = asks_bitset_ptr[word_idx] & mask;

    if (word != 0) {
        best_ask_idx_ = word_idx * detail::kWordBits + std::countr_zero(word);
        best_ask_ = min_price_ + best_ask_idx_;
        has_asks_ = true;
        return;
    }

    uint64_t summary_idx = word_idx / detail::kWordBits;
    uint64_t k = word_idx % detail::kWordBits;
    uint64_t summary_mask = ~((((1ULL << k) - 1) << 1) | 1);
    const uint64_t* const asks_summary_ptr = asks_summary_.data();
    uint64_t summary_word = asks_summary_ptr[summary_idx] & summary_mask;
    const size_t asks_summary_size = asks_summary_.size();

    if (summary_word != 0) {
        size_t next_word_idx = summary_idx * detail::kWordBits + std::countr_zero(summary_word);
        best_ask_idx_ = next_word_idx * detail::kWordBits + std::countr_zero(asks_bitset_ptr[next_word_idx]);
        best_ask_ = min_price_ + best_ask_idx_;
        has_asks_ = true;
        return;
    }

    for (size_t i = summary_idx + 1; i < asks_summary_size; ++i) {
        if (asks_summary_ptr[i] != 0) {
            size_t next_word_idx = i * detail::kWordBits + std::countr_zero(asks_summary_ptr[i]);
            best_ask_idx_ = next_word_idx * detail::kWordBits + std::countr_zero(asks_bitset_ptr[next_word_idx]);
            best_ask_ = min_price_ + best_ask_idx_;
            has_asks_ = true;
            return;
        }
    }
    has_asks_ = false;
    best_ask_ = max_price_ + 1;
    best_ask_idx_ = asks_levels_.size() - 1;  // valid index (never dereferenced)
}

HFT_ENGINE_INLINE void OrderBook::update_best_bid_after_remove(Price removed_price) {
    assert(has_bids_ && best_bid_ == removed_price &&
           "update_best_bid_after_remove must be called from the best bid level");
    size_t bit_idx = removed_price - min_price_;
    uint64_t word_idx = bit_idx / detail::kWordBits;
    uint64_t mask = (1ULL << (bit_idx % detail::kWordBits)) - 1;
    const uint64_t* const bids_bitset_ptr = bids_bitset_.data();
    uint64_t word = bids_bitset_ptr[word_idx] & mask;

    if (word != 0) {
        best_bid_idx_ = word_idx * detail::kWordBits + detail::kTopBitIndex - std::countl_zero(word);
        best_bid_ = min_price_ + best_bid_idx_;
        // The precondition guarantees has_bids_, but setting it here keeps the
        // state assignment locally complete so a future refactor cannot break
        // the invariant silently.
        has_bids_ = true;
        return;
    }

    uint64_t summary_idx = word_idx / detail::kWordBits;
    uint64_t summary_mask = (1ULL << (word_idx % detail::kWordBits)) - 1;
    const uint64_t* const bids_summary_ptr = bids_summary_.data();
    uint64_t summary_word = bids_summary_ptr[summary_idx] & summary_mask;

    if (summary_word != 0) {
        size_t next_word_idx = summary_idx * detail::kWordBits + detail::kTopBitIndex - std::countl_zero(summary_word);
        best_bid_idx_ =
            next_word_idx * detail::kWordBits + detail::kTopBitIndex - std::countl_zero(bids_bitset_ptr[next_word_idx]);
        best_bid_ = min_price_ + best_bid_idx_;
        has_bids_ = true;
        return;
    }

    for (int64_t i = static_cast<int64_t>(summary_idx) - 1; i >= 0; --i) {
        if (bids_summary_ptr[i] != 0) {
            size_t next_word_idx = i * detail::kWordBits + detail::kTopBitIndex - std::countl_zero(bids_summary_ptr[i]);
            best_bid_idx_ = next_word_idx * detail::kWordBits + detail::kTopBitIndex -
                            std::countl_zero(bids_bitset_ptr[next_word_idx]);
            best_bid_ = min_price_ + best_bid_idx_;
            has_bids_ = true;
            return;
        }
    }
    has_bids_ = false;
    best_bid_ = 0;
    best_bid_idx_ = 0;  // valid index (never dereferenced)
}

[[nodiscard]] HFT_ENGINE_INLINE RejectReason OrderBook::cancel_order(OrderId id) {
#if HFT_DEBUG_TOOLS
    if (HFT_UNLIKELY(!debug_entry_gate("cancel_order"))) {
        return RejectReason::ReentrancyBlocked;
    }
    EntryLatch latch(in_public_entry_);
#endif
    return cancel_order_impl(id);
}

HFT_ENGINE_INLINE RejectReason OrderBook::cancel_order_impl(OrderId id) {
    HFT_JOURNAL(OrderSubmitted, id, 0, 0, Side::Buy);

    if (HFT_UNLIKELY(id == 0 || id > max_order_id_)) {
        HFT_JOURNAL(OrderRejected, id, 0, 0, Side::Buy, static_cast<uint8_t>(RejectReason::OutOfBoundsOrderId));
        return RejectReason::OutOfBoundsOrderId;
    }
    uint32_t* const order_map_ptr = order_map_.data();
    if (HFT_UNLIKELY(order_map_ptr[id] == INVALID_INDEX)) {
        HFT_JOURNAL(OrderRejected, id, 0, 0, Side::Buy, static_cast<uint8_t>(RejectReason::CancelFailed));
        return RejectReason::CancelFailed;
    }

    // Prefetch map location for nearby ID to reduce cache miss penalty in sequential patterns
    if (id + detail::kPrefetchDistance <= max_order_id_) {
        HFT_PREFETCH(&order_map_ptr[id + detail::kPrefetchDistance], 1, 3);
    }

    uint32_t order_idx = order_map_ptr[id];
    Order* const pool_ptr = order_pool_.data();
    Order& order = pool_ptr[order_idx];
    Price price = order.price;
    size_t bit_idx = price - min_price_;

    if (order.side == Side::Buy) {
        bids_levels_[bit_idx].remove_order(order_idx, pool_ptr);
        if (HFT_UNLIKELY(bids_levels_[bit_idx].is_empty())) {
            size_t word_offset = bit_idx / detail::kWordBits;
            uint64_t bit_mask = 1ULL << (bit_idx % detail::kWordBits);
            uint64_t* const bids_bitset_ptr = bids_bitset_.data();
            bids_bitset_ptr[word_offset] &= ~bit_mask;
            if (bids_bitset_ptr[word_offset] == 0) {
                bids_summary_.data()[word_offset / detail::kWordBits] &= ~(1ULL << (word_offset % detail::kWordBits));
            }
            if (has_bids_ && best_bid_ == price) {
                update_best_bid_after_remove(price);
            }
        }
    } else {
        asks_levels_[bit_idx].remove_order(order_idx, pool_ptr);
        if (HFT_UNLIKELY(asks_levels_[bit_idx].is_empty())) {
            size_t word_offset = bit_idx / detail::kWordBits;
            uint64_t bit_mask = 1ULL << (bit_idx % detail::kWordBits);
            uint64_t* const asks_bitset_ptr = asks_bitset_.data();
            asks_bitset_ptr[word_offset] &= ~bit_mask;
            if (asks_bitset_ptr[word_offset] == 0) {
                asks_summary_.data()[word_offset / detail::kWordBits] &= ~(1ULL << (word_offset % detail::kWordBits));
            }
            // Mirrors the bid side's `has_asks_ &&` guard; the empty sentinel
            // max_price_+1 can never equal a valid price.
            if (has_asks_ && best_ask_ == price) {
                update_best_ask_after_remove(price);
            }
        }
    }

    order_map_ptr[id] = INVALID_INDEX;
    order_pool_.deallocate(order_idx);
    --active_count_;
    HFT_JOURNAL(OrderCancelled, id, price, 0, static_cast<uint8_t>(order.side));

    return RejectReason::Accepted;
}

template<typename TradeCallback>
[[nodiscard]] HFT_ENGINE_INLINE RejectReason OrderBook::add_order(OrderId id, OrderType type, Price price,
                                                                  Quantity quantity, Side side,
                                                                  TradeCallback&& on_trade, Quantity* filled_out) {
#if HFT_DEBUG_TOOLS
    if (HFT_UNLIKELY(!debug_entry_gate("add_order"))) {
        if (filled_out) *filled_out = 0;
        return RejectReason::ReentrancyBlocked;
    }
    EntryLatch latch(in_public_entry_);
#endif

    if (type == OrderType::Cancel) [[unlikely]] {
        // Internal hand-off: already gated+latched by add_order; calling the
        // impl directly must not trip the re-entrancy latch.
        const RejectReason r = cancel_order_impl(id);
        if (filled_out) *filled_out = 0;
        return r;
    }

    if (quantity == 0) [[unlikely]] {
        HFT_JOURNAL(OrderRejected, id, price, quantity, static_cast<uint8_t>(side),
                    static_cast<uint8_t>(RejectReason::InvalidQuantity));
        if (filled_out) *filled_out = 0;
        return RejectReason::InvalidQuantity;
    }

    if (id == 0 || id > max_order_id_) [[unlikely]] {
        HFT_JOURNAL(OrderRejected, id, price, quantity, static_cast<uint8_t>(side),
                    static_cast<uint8_t>(RejectReason::OutOfBoundsOrderId));
        if (filled_out) *filled_out = 0;
        return RejectReason::OutOfBoundsOrderId;
    }

    uint32_t* const order_map_ptr = order_map_.data();
    if (order_map_ptr[id] != INVALID_INDEX) [[unlikely]] {
        HFT_JOURNAL(OrderRejected, id, price, quantity, static_cast<uint8_t>(side),
                    static_cast<uint8_t>(RejectReason::DuplicateOrderId));
        if (filled_out) *filled_out = 0;
        return RejectReason::DuplicateOrderId;
    }

    // Prefetch map location for sequential inserts
    if (id + detail::kPrefetchDistance <= max_order_id_) {
        HFT_PREFETCH(&order_map_ptr[id + detail::kPrefetchDistance], 1, 3);
    }

    if (type == OrderType::Limit) {
        if (price < min_price_ || price > max_price_) {
            HFT_JOURNAL(OrderRejected, id, price, quantity, static_cast<uint8_t>(side),
                        static_cast<uint8_t>(RejectReason::OutOfBoundsPrice));
            if (filled_out) *filled_out = 0;
            return RejectReason::OutOfBoundsPrice;
        }
    }

    // A Market order against a completely empty opposite side is rejected:
    // accepting it would silently discard the quantity, which is a critical
    // failure in a real venue.
    if (type == OrderType::Market) {
        const bool opposite_empty = (side == Side::Buy) ? !has_asks_ : !has_bids_;
        if (HFT_UNLIKELY(opposite_empty)) {
            HFT_JOURNAL(OrderRejected, id, 0, quantity, static_cast<uint8_t>(side),
                        static_cast<uint8_t>(RejectReason::MarketOnEmptyBook));
            if (filled_out) *filled_out = 0;
            return RejectReason::MarketOnEmptyBook;
        }
    }

    HFT_JOURNAL(OrderSubmitted, id, price, quantity, static_cast<uint8_t>(side));

    Price resolved_price =
        type == OrderType::Market ? (side == Side::Buy ? MARKET_BUY_PRICE : MARKET_SELL_PRICE) : price;
    Quantity remaining_qty = quantity;

    if (side == Side::Buy) {
        match_order_direct<Side::Buy>(id, resolved_price, remaining_qty, std::forward<TradeCallback>(on_trade));
    } else {
        match_order_direct<Side::Sell>(id, resolved_price, remaining_qty, std::forward<TradeCallback>(on_trade));
    }

    // Surface the filled quantity to the caller.
    if (filled_out) *filled_out = quantity - remaining_qty;

    if (remaining_qty > 0) {
        if (type == OrderType::Market) {
            // IOC semantics: the unmatched market remainder is discarded —
            // callers read filled_out instead of summing callbacks.
        } else {
            uint32_t order_idx = order_pool_.allocate();
            if (HFT_UNLIKELY(order_idx == INVALID_INDEX)) {
                HFT_JOURNAL(PoolExhausted, id, resolved_price, remaining_qty, static_cast<uint8_t>(side));
                return RejectReason::PoolExhausted;  // Pool exhausted, order cannot be posted.
            }
            Order* const pool_data = order_pool_.data();
            Order& order = pool_data[order_idx];
            order.id = id;
            order.price = resolved_price;
            order.quantity = remaining_qty;
            order.side = side;
            order.next = INVALID_INDEX;
            order.prev = INVALID_INDEX;
            ++active_count_;

            order_map_ptr[id] = order_idx;
            size_t bit_idx = order.price - min_price_;
            size_t word_offset = bit_idx / detail::kWordBits;
            uint64_t bit_mask = 1ULL << (bit_idx % detail::kWordBits);
            if (side == Side::Buy) {
                bool was_empty = bids_levels_[bit_idx].is_empty();
                bids_levels_[bit_idx].append_order(order_idx, pool_data);
                if (was_empty) {
                    uint64_t* const bids_bitset_ptr = bids_bitset_.data();
                    bids_bitset_ptr[word_offset] |= bit_mask;
                    bids_summary_.data()[word_offset / detail::kWordBits] |=
                        (1ULL << (word_offset % detail::kWordBits));
                }

                if (!has_bids_ || order.price > best_bid_) {
                    best_bid_ = order.price;
                    best_bid_idx_ = bit_idx;
                    has_bids_ = true;
                    HFT_JOURNAL(BestBidChanged, 0, best_bid_, 0, Side::Buy);
                }
            } else {
                bool was_empty = asks_levels_[bit_idx].is_empty();
                asks_levels_[bit_idx].append_order(order_idx, pool_data);
                if (was_empty) {
                    uint64_t* const asks_bitset_ptr = asks_bitset_.data();
                    asks_bitset_ptr[word_offset] |= bit_mask;
                    asks_summary_.data()[word_offset / detail::kWordBits] |=
                        (1ULL << (word_offset % detail::kWordBits));
                }

                if (!has_asks_ || order.price < best_ask_) {
                    best_ask_ = order.price;
                    best_ask_idx_ = bit_idx;
                    has_asks_ = true;
                    HFT_JOURNAL(BestAskChanged, 0, best_ask_, 0, Side::Sell);
                }
            }
            HFT_JOURNAL(OrderRested, id, resolved_price, remaining_qty, static_cast<uint8_t>(side));
        }
    }

    return RejectReason::Accepted;
}

}  // namespace hft
