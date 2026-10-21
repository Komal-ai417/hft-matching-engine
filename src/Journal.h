#pragma once

// Journal.h — order lifecycle tracking for Debug/Audit builds.
//
// A fixed-capacity append-only ring of compact event records written at every
// order state transition inside OrderBook. Single-threaded append is a store
// + index bump — nanoseconds in the Audit gear, fully compiled out in Release.
// The journal is the postmortem primitive: after a canary fault or a
// validate_book() failure, the last N events before the fault identify the
// offending order id, price level and code path in O(1).
//
// Deliberately NOT iostream and NOT heap-allocating on the hot path: the
// backing array is allocated once at construction.

#include <bit>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Macros.h"

#if HFT_DEBUG_TOOLS

namespace hft {

enum class JournalEvent : uint8_t {
    OrderSubmitted = 0,  ///< add_order/cancel_order entry (id, price, qty, side).
    OrderRejected,       ///< validation failed (reason carries the RejectReason).
    OrderRested,         ///< limit remainder posted to a price level.
    TradeExecuted,       ///< maker+taker matched (id = taker, price/qty of the fill).
    MakerFilled,         ///< a resting order fully consumed (id = maker).
    OrderCancelled,      ///< cancel succeeded (id).
    PoolExhausted,       ///< pool could not satisfy a resting remainder.
    BestBidChanged,      ///< best bid transition (price = new best).
    BestAskChanged,      ///< best ask transition (price = new best).
    Count                ///< sentinel: number of events (kept last)
};

inline constexpr const char* journal_event_name(JournalEvent e) noexcept {
    switch (e) {
        case JournalEvent::OrderSubmitted:
            return "SUBMIT";
        case JournalEvent::OrderRejected:
            return "REJECT";
        case JournalEvent::OrderRested:
            return "REST";
        case JournalEvent::TradeExecuted:
            return "TRADE";
        case JournalEvent::MakerFilled:
            return "MAKER_FILLED";
        case JournalEvent::OrderCancelled:
            return "CANCEL";
        case JournalEvent::PoolExhausted:
            return "POOL_EXHAUSTED";
        case JournalEvent::BestBidChanged:
            return "BEST_BID";
        case JournalEvent::BestAskChanged:
            return "BEST_ASK";
        default:
            return "?";
    }
}

/// 24 bytes — one entry per event, fixed layout, no pointers.
struct JournalEntry {
    uint64_t timestamp;  // RDTSC (or fallback counter) — hft::debug_timestamp()
    uint32_t id;         // order id (taker for TradeExecuted, maker for MakerFilled)
    uint32_t price;
    uint32_t quantity;
    uint8_t event;   // JournalEvent
    uint8_t reason;  // RejectReason for OrderRejected, else 0
    uint8_t side;    // Side
    uint8_t pad_;
};

class Journal {
   public:
    /// Default ring capacity (power of two, as required by the index mask).
    static constexpr size_t kDefaultCapacity = 8192;

    explicit Journal(size_t capacity = kDefaultCapacity) : entries_(capacity ? capacity : 1) {
        // capacity is rounded down to a power of two by callers (see
        // OrderBook's journal sizing). Guard anyway — the mask arithmetic
        // requires a power of two:
        const size_t pow2 = std::bit_floor(entries_.size());
        if (pow2 != entries_.size()) {
            entries_.resize(pow2);
        }
        mask_ = pow2 - 1;
    }

    HFT_FORCEINLINE void record(JournalEvent event, uint32_t id, uint32_t price, uint32_t quantity, uint8_t side,
                                uint8_t reason = 0) noexcept {
        JournalEntry& e = entries_[pos_ & mask_];
        e.timestamp = debug_timestamp();
        e.id = id;
        e.price = price;
        e.quantity = quantity;
        e.event = static_cast<uint8_t>(event);
        e.reason = reason;
        e.side = side;
        e.pad_ = 0;
        ++pos_;
    }

    /// Number of events actually retained (bounded by capacity).
    [[nodiscard]] size_t size() const noexcept { return pos_ < entries_.size() ? pos_ : entries_.size(); }
    /// Total events ever written (monotonic).
    [[nodiscard]] size_t total() const noexcept { return pos_; }
    /// Capacity of the ring.
    [[nodiscard]] size_t capacity() const noexcept { return entries_.size(); }

    /// Entry i of the retained window (0 = oldest retained).
    [[nodiscard]] const JournalEntry& at(size_t i) const noexcept {
        size_t base = pos_ - size();
        return entries_[(base + i) & mask_];
    }

    void clear() noexcept { pos_ = 0; }

    /// Render the last `max_lines` events to a FILE* (human-readable,
    /// chronological). Used on faults and by tests.
    void dump(std::FILE* out, size_t max_lines = SIZE_MAX) const noexcept {
        const size_t n = size();
        const size_t start = n > max_lines ? n - max_lines : 0;
        for (size_t i = start; i < n; ++i) {
            const JournalEntry& e = at(i);
            std::fprintf(out, "[%016llu] %-13s id=%-8u px=%-8u qty=%-8u side=%u reason=%u\n",
                         static_cast<unsigned long long>(e.timestamp),
                         journal_event_name(static_cast<JournalEvent>(e.event)), e.id, e.price, e.quantity, e.side,
                         e.reason);
        }
        std::fflush(out);
    }

   private:
    std::vector<JournalEntry> entries_;
    size_t mask_;
    size_t pos_ = 0;
};

}  // namespace hft

#endif  // HFT_DEBUG_TOOLS
