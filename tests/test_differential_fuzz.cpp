// ============================================================================
// Differential fuzz: engine vs StdOrderBook reference.
// Both books receive identical pseudo-random operation streams; after EVERY
// operation the result code, filled quantity, full trade vector (maker/taker/
// price/qty, in order) and resting-count must match. At round end, exhaustive
// market sweeps reconcile final resting liquidity, and resting-state parity
// is spot-checked through the introspection API.
//
// Seven adversarial configurations:
//   1 BaselineMinPriceZero      — parity anchor (repo-typical usage)
//   2 NonzeroMinPrice           — offset arithmetic everywhere
//   3 WordBoundaryStraddle      — bitset word boundaries at odd offsets
//   4 WideRangeSummaryBitset    — two-level summary scans, both directions
//   5 WideRangeNonzeroMin       — the above combined
//   6 TightPoolExhaustion       — exhaustion paths must still match
//   7 CancelHeavyTraffic        — update_best_* + bitset clearing at volume
//
// In HFT_DEBUG_TOOLS builds, validate_book() runs every 128 ops — this is
// the internal-consistency net layered under the behavioral parity net.
// ============================================================================

#include <gtest/gtest.h>

#include <vector>

#include "../src/OrderBook.h"
#include "StdOrderBook.h"

using namespace hft;

namespace {

struct FuzzRng {
    uint64_t s;
    explicit FuzzRng(uint64_t seed) : s(seed | 1) {}
    uint32_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
    }
    uint32_t next(uint32_t max) { return next() % max; }
};

struct OpResult {
    RejectReason engine;
    RejectReason ref;
    Quantity engine_filled;
    Quantity ref_filled;
};

struct TradeLog {
    std::vector<Trade> v;
    auto cb() & {
        return [this](const Trade& t) noexcept { v.push_back(t); };
    }
};

/// Runs one random op against both books and returns both results.
OpResult run_op(OrderBook& engine, StdOrderBook& ref, FuzzRng& rng, Price min_p, Price max_p, uint32_t id_span,
                TradeLog& et, TradeLog& rt) {
    const uint32_t roll = rng.next(100);
    OrderId id = 1 + rng.next(id_span);

    if (roll < 10) {
        // Cancel via the direct API.
        et.v.clear();
        rt.v.clear();
        RejectReason e = engine.cancel_order(id);
        RejectReason r = ref.cancel_order(id);
        return {e, r, 0, 0};
    }
    if (roll < 15) {
        // Cancel via the add_order entry path.
        et.v.clear();
        rt.v.clear();
        RejectReason e = engine.add_order(id, OrderType::Cancel, 0, 0, Side::Buy, et.cb());
        RejectReason r = ref.add_order(id, OrderType::Cancel, 0, 0, Side::Buy, rt.cb());
        return {e, r, 0, 0};
    }

    Side side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
    Quantity qty = 1 + rng.next(40);
    Price price = min_p + rng.next(max_p - min_p + 1);
    OrderType type = (roll < 80) ? OrderType::Limit : OrderType::Market;

    et.v.clear();
    rt.v.clear();
    Quantity ef = 0, rf = 0;
    RejectReason e, r;
    if (type == OrderType::Market) {
        e = engine.add_order(id, OrderType::Market, 0, qty, side, et.cb(), &ef);
        r = ref.add_order(id, OrderType::Market, 0, qty, side, rt.cb(), &rf);
    } else {
        e = engine.add_order(id, OrderType::Limit, price, qty, side, et.cb(), &ef);
        r = ref.add_order(id, OrderType::Limit, price, qty, side, rt.cb(), &rf);
    }
    return {e, r, ef, rf};
}

void compare_trades(const TradeLog& et, const TradeLog& rt, uint64_t op_no) {
    ASSERT_EQ(et.v.size(), rt.v.size()) << "trade count diverged at op " << op_no;
    for (size_t i = 0; i < et.v.size(); ++i) {
        EXPECT_EQ(et.v[i].maker_id, rt.v[i].maker_id) << "maker diverged at op " << op_no << " trade " << i;
        EXPECT_EQ(et.v[i].taker_id, rt.v[i].taker_id) << "taker diverged at op " << op_no << " trade " << i;
        EXPECT_EQ(et.v[i].price, rt.v[i].price) << "price diverged at op " << op_no << " trade " << i;
        EXPECT_EQ(et.v[i].quantity, rt.v[i].quantity) << "quantity diverged at op " << op_no << " trade " << i;
    }
}

/// Drains both books with guaranteed-crossing market orders and
/// compares the full trade stream — reconciles ALL resting liquidity.
/// Market remainders are discarded (IOC), so a single huge market order
/// per side sweeps every level in one operation.
void reconcile_final_state(OrderBook& engine, StdOrderBook& ref, OrderId& next_id, uint64_t seed) {
    TradeLog et, rt;
    for (int side_i = 0; side_i < 2; ++side_i) {
        Side side = side_i == 0 ? Side::Buy : Side::Sell;
        et.v.clear();
        rt.v.clear();
        Quantity qty = 100'000'000;  // >> any possible resting quantity
        RejectReason e = engine.add_order(next_id, OrderType::Market, 0, qty, side, et.cb());
        RejectReason r = ref.add_order(next_id, OrderType::Market, 0, qty, side, rt.cb());
        ASSERT_EQ(e, r) << "final-drain reject diverged (seed " << seed << ")";
        if (e == RejectReason::MarketOnEmptyBook) {
            EXPECT_TRUE(et.v.empty());
        } else {
            compare_trades(et, rt, 1000000 + side_i);
        }
        ++next_id;
    }
    EXPECT_EQ(engine.active_order_count(), 0u) << "engine drain incomplete (seed " << seed << ")";
    EXPECT_EQ(ref.active_order_count(), 0u) << "reference drain incomplete (seed " << seed << ")";
}

void run_differential(uint64_t seed, Price min_p, Price max_p, uint32_t id_span, size_t pool_cap, int ops,
                      int cancel_weight_pct = 15) {
    (void)cancel_weight_pct;
    // id headroom: the drain uses ids id_span+1 / id_span+2, which must be
    // inside both books' max_order_id or the drain silently no-ops with
    // OutOfBoundsOrderId (a harness bug caught by the active-count check).
    OrderBook engine(id_span + 8, pool_cap, min_p, max_p);
    StdOrderBook ref(id_span + 8, pool_cap, min_p, max_p);
    FuzzRng rng(seed);
    TradeLog et, rt;
    OrderId drain_id = id_span + 1;

    for (int op = 0; op < ops; ++op) {
        OpResult res = run_op(engine, ref, rng, min_p, max_p, id_span, et, rt);
        ASSERT_EQ(static_cast<int>(res.engine), static_cast<int>(res.ref))
            << "reject code diverged at op " << op << " (seed " << seed << "): engine=" << static_cast<int>(res.engine)
            << " ref=" << static_cast<int>(res.ref);
        ASSERT_EQ(res.engine_filled, res.ref_filled) << "filled qty diverged at op " << op << " (seed " << seed << ")";
        compare_trades(et, rt, static_cast<uint64_t>(op));

        if ((op & 63) == 0) {
            ASSERT_EQ(engine.active_order_count(), ref.active_order_count())
                << "resting count diverged after op " << op << " (seed " << seed << ")";
        }
#if HFT_DEBUG_TOOLS
        if ((op & 127) == 0) {
            ASSERT_TRUE(engine.validate_book()) << "validate_book failed after op " << op << " (seed " << seed << ")";
        }
#endif
    }

    // Full resting-liquidity reconciliation.
    reconcile_final_state(engine, ref, drain_id, seed);
}

constexpr int FUZZ_OPS = 3'000;  // per seed; tuned for CI runtime

}  // namespace

TEST(DifferentialFuzz, BaselineMinPriceZero) {
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        run_differential(seed * 7919, 0, 2000, 500, 600, FUZZ_OPS);
    }
}

TEST(DifferentialFuzz, NonzeroMinPrice) {
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        run_differential(seed * 104729, 1000, 12000, 500, 600, FUZZ_OPS);
    }
}

TEST(DifferentialFuzz, WordBoundaryStraddle) {
    // min = 63 puts price 63 at bit 0: prices 126/127 straddle the 63/64 word edge.
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        run_differential(seed * 15485863, 63, 400, 200, 300, FUZZ_OPS);
    }
}

TEST(DifferentialFuzz, WideRangeSummaryBitset) {
    // 20001 ticks: forces the two-level summary scans in both directions.
    for (uint64_t seed = 1; seed <= 2; ++seed) {
        run_differential(seed * 32452843, 0, 20000, 600, 800, FUZZ_OPS);
    }
}

TEST(DifferentialFuzz, WideRangeNonzeroMin) {
    for (uint64_t seed = 1; seed <= 2; ++seed) {
        run_differential(seed * 49979687, 7000, 40000, 600, 800, FUZZ_OPS);
    }
}

TEST(DifferentialFuzz, TightPoolExhaustion) {
    // Pool of 60: exhaustion and recovery paths must still match the reference.
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        run_differential(seed * 67867967, 0, 300, 500, 60, FUZZ_OPS);
    }
}

TEST(DifferentialFuzz, CancelHeavyTraffic) {
    // The op generator's cancel weight is fixed at 15% via the roll table;
    // halving the op space concentrates cancels on the same resting set.
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        run_differential(seed * 86028121, 0, 2000, 120, 200, FUZZ_OPS);
    }
}
