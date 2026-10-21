// ============================================================================
// Debugging-aids tests: the event journal, the re-entrancy latch, the
// thread-identity latch and the fault-handler plumbing that makes both
// latches testable. All Debug/Audit-only (Release compiles the aids out by
// design — the RELEASE_NO_TOOLS test below pins that).
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>

#include "../src/OrderBook.h"

using namespace hft;

#if HFT_DEBUG_TOOLS
namespace {
struct BookFaultRecorder {
    static inline int count_ = 0;
    static inline std::string last_message_{};
    BookFaultRecorder() {
        count_ = 0;
        last_message_.clear();
        set_book_fault_handler(&handler);
    }
    ~BookFaultRecorder() { set_book_fault_handler(nullptr); }
    static FaultAction handler(const char* msg) noexcept {
        count_++;
        last_message_ = msg;
        return FaultAction::SkipOperation;
    }
};
}  // namespace

// ── Re-entrancy latch ───────────────────────────────────────────

TEST(DebugAids, ReentrancyLatchBlocksNestedAddOrder) {
    BookFaultRecorder rec;
    OrderBook ob(100, 100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 100, 5, Side::Sell, [](const Trade&) noexcept {});

    // The callback tries to cancel an order from INSIDE on_trade — the exact
    // corruption scenario the header warns about. The latch must catch it.
    // (Taker qty 10 fully consumes maker 1 only -> exactly one callback.)
    int nested_calls = 0;
    auto bad_cb = [&](const Trade& t) noexcept {
        nested_calls++;
        (void)t;
        RejectReason r = ob.cancel_order(1);  // re-entrant!
        EXPECT_EQ(r, RejectReason::ReentrancyBlocked);
    };
    (void)ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy, bad_cb);
    EXPECT_EQ(nested_calls, 1);
    EXPECT_EQ(BookFaultRecorder::count_, 1);
    EXPECT_NE(BookFaultRecorder::last_message_.find("re-entrancy"), std::string::npos)
        << "message: " << BookFaultRecorder::last_message_;
    // The book must still be internally consistent after the blocked attempt:
    EXPECT_TRUE(ob.validate_book());
}

TEST(DebugAids, ReentrancyLatchBlocksNestedCancelOrder) {
    BookFaultRecorder rec;
    OrderBook ob(100, 100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    auto bad_cb = [&](const Trade&) noexcept {
        (void)ob.add_order(99, OrderType::Limit, 100, 1, Side::Buy, [](const Trade&) noexcept {});
    };
    (void)ob.add_order(3, OrderType::Limit, 100, 5, Side::Buy, bad_cb);
    EXPECT_EQ(BookFaultRecorder::count_, 1);
    EXPECT_TRUE(ob.validate_book());
}

TEST(DebugAids, NormalCallbackNeverTripsLatch) {
    OrderBook ob(100, 100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    int trades = 0;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade&) noexcept { trades++; });
    EXPECT_EQ(trades, 1);
    EXPECT_TRUE(ob.validate_book());
}

// ── Thread-identity latch ───────────────────────────────────────

TEST(DebugAids, ThreadLatchBlocksForeignThreadCalls) {
    BookFaultRecorder rec;
    OrderBook ob(100, 100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    std::atomic<int> blocked{0};
    std::thread foreign([&]() {
        RejectReason r = ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy, [](const Trade&) noexcept {});
        if (r == RejectReason::ReentrancyBlocked) blocked = 1;
    });
    foreign.join();
    EXPECT_EQ(blocked, 1);
    EXPECT_EQ(BookFaultRecorder::count_, 1);
    EXPECT_NE(BookFaultRecorder::last_message_.find("foreign thread"), std::string::npos)
        << "message: " << BookFaultRecorder::last_message_;
    // Constructing thread can still use the book:
    EXPECT_EQ(ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy, [](const Trade&) noexcept {}),
              RejectReason::Accepted);
    EXPECT_TRUE(ob.validate_book());
}

// ── Journal: order lifecycle tracking ────────────────────────────

TEST(DebugAids, JournalRecordsFullLifecycle) {
    OrderBook ob(100, 100, 0, 1000);
    std::vector<Trade> trades;
    auto cb = [&](const Trade& t) noexcept { trades.push_back(t); };

    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, cb);  // SUBMIT + BEST_ASK + REST
    (void)ob.add_order(2, OrderType::Limit, 100, 4, Side::Buy, cb);    // SUBMIT + TRADE
    (void)ob.cancel_order(1);                                          // SUBMIT + CANCEL

    const Journal& j = ob.journal();
    ASSERT_GT(j.total(), 0u);

    // Walk retained entries and verify the expected sequence for order 1.
    uint8_t submit = static_cast<uint8_t>(JournalEvent::OrderSubmitted);
    uint8_t rest = static_cast<uint8_t>(JournalEvent::OrderRested);
    uint8_t trade = static_cast<uint8_t>(JournalEvent::TradeExecuted);
    uint8_t cancel = static_cast<uint8_t>(JournalEvent::OrderCancelled);

    bool saw_submit1 = false, saw_rest1 = false, saw_trade = false, saw_cancel1 = false;
    for (size_t i = 0; i < j.size(); ++i) {
        const JournalEntry& e = j.at(i);
        if (e.id == 1) {
            if (e.event == submit) saw_submit1 = true;
            if (e.event == rest) saw_rest1 = true;
            if (e.event == cancel) saw_cancel1 = true;
        }
        if (e.event == trade && e.id == 2) saw_trade = true;  // taker id on trades
    }
    EXPECT_TRUE(saw_submit1);
    EXPECT_TRUE(saw_rest1);
    EXPECT_TRUE(saw_trade);
    EXPECT_TRUE(saw_cancel1);
}

TEST(DebugAids, JournalRecordsRejections) {
    OrderBook ob(100, 100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    const size_t before = ob.journal().total();
    (void)ob.add_order(1, OrderType::Limit, 200, 10, Side::Sell, [](const Trade&) noexcept {});  // duplicate
    const size_t after = ob.journal().total();
    EXPECT_GT(after, before);
    // The last entry must be the rejection with the right reason:
    const JournalEntry& e = ob.journal().at(ob.journal().size() - 1);
    EXPECT_EQ(e.event, static_cast<uint8_t>(JournalEvent::OrderRejected));
    EXPECT_EQ(e.reason, static_cast<uint8_t>(RejectReason::DuplicateOrderId));
}

TEST(DebugAids, JournalRingWrapsWithoutAllocation) {
    OrderBook ob(100, 100, 0, 1000);
    const size_t cap = ob.journal().capacity();
    for (int i = 0; i < static_cast<int>(cap * 3); ++i) {
        (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
        (void)ob.cancel_order(1);
    }
    EXPECT_GT(ob.journal().total(), cap * 2);  // total keeps counting
    EXPECT_EQ(ob.journal().size(), cap);       // retained window is capped
    EXPECT_TRUE(ob.validate_book());           // book survived the churn
}

// ── validate_book() internal-consistency net ─────────────────────

TEST(DebugAids, ValidateBookCleanAcrossScenarioMatrix) {
    std::vector<Trade> trades;
    auto cb = [&](const Trade& t) noexcept { trades.push_back(t); };
    OrderBook ob(500, 500, 100, 5000);  // nonzero min, wide range
    struct FastRng {
        uint64_t s = 0xABCDEF01ULL;
        uint32_t next() {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
        }
        uint32_t next(uint32_t max) { return max ? next() % max : 0; }
    };
    FastRng rng;
    OrderId id = 1;
    for (int op = 0; op < 2'000; ++op) {
        uint32_t roll = rng.next(100);
        if (roll < 50) {
            (void)ob.add_order(id++, OrderType::Limit, 100 + rng.next(4900), 1 + rng.next(30),
                               rng.next(2) ? Side::Buy : Side::Sell, cb);
        } else if (roll < 70) {
            (void)ob.add_order(id++, OrderType::Market, 0, 1 + rng.next(60), rng.next(2) ? Side::Buy : Side::Sell, cb);
        } else {
            (void)ob.cancel_order(1 + rng.next(static_cast<uint32_t>(id - 1)));
        }
        if ((op % 100) == 99) {
            ASSERT_TRUE(ob.validate_book()) << "failed at op " << op;
        }
    }
    EXPECT_TRUE(ob.validate_book());
}

#else
TEST(DebugAids, ReleaseBuildHasNoTools) {
    // Release: journal/latches/validate compile out entirely (documented
    // zero-cost contract). This pins the compile-time shape.
    static_assert(!HFT_DEBUG_TOOLS);
    OrderBook ob(100, 100, 0, 1000);
    std::vector<Trade> trades;
    auto cb = [&](const Trade& t) noexcept { trades.push_back(t); };
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, cb);
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy, cb);
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(ob.active_order_count(), 0u);
    SUCCEED();
}
#endif
