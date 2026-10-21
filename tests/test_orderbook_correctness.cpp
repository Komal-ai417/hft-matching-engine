// ============================================================================
// OrderBook correctness gap tests. Every family here was ZERO-coverage in
// the original suite:
//   * nonzero min_price books (all 30 original tests used min_price = 0)
//   * cancel-the-best-level -> next-best becomes matchable (both sides, gaps)
//   * queue-position cancels (head/middle/tail of multi-order levels)
//   * multi-maker partial fills
//   * wide price ranges (> 4096 ticks) exercising the two-level summary scans
//   * word-boundary straddles (prices 63/64/65 and nonzero-min variants)
//   * pool-exhaustion full cycle + the taker invariant
//   * market-order semantics: filled_out surface, IOC remainder
//   * 5,000-op deterministic mixed-sequence reconciliation
// ============================================================================

#include <gtest/gtest.h>

#include <vector>

#include "../src/OrderBook.h"

using namespace hft;

namespace {
struct Trades {
    std::vector<Trade> v;
    void clear() { v.clear(); }
    size_t size() const { return v.size(); }
    const Trade& operator[](size_t i) const { return v[i]; }
    auto cb() & {
        return [this](const Trade& t) noexcept { v.push_back(t); };
    }
};
}  // namespace

// ── Nonzero min_price (the offset arithmetic: bit_idx = price - min_price_) ──

TEST(OrderBookNonzeroMin, MatchAndSweep) {
    Trades got;
    OrderBook ob(100, 100, 5000, 15000);  // shifted range
    ASSERT_EQ(ob.add_order(1, OrderType::Limit, 5100, 10, Side::Sell, got.cb()), RejectReason::Accepted);
    ASSERT_EQ(ob.add_order(2, OrderType::Limit, 6000, 5, Side::Sell, got.cb()), RejectReason::Accepted);
    ASSERT_EQ(ob.add_order(3, OrderType::Limit, 5200, 7, Side::Sell, got.cb()), RejectReason::Accepted);

    got.clear();
    (void)ob.add_order(4, OrderType::Limit, 5200, 3, Side::Buy, got.cb());  // crosses 5100 only
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].maker_id, 1);
    EXPECT_EQ(got[0].price, 5100);
    EXPECT_EQ(got[0].quantity, 3);

    got.clear();
    (void)ob.add_order(5, OrderType::Limit, 6500, 50, Side::Buy,
                       got.cb());  // sweeps 5100(7), 5200(7), 6000(5); rests 31@6500
    ASSERT_EQ(got.size(), 3);      // bitset must skip the 5300..5999 hole between levels
    EXPECT_EQ(got[0].price, 5100);
    EXPECT_EQ(got[1].price, 5200);
    EXPECT_EQ(got[2].price, 6000);
    EXPECT_EQ(ob.level_total_quantity(Side::Buy, 6500), 31);
}

TEST(OrderBookNonzeroMin, BoundsEnforcedAgainstShiftedRange) {
    Trades got;
    OrderBook ob(100, 100, 5000, 15000);
    EXPECT_EQ(ob.add_order(6, OrderType::Limit, 4999, 1, Side::Buy, got.cb()), RejectReason::OutOfBoundsPrice);
    EXPECT_EQ(ob.add_order(7, OrderType::Limit, 15001, 1, Side::Buy, got.cb()), RejectReason::OutOfBoundsPrice);
    EXPECT_EQ(ob.add_order(8, OrderType::Limit, 5000, 1, Side::Buy, got.cb()), RejectReason::Accepted);
    EXPECT_EQ(ob.add_order(9, OrderType::Limit, 15000, 1, Side::Sell, got.cb()), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 15000);  // resting sells: 15000
    EXPECT_EQ(ob.best_bid(), 5000);
}

TEST(OrderBookNonzeroMin, CancelThenReAdd) {
    Trades got;
    OrderBook ob(100, 100, 7000, 9000);
    (void)ob.add_order(1, OrderType::Limit, 8000, 10, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 8100, 10, Side::Sell, got.cb());
    EXPECT_EQ(ob.best_ask(), 8000);
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 8100);
    EXPECT_EQ(ob.add_order(3, OrderType::Limit, 7900, 4, Side::Sell, got.cb()), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 7900);
}

TEST(OrderBookNonzeroMin, MarketOrderSweeps) {
    Trades got;
    OrderBook ob(100, 100, 5000, 15000);
    (void)ob.add_order(1, OrderType::Limit, 5100, 10, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 5200, 10, Side::Sell, got.cb());
    Quantity filled = 0;
    got.clear();
    EXPECT_EQ(ob.add_order(3, OrderType::Market, 0, 20, Side::Buy, got.cb(), &filled), RejectReason::Accepted);
    ASSERT_EQ(got.size(), 2);
    EXPECT_EQ(got[0].price, 5100);
    EXPECT_EQ(got[1].price, 5200);
    EXPECT_EQ(filled, 20);
    EXPECT_EQ(ob.active_order_count(), 0);
    // Book now empty on asks -> next market buy must be rejected:
    EXPECT_EQ(ob.add_order(4, OrderType::Market, 0, 5, Side::Buy, got.cb()), RejectReason::MarketOnEmptyBook);
}

// ── Cancel-the-best fast-forward (update_best_*_after_remove) ────────────

TEST(OrderBookCancelBest, BestAskEmptiedFallsToNextAskAcrossGap) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 5, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 130, 5, Side::Sell, got.cb());  // gap 101..129
    (void)ob.add_order(3, OrderType::Limit, 200, 5, Side::Sell, got.cb());
    EXPECT_EQ(ob.best_ask(), 100);
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 130);  // skipped the empty levels via bitset
    // The next-best must actually be matchable:
    got.clear();
    (void)ob.add_order(4, OrderType::Limit, 130, 5, Side::Buy, got.cb());
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].maker_id, 2);
    EXPECT_EQ(got[0].price, 130);
}

TEST(OrderBookCancelBest, BestBidEmptiedFallsToNextBidAcrossGap) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 5, Side::Buy, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 90, 5, Side::Buy, got.cb());
    (void)ob.add_order(3, OrderType::Limit, 80, 5, Side::Buy, got.cb());
    (void)ob.add_order(4, OrderType::Limit, 50, 3, Side::Sell, got.cb());  // partial: order 1 -> 2 left
    got.clear();
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);                 // empties the 100 level
    (void)ob.add_order(5, OrderType::Limit, 50, 4, Side::Sell, got.cb());  // must match new best bid 90
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].maker_id, 2);
    EXPECT_EQ(got[0].price, 90);  // proves the downward scan worked
}

TEST(OrderBookCancelBest, CancelDrainsEntireSide) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 5, Side::Buy, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 90, 5, Side::Buy, got.cb());
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(ob.cancel_order(2), RejectReason::Accepted);
    EXPECT_FALSE(ob.has_bids());
    // A sell that would have crossed both must now rest instead:
    (void)ob.add_order(3, OrderType::Limit, 80, 5, Side::Sell, got.cb());
    EXPECT_EQ(got.size(), 0);
    EXPECT_TRUE(ob.has_asks());
    EXPECT_EQ(ob.best_ask(), 80);
}

// ── Queue-position cancels ─────────────────────────────────────────────────

TEST(OrderBookQueuePosition, CancelHeadMiddleTailPreservesFifo) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    for (OrderId id = 1; id <= 5; ++id) {
        (void)ob.add_order(id, OrderType::Limit, 100, 10, Side::Sell, got.cb());
    }
    // Cancel middle (3), head (1), tail (5) — remaining FIFO must be 2,4
    EXPECT_EQ(ob.cancel_order(3), RejectReason::Accepted);
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(ob.cancel_order(5), RejectReason::Accepted);
    EXPECT_EQ(ob.level_order_count(Side::Sell, 100), 2);
    EXPECT_EQ(ob.level_total_quantity(Side::Sell, 100), 20u);

    got.clear();
    (void)ob.add_order(10, OrderType::Limit, 100, 25, Side::Buy, got.cb());
    ASSERT_EQ(got.size(), 2);
    EXPECT_EQ(got[0].maker_id, 2);  // FIFO preserved through all three unlinks
    EXPECT_EQ(got[1].maker_id, 4);
    // order 10 partially filled (20 of 25) -> rests as the only bid
    EXPECT_EQ(ob.level_total_quantity(Side::Buy, 100), 5);
}

// ── Multi-maker partial fills ──────────────────────────────────────────────

TEST(OrderBookPartialFills, FiveMakerQueuePartiallyConsumed) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    for (OrderId id = 1; id <= 5; ++id) {
        (void)ob.add_order(id, OrderType::Limit, 100, 10, Side::Sell, got.cb());
    }
    got.clear();
    Quantity filled = 0;
    EXPECT_EQ(ob.add_order(6, OrderType::Limit, 100, 35, Side::Buy, got.cb(), &filled), RejectReason::Accepted);
    ASSERT_EQ(got.size(), 4);  // 3 full makers + 1 partial
    EXPECT_EQ(got[0].maker_id, 1);
    EXPECT_EQ(got[0].quantity, 10);
    EXPECT_EQ(got[1].maker_id, 2);
    EXPECT_EQ(got[1].quantity, 10);
    EXPECT_EQ(got[2].maker_id, 3);
    EXPECT_EQ(got[2].quantity, 10);
    EXPECT_EQ(got[3].maker_id, 4);
    EXPECT_EQ(got[3].quantity, 5);
    EXPECT_EQ(filled, 35);
    // Maker 4 retains 5; maker 5 untouched:
    EXPECT_EQ(ob.level_total_quantity(Side::Sell, 100), 15u);
    EXPECT_EQ(ob.level_order_count(Side::Sell, 100), 2);
    EXPECT_TRUE(ob.order_exists(4));
    EXPECT_TRUE(ob.order_exists(5));
    // Taker fully filled -> rests nothing:
    EXPECT_EQ(ob.active_order_count(), 2);
}

TEST(OrderBookPartialFills, PartialFillThenCancelRemainingThenReuseId) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 100, 5, Side::Buy, got.cb());  // crosses: order 1 -> 5 left
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].quantity, 5);
    got.clear();
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    (void)ob.add_order(1, OrderType::Limit, 150, 3, Side::Sell, got.cb());  // same ID, new price/qty
    got.clear();
    (void)ob.add_order(3, OrderType::Limit, 150, 2, Side::Buy, got.cb());
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].maker_id, 1);
    EXPECT_EQ(got[0].price, 150);  // matched the REUSED order
}

// ── Wide ranges: two-level summary bitset scans (> 4096 ticks) ────────────

TEST(OrderBookWideRange, ExtremesViaSummaryScan) {
    Trades got;
    OrderBook ob(1000, 1000, 0, 20000);  // 20001 ticks -> 314 words -> 5 summary words
    (void)ob.add_order(1, OrderType::Limit, 100, 5, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 19800, 5, Side::Sell, got.cb());  // word 309 — summary word 4
    EXPECT_EQ(ob.best_ask(), 100);
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 19800);  // summary-tier fast-forward (word 1 -> word 309)
    EXPECT_EQ(ob.cancel_order(2), RejectReason::Accepted);
    EXPECT_FALSE(ob.has_asks());
    // Bids, downward:
    (void)ob.add_order(3, OrderType::Limit, 500, 5, Side::Buy, got.cb());
    (void)ob.add_order(4, OrderType::Limit, 19500, 5, Side::Buy, got.cb());
    EXPECT_EQ(ob.best_bid(), 19500);
    EXPECT_EQ(ob.cancel_order(4), RejectReason::Accepted);
    EXPECT_EQ(ob.best_bid(), 500);
}

TEST(OrderBookWideRange, NonzeroMinWideRange) {
    Trades got;
    OrderBook ob(1000, 1000, 7000, 40000);  // 33001 ticks, offset 7000
    (void)ob.add_order(1, OrderType::Limit, 7100, 5, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 39900, 5, Side::Sell, got.cb());
    EXPECT_EQ(ob.best_ask(), 7100);
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 39900);
    got.clear();
    (void)ob.add_order(3, OrderType::Limit, 40000, 10, Side::Buy, got.cb());
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].price, 39900);
}

TEST(OrderBookWideRange, MultiLevelSweepAcrossWords) {
    Trades got;
    OrderBook ob(1000, 1000, 0, 20000);
    // Sells at word boundaries: 4095, 4096, 4097 (straddles words 63/64) and far 12000
    (void)ob.add_order(1, OrderType::Limit, 4095, 10, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 4096, 10, Side::Sell, got.cb());
    (void)ob.add_order(3, OrderType::Limit, 4097, 10, Side::Sell, got.cb());
    (void)ob.add_order(4, OrderType::Limit, 12000, 10, Side::Sell, got.cb());
    got.clear();
    (void)ob.add_order(5, OrderType::Limit, 12000, 40, Side::Buy, got.cb());
    ASSERT_EQ(got.size(), 4);
    EXPECT_EQ(got[0].price, 4095);
    EXPECT_EQ(got[1].price, 4096);
    EXPECT_EQ(got[2].price, 4097);
    EXPECT_EQ(got[3].price, 12000);
    EXPECT_EQ(ob.active_order_count(), 0);
}

// ── Word-boundary straddles with nonzero min ──────────────────────────────

TEST(OrderBookWordBoundary, StraddleAtSixtyThreeSixtyFive) {
    Trades got;
    OrderBook ob(200, 200, 0, 500);
    for (Price p : {63u, 64u, 65u}) {
        (void)ob.add_order(p, OrderType::Limit, p, 5, Side::Sell, got.cb());
    }
    EXPECT_EQ(ob.best_ask(), 63);
    EXPECT_EQ(ob.cancel_order(63), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 64);
    EXPECT_EQ(ob.cancel_order(64), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 65);  // crossing the 63/64 word boundary
    EXPECT_EQ(ob.cancel_order(65), RejectReason::Accepted);
    EXPECT_FALSE(ob.has_asks());
}

TEST(OrderBookWordBoundary, NonzeroMinOffsetStraddle) {
    Trades got;
    OrderBook ob(200, 200, 60, 500);  // bit 0 == price 60: prices 123/124 straddle words
    (void)ob.add_order(1, OrderType::Limit, 123, 5, Side::Sell, got.cb());  // bit 63
    (void)ob.add_order(2, OrderType::Limit, 124, 5, Side::Sell, got.cb());  // bit 64
    EXPECT_EQ(ob.best_ask(), 123);
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(ob.best_ask(), 124);
    got.clear();
    (void)ob.add_order(3, OrderType::Limit, 124, 5, Side::Buy, got.cb());
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].price, 124);
}

// ── Sweep empties the book, then it recovers ──────────────────────────────

TEST(OrderBookRecovery, SweepEmptiesEntireBookThenRecovers) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 5, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 200, 5, Side::Sell, got.cb());
    (void)ob.add_order(3, OrderType::Limit, 300, 5, Side::Sell, got.cb());
    got.clear();
    (void)ob.add_order(4, OrderType::Market, 0, 100, Side::Buy, got.cb());  // drain all three levels
    ASSERT_EQ(got.size(), 3);
    EXPECT_FALSE(ob.has_asks());
    got.clear();
    (void)ob.add_order(5, OrderType::Limit, 400, 5, Side::Sell, got.cb());
    (void)ob.add_order(6, OrderType::Limit, 450, 5, Side::Buy, got.cb());  // must reinit best-ask from scratch
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].price, 400);
    EXPECT_EQ(got[0].maker_id, 5);
}

// ── Taker invariant: a taker that trades can never return PoolExhausted ────

TEST(OrderBookPoolInvariant, SweepFreesPoolCapacityForTakerRemainder) {
    Trades got;
    // Pool of exactly 5 slots; 5 resting asks; taker sweeps all 5 and rests.
    OrderBook ob(100, 5, 0, 2000);
    for (OrderId id = 1; id <= 5; ++id) {
        EXPECT_EQ(ob.add_order(id, OrderType::Limit, 100 + id, 10, Side::Sell, got.cb()), RejectReason::Accepted);
    }
    // Pool full — a NON-CROSSING order must be rejected (a crossing taker
    // would free maker slots; see the next assertion):
    EXPECT_EQ(ob.add_order(9, OrderType::Limit, 1500, 10, Side::Sell, got.cb()), RejectReason::PoolExhausted);
    // But a crossing taker that frees all 5 maker slots CAN rest its remainder:
    got.clear();
    Quantity filled = 0;
    EXPECT_EQ(ob.add_order(10, OrderType::Limit, 105, 55, Side::Buy, got.cb(), &filled), RejectReason::Accepted);
    ASSERT_EQ(got.size(), 5);
    EXPECT_EQ(filled, 50);
    EXPECT_EQ(ob.active_order_count(), 1);  // taker's 5-unit remainder rests
    EXPECT_EQ(ob.level_total_quantity(Side::Buy, 105), 5);
}

TEST(OrderBookPoolInvariant, PoolFullNonCrossingRejectedNoSideEffects) {
    Trades got;
    OrderBook ob(100, 2, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, got.cb());
    (void)ob.add_order(2, OrderType::Limit, 110, 10, Side::Sell, got.cb());
    RejectReason r = ob.add_order(3, OrderType::Limit, 120, 10, Side::Sell, got.cb());
    EXPECT_EQ(r, RejectReason::PoolExhausted);
    EXPECT_FALSE(ob.order_exists(3));  // rejection must not leave residue
    EXPECT_EQ(ob.active_order_count(), 2);
}

// ── Taker remainder rests at the taker's limit price ──────────────────────

TEST(OrderBookResting, TakerRemainderRestsAtOwnPriceNotMakerPrice) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, got.cb());
    got.clear();
    (void)ob.add_order(2, OrderType::Limit, 150, 30, Side::Buy, got.cb());  // fills 10 @100, rests 20 @150
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(ob.level_total_quantity(Side::Buy, 150), 20);  // NOT at 100
    EXPECT_EQ(ob.best_bid(), 150);
    // A later sell at 150 must cross the resting remainder at 150:
    got.clear();
    (void)ob.add_order(3, OrderType::Limit, 140, 20, Side::Sell, got.cb());
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].price, 150);
    EXPECT_EQ(got[0].quantity, 20);
}

// ── Validation-edge combinations the original suite missed ────────────────

TEST(OrderBookValidation, DuplicateAcrossSidesAndEntryPaths) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Buy, got.cb());  // resting BID
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 200, 10, Side::Sell, got.cb()),
              RejectReason::DuplicateOrderId);  // same id, other side
    EXPECT_EQ(ob.add_order(1, OrderType::Market, 0, 5, Side::Sell, got.cb()),
              RejectReason::DuplicateOrderId);  // same id, market
    EXPECT_EQ(ob.add_order(1, OrderType::Cancel, 0, 0, Side::Buy, got.cb()),
              RejectReason::Accepted);  // cancel via add_order path
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 200, 10, Side::Sell, got.cb()),
              RejectReason::Accepted);  // now reusable
}

TEST(OrderBookValidation, CancelPathIdBounds) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    EXPECT_EQ(ob.cancel_order(0), RejectReason::OutOfBoundsOrderId);
    EXPECT_EQ(ob.cancel_order(101), RejectReason::OutOfBoundsOrderId);
    EXPECT_EQ(ob.cancel_order(1), RejectReason::CancelFailed);  // in range, not resting
}

TEST(OrderBookValidation, DoubleCancelIsFailedCancel) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    (void)ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, got.cb());
    EXPECT_EQ(ob.cancel_order(1), RejectReason::Accepted);
    EXPECT_EQ(ob.cancel_order(1), RejectReason::CancelFailed);
}

TEST(OrderBookValidation, IdReuseAfterRejection) {
    Trades got;
    OrderBook ob(100, 100, 0, 2000);
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 9999, 10, Side::Sell, got.cb()), RejectReason::OutOfBoundsPrice);
    EXPECT_EQ(ob.add_order(1, OrderType::Limit, 100, 10, Side::Sell, got.cb()),
              RejectReason::Accepted);  // id still free
}

// ── 5,000-op deterministic mixed-sequence reconciliation ──────────────────

TEST(OrderBookLongSequence, FiveThousandOpsEndConsistent) {
    struct FastRng {
        uint64_t s = 0xDEADBEEFCAFEF00DULL;
        uint32_t next() {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
        }
        uint32_t next(uint32_t max) { return max ? next() % max : 0; }
    };
    FastRng rng;
    Trades got;
    OrderBook ob(6000, 6000, 1000, 3000);  // nonzero min + multi-word bitset
    OrderId next_id = 1;

    for (int op = 0; op < 5'000; ++op) {
        uint32_t roll = rng.next(100);
        if (roll < 45) {  // passive limit
            Side side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            Price p = 1000 + rng.next(2000);
            Quantity q = 1 + rng.next(20);
            (void)ob.add_order(next_id++, OrderType::Limit, p, q, side, got.cb());
        } else if (roll < 60) {  // aggressive limit (crosses or rests)
            Side side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            Price p = side == Side::Buy ? 2100 + rng.next(900) : 1000 + rng.next(900);
            Quantity q = 1 + rng.next(60);
            (void)ob.add_order(next_id++, OrderType::Limit, p, q, side, got.cb());
        } else if (roll < 75) {  // market
            Side side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            Quantity q = 1 + rng.next(100);
            (void)ob.add_order(next_id++, OrderType::Market, 0, q, side, got.cb());
        } else {  // cancel a recent order
            OrderId id = 1 + (rng.next(4000));
            (void)ob.cancel_order(id);
        }
        if (op % 500 == 499) {
#if HFT_DEBUG_TOOLS
            ASSERT_TRUE(ob.validate_book()) << "validate_book failed at op " << op;
#endif
        }
    }
#if HFT_DEBUG_TOOLS
    EXPECT_TRUE(ob.validate_book());
#endif
    // Cross-checks independent of flavor:
    EXPECT_TRUE(ob.active_order_count() <= 6000);
    EXPECT_EQ(ob.price_level_count(Side::Buy) + ob.price_level_count(Side::Sell) > 0, true);
}
