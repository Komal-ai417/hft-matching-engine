// ============================================================================
// PriceLevel unit tests (audit M-5/U-4: previously ZERO direct tests for a
// core engine component). Covers: append semantics (first/FIFO/totals/links),
// remove semantics (head/middle/tail/sole), emptiness, the debug canary,
// layout static_asserts, and a 2,000-op shadow-model property test that
// verifies totals, count, FIFO content and BOTH link directions throughout.
// ============================================================================

#include <gtest/gtest.h>

#include <deque>
#include <vector>

#include "../src/MemoryPool.h"
#include "../src/PriceLevel.h"

using namespace hft;

TEST(PriceLevelLayoutTest, SizeAndAlignmentContract) {
    static_assert(sizeof(PriceLevel) == 32);
    static_assert(alignof(PriceLevel) == 32);
    SUCCEED();
}

namespace {
// A tiny harness that mirrors OrderBook's usage: orders live in a pool,
// levels reference them by index.
struct LevelFixture {
    MemoryPool<Order> pool{2048};
    std::vector<PriceLevel> levels{8};

    uint32_t make_order(OrderId id, Quantity qty) {
        uint32_t idx = pool.allocate();
        EXPECT_NE(idx, INVALID_INDEX);  // capacity 2048 covers every test below
        Order& o = pool.data()[idx];
        o.id = id;
        o.price = 100;
        o.quantity = qty;
        o.side = Side::Sell;
        o.next = INVALID_INDEX;
        o.prev = INVALID_INDEX;
        return idx;
    }
};
}  // namespace

TEST(PriceLevelAppendTest, FirstOrderBecomesHeadAndTail) {
    LevelFixture f;
    uint32_t a = f.make_order(1, 10);
    f.levels[0].append_order(a, f.pool.data());
    EXPECT_EQ(f.levels[0].head, a);
    EXPECT_EQ(f.levels[0].tail, a);
    EXPECT_EQ(f.levels[0].order_count, 1);
    EXPECT_EQ(f.levels[0].total_quantity, 10);
    EXPECT_EQ(f.pool.data()[a].prev, INVALID_INDEX);
    EXPECT_EQ(f.pool.data()[a].next, INVALID_INDEX);
}

TEST(PriceLevelAppendTest, AppendsMaintainFifoOrderAndTotals) {
    LevelFixture f;
    uint32_t a = f.make_order(1, 10);
    uint32_t b = f.make_order(2, 20);
    uint32_t c = f.make_order(3, 30);
    f.levels[0].append_order(a, f.pool.data());
    f.levels[0].append_order(b, f.pool.data());
    f.levels[0].append_order(c, f.pool.data());
    const Order* d = f.pool.data();
    EXPECT_EQ(f.levels[0].head, a);
    EXPECT_EQ(f.levels[0].tail, c);
    EXPECT_EQ(f.levels[0].order_count, 3);
    EXPECT_EQ(f.levels[0].total_quantity, 60u);
    // Bidirectional link integrity:
    EXPECT_EQ(d[a].prev, INVALID_INDEX);
    EXPECT_EQ(d[a].next, b);
    EXPECT_EQ(d[b].prev, a);
    EXPECT_EQ(d[b].next, c);
    EXPECT_EQ(d[c].prev, b);
    EXPECT_EQ(d[c].next, INVALID_INDEX);
}

TEST(PriceLevelAppendTest, RunningTotalsAcrossAppends) {
    LevelFixture f;
    PriceLevel& lvl = f.levels[3];
    uint32_t a = f.make_order(1, 7);
    lvl.append_order(a, f.pool.data());
    EXPECT_EQ(lvl.total_quantity, 7u);
    uint32_t b = f.make_order(2, 5);
    lvl.append_order(b, f.pool.data());
    EXPECT_EQ(lvl.total_quantity, 12u);
    EXPECT_EQ(lvl.order_count, 2);
}

TEST(PriceLevelRemoveTest, RemoveHeadPreservesFifo) {
    LevelFixture f;
    uint32_t a = f.make_order(1, 10);
    uint32_t b = f.make_order(2, 20);
    uint32_t c = f.make_order(3, 30);
    for (uint32_t idx : {a, b, c}) f.levels[0].append_order(idx, f.pool.data());
    f.levels[0].remove_order(a, f.pool.data());
    EXPECT_EQ(f.levels[0].head, b);
    EXPECT_EQ(f.pool.data()[b].prev, INVALID_INDEX);  // new head's prev cleared
    EXPECT_EQ(f.levels[0].tail, c);
    EXPECT_EQ(f.levels[0].order_count, 2);
    EXPECT_EQ(f.levels[0].total_quantity, 50u);
}

TEST(PriceLevelRemoveTest, RemoveMiddleSplicesBothDirections) {
    LevelFixture f;
    uint32_t a = f.make_order(1, 10);
    uint32_t b = f.make_order(2, 20);
    uint32_t c = f.make_order(3, 30);
    for (uint32_t idx : {a, b, c}) f.levels[0].append_order(idx, f.pool.data());
    f.levels[0].remove_order(b, f.pool.data());
    const Order* d = f.pool.data();
    EXPECT_EQ(d[a].next, c);
    EXPECT_EQ(d[c].prev, a);
    EXPECT_EQ(f.levels[0].order_count, 2);
    EXPECT_EQ(f.levels[0].total_quantity, 40u);
    // Victim's links are cleared:
    EXPECT_EQ(d[b].prev, INVALID_INDEX);
    EXPECT_EQ(d[b].next, INVALID_INDEX);
}

TEST(PriceLevelRemoveTest, RemoveTail) {
    LevelFixture f;
    uint32_t a = f.make_order(1, 10);
    uint32_t b = f.make_order(2, 20);
    f.levels[0].append_order(a, f.pool.data());
    f.levels[0].append_order(b, f.pool.data());
    f.levels[0].remove_order(b, f.pool.data());
    EXPECT_EQ(f.levels[0].tail, a);
    EXPECT_EQ(f.pool.data()[a].next, INVALID_INDEX);
    EXPECT_EQ(f.levels[0].order_count, 1);
    EXPECT_EQ(f.levels[0].total_quantity, 10u);
}

TEST(PriceLevelRemoveTest, RemoveSoleOrderEmptiesLevel) {
    LevelFixture f;
    uint32_t a = f.make_order(1, 10);
    f.levels[0].append_order(a, f.pool.data());
    f.levels[0].remove_order(a, f.pool.data());
    EXPECT_TRUE(f.levels[0].is_empty());
    EXPECT_EQ(f.levels[0].head, INVALID_INDEX);
    EXPECT_EQ(f.levels[0].tail, INVALID_INDEX);
    EXPECT_EQ(f.levels[0].order_count, 0);
    EXPECT_EQ(f.levels[0].total_quantity, 0u);
}

TEST(PriceLevelEmptinessTest, FreshLevelIsEmpty) {
    PriceLevel lvl;
    EXPECT_TRUE(lvl.is_empty());
}

TEST(PriceLevelCanaryTest, CanaryWrittenAndIntact) {
    LevelFixture f;
    EXPECT_TRUE(f.levels[0].canary_intact());
    uint32_t a = f.make_order(1, 10);
    f.levels[0].append_order(a, f.pool.data());
    f.levels[0].remove_order(a, f.pool.data());
    EXPECT_TRUE(f.levels[0].canary_intact());  // operations never clobber padding
}

// ── Shadow-model property test (2,000 ops) ───────────────────────────────
TEST(PriceLevelPropertyTest, InterleavedAddRemoveMatchesShadowQueue) {
    struct FastRng {
        uint64_t s = 0x853C49E6748FEA9BULL;
        uint32_t next() {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
        }
    };
    FastRng rng;
    LevelFixture f;
    PriceLevel& lvl = f.levels[0];
    std::deque<std::pair<uint32_t, Quantity>> shadow;  // {pool_idx, qty}
    OrderId next_id = 1;

    auto verify = [&]() {
        ASSERT_EQ(lvl.order_count, shadow.size());
        uint64_t qty = 0;
        for (auto& [idx, q] : shadow) qty += q;
        ASSERT_EQ(lvl.total_quantity, qty);
        // Walk forward from head: must reproduce the shadow FIFO exactly.
        uint32_t cur = lvl.head;
        uint32_t prv = INVALID_INDEX;
        for (size_t i = 0; i < shadow.size(); ++i) {
            ASSERT_NE(cur, INVALID_INDEX);
            const Order& o = f.pool.data()[cur];
            ASSERT_EQ(o.prev, prv);
            ASSERT_EQ(o.quantity, shadow[i].second);
            prv = cur;
            cur = o.next;
        }
        ASSERT_EQ(cur, INVALID_INDEX);
        ASSERT_EQ(lvl.tail, prv);
        // Walk backward from tail:
        cur = lvl.tail;
        uint32_t nxt = INVALID_INDEX;
        for (size_t i = shadow.size(); i-- > 0;) {
            ASSERT_NE(cur, INVALID_INDEX);
            const Order& o = f.pool.data()[cur];
            ASSERT_EQ(o.next, nxt);
            ASSERT_EQ(o.quantity, shadow[i].second);
            nxt = cur;
            cur = o.prev;
        }
        ASSERT_EQ(cur, INVALID_INDEX);
        ASSERT_EQ(lvl.head, nxt);
    };

    for (int op = 0; op < 2'000; ++op) {
        if (shadow.empty() || (rng.next() % 100) < 60) {
            Quantity q = 1 + rng.next() % 50;
            uint32_t idx = f.make_order(next_id++, q);
            lvl.append_order(idx, f.pool.data());
            shadow.emplace_back(idx, q);
        } else {
            // Remove a random position (head / tail / middle all get hit over 2k ops)
            size_t pos = rng.next() % shadow.size();
            lvl.remove_order(shadow[pos].first, f.pool.data());
            shadow.erase(shadow.begin() + static_cast<long>(pos));
        }
        if ((op % 64) == 0) verify();
    }
    verify();  // final full reconciliation
    ASSERT_TRUE(lvl.is_empty() || lvl.head != INVALID_INDEX);
}
