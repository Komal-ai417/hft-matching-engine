#include <gtest/gtest.h>
#include "../src/OrderBook.h"

using namespace hft;

TEST(OrderBookTest, SingleMatch) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    
    auto trades = ob.add_order(2, OrderType::Limit, 100, 10, Side::Buy);
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].maker_id, 1);
    EXPECT_EQ(trades[0].taker_id, 2);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 10);
}

TEST(OrderBookTest, PartialFill) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    
    auto trades = ob.add_order(2, OrderType::Limit, 100, 5, Side::Buy); // Buy 5
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 5);
    
    // Remaining 5 should be matched by next order
    auto trades2 = ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy);
    ASSERT_EQ(trades2.size(), 1);
    EXPECT_EQ(trades2[0].quantity, 5);
}

TEST(OrderBookTest, PriceTimePriority) {
    OrderBook ob(100);
    // Add two sells at same price
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell); // First in time
    ob.add_order(2, OrderType::Limit, 100, 10, Side::Sell); // Second in time
    // Add a better sell
    ob.add_order(3, OrderType::Limit, 99, 10, Side::Sell); // Best price

    // Match 15 units
    auto trades = ob.add_order(4, OrderType::Limit, 100, 15, Side::Buy);
    
    ASSERT_EQ(trades.size(), 2);
    // Should match Best Price first
    EXPECT_EQ(trades[0].maker_id, 3);
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_EQ(trades[0].price, 99); // Price improvement for buyer
    
    // Then should match First in Time at next level
    EXPECT_EQ(trades[1].maker_id, 1);
    EXPECT_EQ(trades[1].quantity, 5);
    EXPECT_EQ(trades[1].price, 100);
}

TEST(OrderBookTest, MarketOrders) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    ob.add_order(2, OrderType::Limit, 101, 10, Side::Sell);
    
    auto trades = ob.add_order(3, OrderType::Market, 0, 15, Side::Buy);
    
    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].maker_id, 1);
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_EQ(trades[1].maker_id, 2);
    EXPECT_EQ(trades[1].quantity, 5);
}

TEST(OrderBookTest, Cancellations) {
    OrderBook ob(100);
    ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell);
    
    auto trades = ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy); // Side doesn't matter
    EXPECT_EQ(trades.size(), 0);
    
    // Try to match, should fail because order 1 was canceled
    auto trades2 = ob.add_order(3, OrderType::Limit, 100, 10, Side::Buy);
    EXPECT_EQ(trades2.size(), 0);
}
