#include <gtest/gtest.h>
#include "../src/OrderBook.h"

using namespace hft;

// ============================================================
// MATCHING TESTS
// ============================================================

TEST(OrderBookTest, SingleMatch) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    
    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    
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
    (void)ob.add_order(2, OrderType::Limit, 100, 5, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    
    ASSERT_EQ(result_trades.size(), 1);
    EXPECT_EQ(result_trades[0].quantity, 5);
    
    // Remaining 5 should be matched by next order
    std::vector<Trade> result2_trades;
    (void)ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    ASSERT_EQ(result2_trades.size(), 1);
    EXPECT_EQ(result2_trades[0].quantity, 5);
}

TEST(OrderBookTest, PriceTimePriority) {
    OrderBook ob(100, 0, 1000);
    // Add two sells at same price
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {}); // First in time
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {}); // Second in time
    // Add a better sell
    (void)ob.add_order(3, OrderType::Limit, 99, 10, Side::Sell, [](const Trade&) noexcept {}); // Best price

    // Match 15 units
    std::vector<Trade> result_trades;
    (void)ob.add_order(4, OrderType::Limit, 100, 15, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    
    ASSERT_EQ(result_trades.size(), 2);
    // Should match Best Price first
    EXPECT_EQ(result_trades[0].maker_id, 3);
    EXPECT_EQ(result_trades[0].quantity, 10);
    EXPECT_EQ(result_trades[0].price, 99); // Price improvement for buyer
    
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
    (void)ob.add_order(4, OrderType::Limit, 105, 30, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });

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
    (void)ob.add_order(2, OrderType::Limit, 99, 10, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    
    EXPECT_EQ(result_trades.size(), 0);
}

TEST(OrderBookTest, SellTakerMatchesBids) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 101, 10, Side::Buy, [](const Trade&) noexcept {});

    // Aggressive sell sweeps from highest bid down
    std::vector<Trade> result_trades;
    (void)ob.add_order(3, OrderType::Limit, 99, 15, Side::Sell, [&](const Trade& t) noexcept { result_trades.push_back(t); });

    ASSERT_EQ(result_trades.size(), 2);
    EXPECT_EQ(result_trades[0].maker_id, 2); // Highest bid first
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
    (void)ob.add_order(3, OrderType::Market, 0, 15, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    
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
    (void)ob.add_order(3, OrderType::Market, 0, 15, Side::Sell, [&](const Trade& t) noexcept { result_trades.push_back(t); });

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
    // No resting orders — market order should be accepted but produce no trades,
    // and should NOT remain in the book.
    std::vector<Trade> result_trades;
    (void)ob.add_order(1, OrderType::Market, 0, 10, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    

    // Adding a sell now should NOT match with the stale market buy
    std::vector<Trade> result2_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell, [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    EXPECT_EQ(result2_trades.size(), 0);
}

// ============================================================
// CANCELLATION TESTS
// ============================================================

TEST(OrderBookTest, CancelExistingOrder) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    
    (void)ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});
    
    
    
    // Try to match — should fail because order 1 was canceled
    std::vector<Trade> result2_trades;
    (void)ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    EXPECT_EQ(result2_trades.size(), 0);
}

TEST(OrderBookTest, CancelNonExistentOrder) {
    OrderBook ob(100, 0, 1000);

    // Cancel an order that doesn't exist; should be a no-op
    (void)ob.add_order(99, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});
    
    // Ensure book is still functional
    std::vector<Trade> result_trades;
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    EXPECT_EQ(result_trades.size(), 0);
}

TEST(OrderBookTest, CancelThenReAdd) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});
    (void)ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});

    // Re-add with same ID should work
    std::vector<Trade> result_trades;
    (void)ob.add_order(1, OrderType::Limit, 105, 20, Side::Sell, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    

    // Matching should use the new order's price/qty
    std::vector<Trade> result2_trades;
    (void)ob.add_order(2, OrderType::Limit, 110, 20, Side::Buy, [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    ASSERT_EQ(result2_trades.size(), 1);
    EXPECT_EQ(result2_trades[0].price, 105);
    EXPECT_EQ(result2_trades[0].quantity, 20);
}

TEST(OrderBookTest, CancelPartiallyFilledOrder) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    // Partially fill: buy 3
    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 3, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    ASSERT_EQ(result_trades.size(), 1);
    EXPECT_EQ(result_trades[0].quantity, 3);

    // Cancel remaining 7
    (void)ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});
    

    // Verify book is empty
    std::vector<Trade> result2_trades;
    (void)ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    EXPECT_EQ(result2_trades.size(), 0);
}

// ============================================================
// DUPLICATE ORDER ID TESTS (Bug #1)
// ============================================================

TEST(OrderBookTest, DuplicateOrderIdRejected) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result1_trades;
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [&](const Trade& t) noexcept { result1_trades.push_back(t); });
    

    // Same ID again — should be rejected
    std::vector<Trade> result2_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 200, 20, Side::Sell, [&](const Trade& t) noexcept { result2_trades.push_back(t); }), RejectReason::DuplicateOrderId);
    
    EXPECT_EQ(result2_trades.size(), 0);

    // Original order should still be in the book
    std::vector<Trade> result3_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result3_trades.push_back(t); });
    EXPECT_EQ(result3_trades[0].price, 100);   // Original price
    EXPECT_EQ(result3_trades[0].quantity, 10);  // Original quantity
}

TEST(OrderBookTest, DuplicateIdAfterFullFill) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    // Fully fill order 1
    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    ASSERT_EQ(result_trades.size(), 1);

    // Reuse ID 1 — should work since it was fully filled and removed
    std::vector<Trade> result2_trades;
    (void)ob.add_order(1, OrderType::Limit, 200, 5, Side::Sell, [&](const Trade& t) noexcept { result2_trades.push_back(t); });
    
}

// ============================================================
// ZERO QUANTITY & INPUT VALIDATION TESTS (Issue #6)
// ============================================================

TEST(OrderBookTest, ZeroQuantityRejected) {
    OrderBook ob(100, 0, 1000);

    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 100, 0, Side::Sell, [&](const Trade& t) noexcept { result_trades.push_back(t); }), RejectReason::InvalidQuantity);
    
    EXPECT_EQ(result_trades.size(), 0);
}

TEST(OrderBookTest, ZeroQuantityMarketRejected) {
    OrderBook ob(100, 0, 1000);

    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Market, 0, 0, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); }), RejectReason::InvalidQuantity);
    
    EXPECT_EQ(result_trades.size(), 0);
}

// ============================================================
// ORDERRESULT STATUS TESTS (Bug #3)
// ============================================================

TEST(OrderBookTest, OrderResultAcceptedOnRest) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [&](const Trade& t) noexcept { result_trades.push_back(t); }), RejectReason::Accepted);
    
    
}

TEST(OrderBookTest, OrderResultAcceptedOnMatch) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    std::vector<Trade> result_trades;
    (void)ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    
    EXPECT_EQ(result_trades.size(), 1);
}

TEST(OrderBookTest, OrderResultCancelSuccess) {
    OrderBook ob(100, 0, 1000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&) noexcept {});

    EXPECT_EQ(ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {}), RejectReason::Accepted);
    
}

TEST(OrderBookTest, OrderResultCancelFailure) {
    OrderBook ob(100, 0, 1000);

    EXPECT_EQ(ob.add_order(99, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {}), RejectReason::CancelFailed);
    
}

// ============================================================
// EMPTY BOOK TESTS
// ============================================================

TEST(OrderBookTest, MatchingOnEmptyBook) {
    OrderBook ob(100, 0, 1000);

    std::vector<Trade> result_trades;
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy, [&](const Trade& t) noexcept { result_trades.push_back(t); });
    
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

    // Pool exhausted — next allocation should be silently dropped (no exception)
    EXPECT_EQ(ob.add_order(4, OrderType::Limit, 99, 10, Side::Buy, [](const Trade&) noexcept {}), RejectReason::PoolExhausted);

    // Verify it doesn't exist in the book by sending a matching Sell order
    std::vector<Trade> match_trades;
    (void)ob.add_order(5, OrderType::Limit, 99, 10, Side::Sell, [&](const Trade& t) noexcept { match_trades.push_back(t);  });
    EXPECT_TRUE(match_trades.empty());
}

TEST(OrderBookTest, AllocDeallocCycleStress) {
    OrderBook ob(10, 0, 1000);
    // Allocate and cancel 100 times with only 10 pool slots
    for (OrderId i = 1; i <= 100; ++i) {
        std::vector<Trade> result_trades;
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, [&](const Trade& t) noexcept { result_trades.push_back(t); }); // Reuse ID 1
    
        (void)ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});
    }
}

// ============================================================
// MEMORY POOL DOUBLE-FREE DETECTION (Bug #2)
// ============================================================

#ifndef NDEBUG
TEST(MemoryPoolTest, DoubleFreeThrows) {
    MemoryPool<Order> pool(10);
    uint32_t o = pool.allocate();
    pool.deallocate(o);

    try {
        pool.deallocate(o);
        FAIL() << "Should have thrown std::logic_error";
    } catch (const std::logic_error& e) {
        SUCCEED();
    }
}

TEST(MemoryPoolTest, OutOfRangeThrows) {
    MemoryPool<Order> pool(10);
    EXPECT_THROW(pool.deallocate(999), std::out_of_range);
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
    pool.deallocate(o); // Should NOT crash in release mode, although free list is corrupted
}
#endif

// ============================================================
// BOUNDS TESTS
// ============================================================

TEST(OrderBookTest, OutOfRangeOrderIdRejected) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(105, OrderType::Limit, 100, 10, Side::Sell, [&](const Trade& t) noexcept { result_trades.push_back(t); }), RejectReason::OutOfBoundsOrderId);
    
    EXPECT_EQ(result_trades.size(), 0);
}

TEST(OrderBookTest, OutOfRangePriceRejected) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result_trades;
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 1005, 10, Side::Sell, [&](const Trade& t) noexcept { result_trades.push_back(t); }), RejectReason::OutOfBoundsPrice);
    
    EXPECT_EQ(result_trades.size(), 0);
}

TEST(OrderBookTest, BoundaryPricesAccepted) {
    OrderBook ob(100, 0, 1000);
    std::vector<Trade> result1_trades;
    (void)ob.add_order(1, OrderType::Limit, 0, 10, Side::Sell, [&](const Trade& t) noexcept { result1_trades.push_back(t); });
    
    std::vector<Trade> result2_trades;
    (void)ob.add_order(2, OrderType::Limit, 1000, 10, Side::Sell, [&](const Trade& t) noexcept { result2_trades.push_back(t); });
}
