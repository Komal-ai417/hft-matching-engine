// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Karyampudi Komal
//
// ============================================================================
// test_ob.cpp — core OrderBook behaviour: matching, market orders, cancels,
// duplicate ids, input validation, pool boundaries, and the pool's
// double-free detection exercised directly through MemoryPool.
// ============================================================================
#include <gtest/gtest.h>

#include <vector>

#include "../src/OrderBook.h"

using namespace hft;

// ============================================================
// MATCHING TESTS
// ============================================================

TEST(OrderBookTest, SingleMatch) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    ASSERT_EQ(result_trades.size(), 1);
    EXPECT_EQ(result_trades[0].maker_id, 1);
    EXPECT_EQ(result_trades[0].taker_id, 2);
    EXPECT_EQ(result_trades[0].price, 100);
    EXPECT_EQ(result_trades[0].quantity, 10);
}

TEST(OrderBookTest, PartialFill) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 5, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    ASSERT_EQ(result_trades.size(), 1);
    EXPECT_EQ(result_trades[0].quantity, 5);

    // Remaining 5 should be matched by next order
    std::vector<Trade> result2_trades;
    (void)ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    ASSERT_EQ(result2_trades.size(), 1);
    EXPECT_EQ(result2_trades[0].quantity, 5);
}

TEST(OrderBookTest, PriceTimePriority) {
    OrderBook ob(100, 0, 1000);
    // Add two sells at same price
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});  // First in time
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});  // Second in time
    // Add a better sell
    (void)ob.add_order(3, OrderType::Limit, 99, 10, Side::Sell, [](const Trade&) noexcept {});  // Best price

    // Match 15 units
    std::vector<Trade> result_trades;
    (void)ob.add_order(4, OrderType::Limit, 100, 15, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    ASSERT_EQ(result_trades.size(), 2);
    // Should match Best Price first
    EXPECT_EQ(result_trades[0].maker_id, 3);
    EXPECT_EQ(result_trades[0].quantity, 10);
    EXPECT_EQ(result_trades[0].price, 99);  // Price improvement for buyer

    // Then should match First in Time at next level
    EXPECT_EQ(result_trades[1].maker_id, 1);
    EXPECT_EQ(result_trades[1].quantity, 5);
    EXPECT_EQ(result_trades[1].price, 100);
}

TEST(OrderBookTest, SweepAcrossMultipleLevels) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 101, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(3, OrderType::Limit, 102, 10, Side::Sell, [](const Trade&) noexcept {});

    // Sweep all 3 levels
    std::vector<Trade> result_trades;
    (void)ob.add_order(4, OrderType::Limit, 105, 30, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    ASSERT_EQ(result_trades.size(), 3);
    EXPECT_EQ(result_trades[0].price, 100);
    EXPECT_EQ(result_trades[1].price, 101);
    EXPECT_EQ(result_trades[2].price, 102);
}

TEST(OrderBookTest, NoMatchWhenPriceDontCross) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    // Buy at 99 — shouldn't cross
    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 99, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    EXPECT_EQ(result_trades.size(), 0);
    // the non-crossing taker must REST in the book
    EXPECT_TRUE(ob.order_exists(2));
    EXPECT_EQ(ob.active_order_count(), 2);
}

TEST(OrderBookTest, SellTakerMatchesBids) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 101, 10, Side::Buy, [](const Trade&) noexcept {});

    // Aggressive sell sweeps from highest bid down
    std::vector<Trade> result_trades;
    (void)ob.add_order(3, OrderType::Limit, 99, 15, Side::Sell,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    ASSERT_EQ(result_trades.size(), 2);
    EXPECT_EQ(result_trades[0].maker_id, 2);  // Highest bid first
    EXPECT_EQ(result_trades[0].price, 101);   // Price improvement for seller
    EXPECT_EQ(result_trades[1].maker_id, 1);
    EXPECT_EQ(result_trades[1].price, 100);
}

// ============================================================
// MARKET ORDER TESTS
// ============================================================

TEST(OrderBookTest, MarketBuyOrder) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 101, 10, Side::Sell, [](const Trade&) noexcept {});

    std::vector<Trade> result_trades;
    (void)ob.add_order(3, OrderType::Market, 0, 15, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    ASSERT_EQ(result_trades.size(), 2);
    EXPECT_EQ(result_trades[0].maker_id, 1);
    EXPECT_EQ(result_trades[0].quantity, 10);
    EXPECT_EQ(result_trades[1].maker_id, 2);
    EXPECT_EQ(result_trades[1].quantity, 5);
}

TEST(OrderBookTest, MarketSellOrder) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 99, 10, Side::Buy, [](const Trade&) noexcept {});

    std::vector<Trade> result_trades;
    (void)ob.add_order(3, OrderType::Market, 0, 15, Side::Sell,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    ASSERT_EQ(result_trades.size(), 2);
    EXPECT_EQ(result_trades[0].maker_id, 1);  // Highest bid first
    EXPECT_EQ(result_trades[0].price, 100);
    EXPECT_EQ(result_trades[0].quantity, 10);
    EXPECT_EQ(result_trades[1].maker_id, 2);
    EXPECT_EQ(result_trades[1].price, 99);
    EXPECT_EQ(result_trades[1].quantity, 5);
}

TEST(OrderBookTest, MarketOrderDoesNotRestInBook) {
    OrderBook ob(100, 0, 1000);
    // No resting orders — the market order is REJECTED (nothing to trade
    // against), produces no trades, and does NOT remain in the book.
    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Market, 0, 10, Side::Buy,
                           [&](const Trade& t) noexcept { result_trades.push_back(t); }),
              RejectReason::MarketOnEmptyBook);
    EXPECT_EQ(result_trades.size(), 0);
    EXPECT_FALSE(ob.order_exists(1));
    EXPECT_EQ(ob.active_order_count(), 0);

    // Adding a sell now should NOT match with the stale market buy
    std::vector<Trade> result2_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell,
                       [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    EXPECT_EQ(result2_trades.size(), 0);
}

// The filled-quantity out-parameter makes market partial fills visible to
// callers.
TEST(OrderBookTest, MarketPartialFillSurfacesFilledQuantity) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    Quantity filled = 0;
    std::vector<Trade> trades;
    EXPECT_EQ(
        ob.add_order(
            2, OrderType::Market, 0, 100, Side::Buy, [&](const Trade& t) noexcept { trades.push_back(t); }, &filled),
        RejectReason::Accepted);
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(filled, 10);  // only 10 available; 90 unfilled is now VISIBLE
}

// ============================================================
// CANCELLATION TESTS
// ============================================================

TEST(OrderBookTest, CancelExistingOrder) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    (void)ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});

    EXPECT_FALSE(ob.order_exists(1));
    EXPECT_EQ(ob.active_order_count(), 0);

    // Try to match — should fail because order 1 was canceled
    std::vector<Trade> result2_trades;
    (void)ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    EXPECT_EQ(result2_trades.size(), 0);
}

TEST(OrderBookTest, CancelNonExistentOrder) {
    OrderBook ob(100, 0, 1000);

    // Cancel an order that doesn't exist; should be a no-op
    (void)ob.add_order(99, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});

    // Ensure book is still functional
    std::vector<Trade> result_trades;
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });
    EXPECT_EQ(result_trades.size(), 0);
    EXPECT_TRUE(ob.order_exists(1));
}

TEST(OrderBookTest, CancelThenReAdd) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});

    // Re-add with same ID should work
    std::vector<Trade> result_trades;
    (void)ob.add_order(1, OrderType::Limit, 105, 20, Side::Sell,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    // Matching should use the new order's price/qty
    std::vector<Trade> result2_trades;
    (void)ob.add_order(2, OrderType::Limit, 110, 20, Side::Buy,
                       [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    ASSERT_EQ(result2_trades.size(), 1);
    EXPECT_EQ(result2_trades[0].price, 105);
    EXPECT_EQ(result2_trades[0].quantity, 20);
}

TEST(OrderBookTest, CancelPartiallyFilledOrder) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    // Partially fill: buy 3
    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 3, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });
    ASSERT_EQ(result_trades.size(), 1);
    EXPECT_EQ(result_trades[0].quantity, 3);

    // Cancel remaining 7
    (void)ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});

    // Verify book is empty
    std::vector<Trade> result2_trades;
    (void)ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    EXPECT_EQ(result2_trades.size(), 0);
}

// ============================================================
// DUPLICATE ORDER ID TESTS
// ============================================================

TEST(OrderBookTest, DuplicateOrderIdRejected) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result1_trades;
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell,
                       [&](const Trade& t) noexcept { result1_trades.push_back(t); });

    // Same ID again — should be rejected
    std::vector<Trade> result2_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 200, 20, Side::Sell,
                           [&](const Trade& t) noexcept { result2_trades.push_back(t); }),
              RejectReason::DuplicateOrderId);

    EXPECT_EQ(result2_trades.size(), 0);

    // Original order should still be in the book
    std::vector<Trade> result3_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result3_trades.push_back(t); });
    EXPECT_EQ(result3_trades[0].price, 100);    // Original price
    EXPECT_EQ(result3_trades[0].quantity, 10);  // Original quantity
}

TEST(OrderBookTest, DuplicateIdAfterFullFill) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    // Fully fill order 1
    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });
    ASSERT_EQ(result_trades.size(), 1);

    // Reuse ID 1 — should work since it was fully filled and removed
    std::vector<Trade> result2_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 200, 5, Side::Sell,
                           [&](const Trade& t) noexcept { result2_trades.push_back(t); }),
              RejectReason::Accepted);
    EXPECT_TRUE(ob.order_exists(1));

    // prove REPEATED reuse, not just one.
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    std::vector<Trade> result3_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 300, 7, Side::Sell,
                           [&](const Trade& t) noexcept { result3_trades.push_back(t); }),
              RejectReason::Accepted);
    EXPECT_EQ(ob.level_total_quantity(Side::Sell, 300), 7);
}

// ============================================================
// ZERO QUANTITY & INPUT VALIDATION TESTS
// ============================================================

TEST(OrderBookTest, ZeroQuantityRejected) {
    OrderBook ob(100, 0, 1000);

    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 100, 0, Side::Sell,
                           [&](const Trade& t) noexcept { result_trades.push_back(t); }),
              RejectReason::InvalidQuantity);

    EXPECT_EQ(result_trades.size(), 0);
}

TEST(OrderBookTest, ZeroQuantityMarketRejected) {
    OrderBook ob(100, 0, 1000);

    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Market, 0, 0, Side::Buy,
                           [&](const Trade& t) noexcept { result_trades.push_back(t); }),
              RejectReason::InvalidQuantity);

    EXPECT_EQ(result_trades.size(), 0);
}

// ============================================================
// ORDERRESULT STATUS TESTS
// ============================================================

TEST(OrderBookTest, OrderResultAcceptedOnRest) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell,
                           [&](const Trade& t) noexcept { result_trades.push_back(t); }),
              RejectReason::Accepted);
    EXPECT_TRUE(ob.order_exists(1));
}

TEST(OrderBookTest, OrderResultAcceptedOnMatch) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    std::vector<Trade> result_trades;
    // the test is named for the result code — assert it, not just the trade
    // count.
    EXPECT_EQ(ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy,
                           [&](const Trade& t) noexcept { result_trades.push_back(t); }),
              RejectReason::Accepted);

    EXPECT_EQ(result_trades.size(), 1);
}

TEST(OrderBookTest, OrderResultCancelSuccess) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    EXPECT_EQ(ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {}),
              RejectReason::Accepted);
}

TEST(OrderBookTest, OrderResultCancelFailure) {
    OrderBook ob(100, 0, 1000);

    EXPECT_EQ(ob.add_order(99, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {}),
              RejectReason::CancelFailed);
}

// ============================================================
// EMPTY BOOK TESTS
// ============================================================

TEST(OrderBookTest, MatchingOnEmptyBook) {
    OrderBook ob(100, 0, 1000);

    std::vector<Trade> result_trades;
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy,
                       [&](const Trade& t) noexcept { result_trades.push_back(t); });

    EXPECT_EQ(result_trades.size(), 0);
}

// ============================================================
// POOL BOUNDARY TESTS
// ============================================================

TEST(OrderBookTest, ExhaustPoolSilentDrop) {
    // max_order_id = 10, max_active_orders = 3
    OrderBook ob(10, 3, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 101, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(3, OrderType::Limit, 102, 10, Side::Sell, [](const Trade&) noexcept {});

    // Pool exhausted — next allocation should be rejected (no exception)
    EXPECT_EQ(ob.add_order(4, OrderType::Limit, 99, 10, Side::Buy, [](const Trade&) noexcept {}),
              RejectReason::PoolExhausted);

    // Verify it doesn't exist in the book by sending a matching Sell order
    std::vector<Trade> match_trades;
    (void)ob.add_order(5, OrderType::Limit, 99, 10, Side::Sell,
                       [&](const Trade& t) noexcept { match_trades.push_back(t); });
    EXPECT_TRUE(match_trades.empty());
}

// 1,000,000 cycles against a 10-slot pool, with real invariant assertions.
TEST(OrderBookTest, AllocDeallocCycleStress) {
    OrderBook ob(10, 0, 1000);
    // Allocate and cancel 1,000,000 times with only 10 pool slots.
    for (OrderId i = 1; i <= 1'000'000; ++i) {
        (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});  // Reuse ID 1
        (void)ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});
    }
    // After 1M cycles the capacity must have floated back to maximum exactly:
    EXPECT_EQ(ob.active_order_count(), 0);
    EXPECT_FALSE(ob.order_exists(1));
    EXPECT_EQ(ob.level_order_count(Side::Sell, 100), 0);
    EXPECT_EQ(ob.price_level_count(Side::Sell), 0);
    // And the book must still be fully functional:
    std::vector<Trade> trades;
    EXPECT_EQ(
        ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell, [&](const Trade& t) noexcept { trades.push_back(t); }),
        RejectReason::Accepted);
    EXPECT_EQ(ob.active_order_count(), 1);
#if HFT_DEBUG_TOOLS
    EXPECT_TRUE(ob.validate_book());
#endif
}

// fill the pool, fail, cancel one, succeed again.
TEST(OrderBookTest, PoolExhaustionRecovery) {
    OrderBook ob(10, 3, 0, 1000);
    for (OrderId id = 1; id <= 3; ++id) {
        EXPECT_EQ(ob.add_order(id, OrderType::Limit, 100 + id, 10, Side::Sell, [](const Trade&) noexcept {}),
                  RejectReason::Accepted);
    }
    EXPECT_EQ(ob.add_order(4, OrderType::Limit, 50, 10, Side::Sell, [](const Trade&) noexcept {}),
              RejectReason::PoolExhausted);
    EXPECT_EQ(ob.cancel_order(2), RejectReason::Accepted);
    EXPECT_EQ(ob.add_order(4, OrderType::Limit, 50, 10, Side::Sell, [](const Trade&) noexcept {}),
              RejectReason::Accepted);
    EXPECT_EQ(ob.active_order_count(), 3);
}

// ============================================================
// MEMORY POOL DOUBLE-FREE DETECTION
//   The pool's own double-free / out-of-range / chain tests live in
//   test_memory_pool.cpp (exception-free fault-handler semantics). These
//   variants keep coverage of the pool through the OrderBook API.
// ============================================================

#if HFT_DEBUG_TOOLS
namespace {
// Captures pool fault messages; chooses SkipOperation so the faulting call
// returns instead of aborting (see MemoryPool.h: exception-free faults).
struct PoolFaultRecorder {
    int count = 0;
    std::string last_message;
    static FaultAction handler(const char* msg, size_t) noexcept {
        auto* self = static_cast<PoolFaultRecorder*>(g_current);
        self->count++;
        self->last_message = msg;
        return FaultAction::SkipOperation;
    }
    inline static PoolFaultRecorder* g_current = nullptr;
    PoolFaultRecorder() {
        g_current = this;
        set_pool_fault_handler(&handler);
    }
    ~PoolFaultRecorder() {
        set_pool_fault_handler(nullptr);
        g_current = nullptr;
    }
};
}  // namespace

TEST(MemoryPoolTest, DoubleFreeDetectedViaFaultHandler) {
    PoolFaultRecorder rec;
    MemoryPool<Order> pool(10);
    uint32_t o = pool.allocate();
    pool.deallocate(o);
    EXPECT_EQ(rec.count, 0);
    pool.deallocate(o);  // double free -> fault handler, no abort
    EXPECT_EQ(rec.count, 1);
    EXPECT_NE(rec.last_message.find("double free"), std::string::npos);
    // The pool must remain internally consistent after the skipped operation:
    EXPECT_FALSE(pool.is_allocated(o));
    EXPECT_EQ(pool.stats().live_count, 0);
    EXPECT_EQ(pool.stats().free_list_depth, 10);
    uint32_t p = pool.allocate();  // still fully functional
    EXPECT_TRUE(pool.is_allocated(p));
}

TEST(MemoryPoolTest, OutOfRangeFreeDetectedViaFaultHandler) {
    PoolFaultRecorder rec;
    MemoryPool<Order> pool(10);
    pool.deallocate(999);  // out of range -> fault, no abort
    EXPECT_EQ(rec.count, 1);
    EXPECT_NE(rec.last_message.find("does not belong to this pool"), std::string::npos)
        << "message was: " << rec.last_message;
    EXPECT_EQ(pool.stats().live_count, 0);
}

TEST(MemoryPoolTest, IsAllocatedTracking) {
    MemoryPool<Order> pool(10);
    uint32_t o = pool.allocate();
    EXPECT_TRUE(pool.is_allocated(o));
    pool.deallocate(o);
    EXPECT_FALSE(pool.is_allocated(o));
}
#else
TEST(MemoryPoolTest, DeallocDoesNotCrash) {
    MemoryPool<Order> pool(10);
    uint32_t o = pool.allocate();
    pool.deallocate(o);
    pool.deallocate(o);  // Release: documented-trust contract (no guard compiled in)
}
#endif

// ============================================================
// BOUNDS TESTS
// ============================================================

TEST(OrderBookTest, OutOfRangeOrderIdRejected) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(105, OrderType::Limit, 100, 10, Side::Sell,
                           [&](const Trade& t) noexcept { result_trades.push_back(t); }),
              RejectReason::OutOfBoundsOrderId);
    EXPECT_EQ(result_trades.size(), 0);
    // Boundary IDs are accepted:
    EXPECT_EQ(ob.add_order(100, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {}),
              RejectReason::Accepted);
    EXPECT_EQ(ob.add_order(0, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {}),
              RejectReason::OutOfBoundsOrderId);
}

TEST(OrderBookTest, OutOfRangePriceRejected) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 1005, 10, Side::Sell,
                           [&](const Trade& t) noexcept { result_trades.push_back(t); }),
              RejectReason::OutOfBoundsPrice);
    EXPECT_EQ(result_trades.size(), 0);
}

// asserts acceptance AND that both orders actually rested, using the
// introspection API.
TEST(OrderBookTest, BoundaryPricesAccepted) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result1_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 0, 10, Side::Sell,
                           [&](const Trade& t) noexcept { result1_trades.push_back(t); }),
              RejectReason::Accepted);
    std::vector<Trade> result2_trades;
    EXPECT_EQ(ob.add_order(2, OrderType::Limit, 1000, 10, Side::Sell,
                           [&](const Trade& t) noexcept { result2_trades.push_back(t); }),
              RejectReason::Accepted);

    EXPECT_EQ(result1_trades.size(), 0);
    EXPECT_EQ(result2_trades.size(), 0);
    EXPECT_TRUE(ob.order_exists(1));
    EXPECT_TRUE(ob.order_exists(2));
    EXPECT_EQ(ob.best_ask(), 0);  // min price is the best ask
    EXPECT_EQ(ob.price_level_count(Side::Sell), 2);
    EXPECT_EQ(ob.level_order_count(Side::Sell, 0), 1);
    EXPECT_EQ(ob.level_order_count(Side::Sell, 1000), 1);
}

// copy must be deleted; move must work.
TEST(OrderBookTest, CopyDeletedMoveAllowed) {
    static_assert(!std::is_copy_constructible_v<OrderBook>, "OrderBook copy must be deleted");
    static_assert(!std::is_copy_assignable_v<OrderBook>, "OrderBook copy-assign must be deleted");
    static_assert(std::is_nothrow_move_constructible_v<OrderBook>, "OrderBook move must be noexcept");
    OrderBook ob(100, 100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    OrderBook moved = std::move(ob);
    EXPECT_TRUE(moved.order_exists(1));  // indices stay valid across the move
    std::vector<Trade> trades;
    EXPECT_EQ(
        moved.add_order(2, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { trades.push_back(t); }),
        RejectReason::Accepted);
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].maker_id, 1);
}
