#include <gtest/gtest.h>
#include "../src/OrderBook.h"

using namespace hft;

// ============================================================
// MATCHING TESTS
// ============================================================

TEST(OrderBookTest, SingleMatch) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    
    auto result = ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy);
    
    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.trades.size(), 1);
    EXPECT_EQ(result.trades[0].maker_id, 1);
    EXPECT_EQ(result.trades[0].taker_id, 2);
    EXPECT_EQ(result.trades[0].price, 100);
    EXPECT_EQ(result.trades[0].quantity, 10);
}

TEST(OrderBookTest, PartialFill) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    
    auto result = ob.add_order(2, OrderType::Limit, 100, 5, Side::Buy);
    
    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.trades.size(), 1);
    EXPECT_EQ(result.trades[0].quantity, 5);
    
    // Remaining 5 should be matched by next order
    auto result2 = ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy);
    ASSERT_TRUE(result2.accepted);
    ASSERT_EQ(result2.trades.size(), 1);
    EXPECT_EQ(result2.trades[0].quantity, 5);
}

TEST(OrderBookTest, PriceTimePriority) {
    OrderBook ob(100);
    // Add two sells at same price
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell); // First in time
    ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell); // Second in time
    // Add a better sell
    ob.add_order(3, OrderType::Limit, 99, 10, Side::Sell); // Best price

    // Match 15 units
    auto result = ob.add_order(4, OrderType::Limit, 100, 15, Side::Buy);
    
    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.trades.size(), 2);
    // Should match Best Price first
    EXPECT_EQ(result.trades[0].maker_id, 3);
    EXPECT_EQ(result.trades[0].quantity, 10);
    EXPECT_EQ(result.trades[0].price, 99); // Price improvement for buyer
    
    // Then should match First in Time at next level
    EXPECT_EQ(result.trades[1].maker_id, 1);
    EXPECT_EQ(result.trades[1].quantity, 5);
    EXPECT_EQ(result.trades[1].price, 100);
}

TEST(OrderBookTest, SweepAcrossMultipleLevels) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    ob.add_order(2, OrderType::Limit, 101, 10, Side::Sell);
    ob.add_order(3, OrderType::Limit, 102, 10, Side::Sell);

    // Sweep all 3 levels
    auto result = ob.add_order(4, OrderType::Limit, 105, 30, Side::Buy);

    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.trades.size(), 3);
    EXPECT_EQ(result.trades[0].price, 100);
    EXPECT_EQ(result.trades[1].price, 101);
    EXPECT_EQ(result.trades[2].price, 102);
}

TEST(OrderBookTest, NoMatchWhenPriceDontCross) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);

    // Buy at 99 — shouldn't cross
    auto result = ob.add_order(2, OrderType::Limit, 99, 10, Side::Buy);

    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.trades.size(), 0);
}

TEST(OrderBookTest, SellTakerMatchesBids) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy);
    ob.add_order(2, OrderType::Limit, 101, 10, Side::Buy);

    // Aggressive sell sweeps from highest bid down
    auto result = ob.add_order(3, OrderType::Limit, 99, 15, Side::Sell);

    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.trades.size(), 2);
    EXPECT_EQ(result.trades[0].maker_id, 2); // Highest bid first
    EXPECT_EQ(result.trades[0].price, 101);   // Price improvement for seller
    EXPECT_EQ(result.trades[1].maker_id, 1);
    EXPECT_EQ(result.trades[1].price, 100);
}

// ============================================================
// MARKET ORDER TESTS
// ============================================================

TEST(OrderBookTest, MarketBuyOrder) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    ob.add_order(2, OrderType::Limit, 101, 10, Side::Sell);
    
    auto result = ob.add_order(3, OrderType::Market, 0, 15, Side::Buy);
    
    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.trades.size(), 2);
    EXPECT_EQ(result.trades[0].maker_id, 1);
    EXPECT_EQ(result.trades[0].quantity, 10);
    EXPECT_EQ(result.trades[1].maker_id, 2);
    EXPECT_EQ(result.trades[1].quantity, 5);
}

TEST(OrderBookTest, MarketSellOrder) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy);
    ob.add_order(2, OrderType::Limit, 99, 10, Side::Buy);

    auto result = ob.add_order(3, OrderType::Market, 0, 15, Side::Sell);

    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.trades.size(), 2);
    EXPECT_EQ(result.trades[0].maker_id, 1);  // Highest bid first
    EXPECT_EQ(result.trades[0].price, 100);
    EXPECT_EQ(result.trades[0].quantity, 10);
    EXPECT_EQ(result.trades[1].maker_id, 2);
    EXPECT_EQ(result.trades[1].price, 99);
    EXPECT_EQ(result.trades[1].quantity, 5);
}

TEST(OrderBookTest, MarketOrderDoesNotRestInBook) {
    OrderBook ob(100);
    // No resting orders — market order should be accepted but produce no trades,
    // and should NOT remain in the book.
    auto result = ob.add_order(1, OrderType::Market, 0, 10, Side::Buy);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.trades.size(), 0);

    // Adding a sell now should NOT match with the stale market buy
    auto result2 = ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell);
    ASSERT_TRUE(result2.accepted);
    EXPECT_EQ(result2.trades.size(), 0);
}

// ============================================================
// CANCELLATION TESTS
// ============================================================

TEST(OrderBookTest, CancelExistingOrder) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    
    auto result = ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy);
    EXPECT_TRUE(result.cancelled);
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.trades.size(), 0);
    
    // Try to match — should fail because order 1 was canceled
    auto result2 = ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy);
    EXPECT_EQ(result2.trades.size(), 0);
}

TEST(OrderBookTest, CancelNonExistentOrder) {
    OrderBook ob(100);

    auto result = ob.add_order(999, OrderType::Cancel, 0, 0, Side::Buy);
    EXPECT_FALSE(result.cancelled);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.trades.size(), 0);
}

TEST(OrderBookTest, CancelThenReAdd) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy);

    // Re-add with same ID should work
    auto result = ob.add_order(1, OrderType::Limit, 105, 20, Side::Sell);
    ASSERT_TRUE(result.accepted);

    // Matching should use the new order's price/qty
    auto result2 = ob.add_order(2, OrderType::Limit, 110, 20, Side::Buy);
    ASSERT_EQ(result2.trades.size(), 1);
    EXPECT_EQ(result2.trades[0].price, 105);
    EXPECT_EQ(result2.trades[0].quantity, 20);
}

TEST(OrderBookTest, CancelPartiallyFilledOrder) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);

    // Partially fill: buy 3
    auto result = ob.add_order(2, OrderType::Limit, 100, 3, Side::Buy);
    ASSERT_EQ(result.trades.size(), 1);
    EXPECT_EQ(result.trades[0].quantity, 3);

    // Cancel remaining 7
    auto cancel_result = ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy);
    EXPECT_TRUE(cancel_result.cancelled);

    // Verify book is empty
    auto result2 = ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy);
    EXPECT_EQ(result2.trades.size(), 0);
}

// ============================================================
// DUPLICATE ORDER ID TESTS (Bug #1)
// ============================================================

TEST(OrderBookTest, DuplicateOrderIdRejected) {
    OrderBook ob(100);
    auto result1 = ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    ASSERT_TRUE(result1.accepted);

    // Same ID again — should be rejected
    auto result2 = ob.add_order(1, OrderType::Limit, 200, 20, Side::Sell);
    ASSERT_FALSE(result2.accepted);
    EXPECT_EQ(result2.trades.size(), 0);

    // Original order should still be in the book
    auto result3 = ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy);
    ASSERT_EQ(result3.trades.size(), 1);
    EXPECT_EQ(result3.trades[0].price, 100);   // Original price
    EXPECT_EQ(result3.trades[0].quantity, 10);  // Original quantity
}

TEST(OrderBookTest, DuplicateIdAfterFullFill) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);

    // Fully fill order 1
    auto result = ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy);
    ASSERT_EQ(result.trades.size(), 1);

    // Reuse ID 1 — should work since it was fully filled and removed
    auto result2 = ob.add_order(1, OrderType::Limit, 200, 5, Side::Sell);
    ASSERT_TRUE(result2.accepted);
}

// ============================================================
// ZERO QUANTITY & INPUT VALIDATION TESTS (Issue #6)
// ============================================================

TEST(OrderBookTest, ZeroQuantityRejected) {
    OrderBook ob(100);

    auto result = ob.add_order(1, OrderType::Limit, 100, 0, Side::Sell);
    ASSERT_FALSE(result.accepted);
    EXPECT_EQ(result.trades.size(), 0);
}

TEST(OrderBookTest, ZeroQuantityMarketRejected) {
    OrderBook ob(100);

    auto result = ob.add_order(1, OrderType::Market, 0, 0, Side::Buy);
    ASSERT_FALSE(result.accepted);
}

// ============================================================
// ORDERRESULT STATUS TESTS (Bug #3)
// ============================================================

TEST(OrderBookTest, OrderResultAcceptedOnRest) {
    OrderBook ob(100);
    auto result = ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    EXPECT_TRUE(result.accepted);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.trades.size(), 0);
}

TEST(OrderBookTest, OrderResultAcceptedOnMatch) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);

    auto result = ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy);
    EXPECT_TRUE(result.accepted);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.trades.size(), 1);
}

TEST(OrderBookTest, OrderResultCancelSuccess) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);

    auto result = ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy);
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.cancelled);
}

TEST(OrderBookTest, OrderResultCancelFailure) {
    OrderBook ob(100);

    auto result = ob.add_order(999, OrderType::Cancel, 0, 0, Side::Buy);
    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.cancelled);
}

// ============================================================
// EMPTY BOOK TESTS
// ============================================================

TEST(OrderBookTest, MatchingOnEmptyBook) {
    OrderBook ob(100);

    auto result = ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.trades.size(), 0);
}

// ============================================================
// POOL BOUNDARY TESTS
// ============================================================

TEST(OrderBookTest, ExhaustPoolThrows) {
    OrderBook ob(3);  // Only 3 slots
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    ob.add_order(2, OrderType::Limit, 101, 10, Side::Sell);
    ob.add_order(3, OrderType::Limit, 102, 10, Side::Sell);

    // Pool exhausted — next allocation should throw
    EXPECT_THROW(
        ob.add_order(4, OrderType::Limit, 103, 10, Side::Sell),
        std::bad_alloc
    );
}

TEST(OrderBookTest, AllocDeallocCycleStress) {
    OrderBook ob(10);
    // Allocate and cancel 100 times with only 10 pool slots
    for (uint64_t i = 1; i <= 100; ++i) {
        auto result = ob.add_order(i, OrderType::Limit, 100, 10, Side::Sell);
        ASSERT_TRUE(result.accepted) << "Failed on iteration " << i;
        ob.add_order(i, OrderType::Cancel, 0, 0, Side::Buy);
    }
}

// ============================================================
// MEMORY POOL DOUBLE-FREE DETECTION (Bug #2)
// ============================================================

TEST(MemoryPoolTest, DoubleFreeThrows) {
    MemoryPool<Order> pool(10);
    Order* o = pool.allocate();
    pool.deallocate(o);

    EXPECT_THROW(pool.deallocate(o), std::logic_error);
}

TEST(MemoryPoolTest, OutOfRangeThrows) {
    MemoryPool<Order> pool(10);
    Order dummy;
    EXPECT_THROW(pool.deallocate(&dummy), std::out_of_range);
}

TEST(MemoryPoolTest, IsAllocatedTracking) {
    MemoryPool<Order> pool(10);
    Order* o = pool.allocate();
    EXPECT_TRUE(pool.is_allocated(o));

    pool.deallocate(o);
    EXPECT_FALSE(pool.is_allocated(o));
}
