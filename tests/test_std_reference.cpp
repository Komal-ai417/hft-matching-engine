// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Karyampudi Komal
//
// ============================================================================
// test_std_reference.cpp — StdOrderBook reference tests: the benchmark
// baseline AND the differential-testing oracle.
// ============================================================================

#include <gtest/gtest.h>

#include <vector>

#include "StdOrderBook.h"

using namespace hft;

namespace {
struct Trades {
    std::vector<Trade> v;
    auto cb() & {
        return [this](const Trade& t) noexcept { v.push_back(t); };
    }
};
}  // namespace

TEST(StdReference, BasicMatchParity) {
    Trades t;
    StdOrderBook sb(100, 100, 0, 1000);
    (void)sb.add_order(1, OrderType::Limit, 100, 10, Side::Sell, t.cb());
    (void)sb.add_order(2, OrderType::Limit, 100, 10, Side::Buy, t.cb());
    ASSERT_EQ(t.v.size(), 1);
    EXPECT_EQ(t.v[0].maker_id, 1);
    EXPECT_EQ(t.v[0].taker_id, 2);
    EXPECT_EQ(t.v[0].price, 100);
    EXPECT_EQ(t.v[0].quantity, 10);
    EXPECT_EQ(sb.active_order_count(), 0u);
}

// A buy taker that fully fills resting asks must erase order_map_ AND
// side_map_ — a missing side_map_ erase is an unbounded leak that skews the
// published memory benchmark ratios.
TEST(StdReference, SideMapDoesNotLeakOnBuyTakerFills) {
    Trades t;
    StdOrderBook sb(10'000, 10'000, 0, 1000);
    const size_t rounds = 5'000;
    for (OrderId id = 1; id <= rounds; ++id) {
        // Each round: resting sell fully consumed by a crossing buy (taker
        // uses a distinct id — reusing the maker's would trip the duplicate
        // check before any matching happens).
        (void)sb.add_order(id, OrderType::Limit, 100, 10, Side::Sell, t.cb());
        (void)sb.add_order(rounds + id, OrderType::Limit, 100, 10, Side::Buy, t.cb());
    }
    EXPECT_EQ(sb.active_order_count(), 0u);
    EXPECT_EQ(sb.order_exists(rounds), false);
    // side_map_ must be empty — one stale entry per fill would leak.
    EXPECT_EQ(sb.side_map_size(), 0u) << "side_map_ leaked " << sb.side_map_size() << " entries after " << rounds
                                      << " fully-filled asks";
}

TEST(StdReference, ValidationParityWithEngine) {
    Trades t;
    StdOrderBook sb(100, 100, 0, 1000);
    EXPECT_EQ(sb.add_order(1, OrderType::Limit, 100, 0, Side::Sell, t.cb()), RejectReason::InvalidQuantity);
    EXPECT_EQ(sb.add_order(0, OrderType::Limit, 100, 5, Side::Sell, t.cb()), RejectReason::OutOfBoundsOrderId);
    EXPECT_EQ(sb.add_order(101, OrderType::Limit, 100, 5, Side::Sell, t.cb()), RejectReason::OutOfBoundsOrderId);
    (void)sb.add_order(1, OrderType::Limit, 100, 5, Side::Sell, t.cb());
    EXPECT_EQ(sb.add_order(1, OrderType::Limit, 200, 5, Side::Sell, t.cb()), RejectReason::DuplicateOrderId);
    EXPECT_EQ(sb.add_order(2, OrderType::Limit, 2000, 5, Side::Sell, t.cb()), RejectReason::OutOfBoundsPrice);
    EXPECT_EQ(sb.add_order(2, OrderType::Market, 0, 5, Side::Sell, t.cb()), RejectReason::MarketOnEmptyBook);
    // Cancel semantics:
    EXPECT_EQ(sb.cancel_order(42), RejectReason::CancelFailed);
    EXPECT_EQ(sb.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(sb.cancel_order(1), RejectReason::CancelFailed);
}

TEST(StdReference, MarketRemainderDiscardedAndFilledReported) {
    Trades t;
    StdOrderBook sb(100, 100, 0, 1000);
    (void)sb.add_order(1, OrderType::Limit, 100, 10, Side::Sell, t.cb());
    Quantity filled = 0;
    EXPECT_EQ(sb.add_order(2, OrderType::Market, 0, 100, Side::Buy, t.cb(), &filled), RejectReason::Accepted);
    EXPECT_EQ(filled, 10);
    EXPECT_EQ(sb.active_order_count(), 0u);  // market remainder never rests
}

TEST(StdReference, PoolExhaustionParity) {
    Trades t;
    StdOrderBook sb(100, 3, 0, 1000);
    EXPECT_EQ(sb.add_order(1, OrderType::Limit, 100, 10, Side::Sell, t.cb()), RejectReason::Accepted);
    EXPECT_EQ(sb.add_order(2, OrderType::Limit, 110, 10, Side::Sell, t.cb()), RejectReason::Accepted);
    EXPECT_EQ(sb.add_order(3, OrderType::Limit, 120, 10, Side::Sell, t.cb()), RejectReason::Accepted);
    EXPECT_EQ(sb.add_order(4, OrderType::Limit, 130, 10, Side::Sell, t.cb()), RejectReason::PoolExhausted);
    EXPECT_EQ(sb.cancel_order(2), RejectReason::Accepted);
    EXPECT_EQ(sb.add_order(4, OrderType::Limit, 130, 10, Side::Sell, t.cb()), RejectReason::Accepted);
    EXPECT_EQ(sb.active_order_count(), 3u);
}
