// ============================================================================
// MemoryPool unit tests. Covers: allocation order, exhaustion, LIFO
// recycling, double-free / out-of-range / chain faults (exception-free, via
// the fault handler), poison canaries, generation stamps, allocation
// statistics, free-list depth reconciliation, template genericity and a
// 10k-op shadow-model property test.
// ============================================================================

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "../src/MemoryPool.h"

using namespace hft;

namespace {
// Captures pool fault messages; chooses SkipOperation so the faulting call
// returns instead of aborting (see MemoryPool.h: exception-free faults).
// GoogleTest runs suites sequentially on one thread, so statics are safe.
struct FaultRecorder {
    static inline int count_ = 0;
    static inline std::string last_message_{};
    static inline size_t last_slot_ = 0;

    FaultRecorder() {
        reset();
        set_pool_fault_handler(&handler);
    }
    ~FaultRecorder() { set_pool_fault_handler(nullptr); }
    void reset() {
        count_ = 0;
        last_message_.clear();
        last_slot_ = 0;
    }
    static FaultAction handler(const char* msg, size_t slot) noexcept {
        count_++;
        last_message_ = msg;
        last_slot_ = slot;
        return FaultAction::SkipOperation;
    }
};
}  // namespace

// ── Layout / template constraints (compile-time) ──────────────
TEST(MemoryPoolLayoutTest, OrderContractEnforced) {
    static_assert(sizeof(Order) == 24);
    static_assert(std::is_trivially_copyable_v<Order>);
    SUCCEED();  // static_asserts above are the actual assertions
}

TEST(MemoryPoolLayoutTest, GenericTrivialPODInstantiable) {
    // The debug-mode deallocate_chain walks the overlay link, not T::next, so
    // any T without a `next` member stays instantiable. A plain POD must be
    // instantiable AND chain-freeable in every instrumented flavor.
    struct Blob {
        uint32_t link;
        uint64_t payload;
    };
    static_assert(sizeof(Blob) >= 4);
    MemoryPool<Blob> pool(8);
    uint32_t a = pool.allocate();
    uint32_t b = pool.allocate();
    uint32_t c = pool.allocate();
    ASSERT_NE(a, INVALID_INDEX);
    ASSERT_NE(b, INVALID_INDEX);
    ASSERT_NE(c, INVALID_INDEX);  // Overlay the chain manually through the documented link region:
    pool.data()[a].payload = 111;
    pool.data()[b].payload = 222;
    pool.data()[c].payload = 333;
    // Build a chain a -> b -> c through the first-4-byte overlay semantics
    // (this is what PriceLevel queues do with Order::next):
    std::memcpy(&pool.data()[a].link, &b, sizeof(b));
    uint32_t c_copy = c;
    std::memcpy(&pool.data()[b].link, &c_copy, sizeof(c_copy));
    uint32_t end = INVALID_INDEX;
    std::memcpy(&pool.data()[c].link, &end, sizeof(end));
    pool.deallocate_chain(a, c);
#if HFT_DEBUG_TOOLS
    EXPECT_EQ(pool.stats().live_count, 0);
    EXPECT_EQ(pool.stats().free_list_depth, 8);
#endif
}

// ── allocate() ────────────────────────────────────────────────────────────
TEST(MemoryPoolAllocateTest, FreshPoolAllocatesAscendingUniqueIndices) {
    MemoryPool<Order> pool(100);
    std::set<uint32_t> seen;
    uint32_t prev = INVALID_INDEX;
    for (int i = 0; i < 100; ++i) {
        uint32_t idx = pool.allocate();
        ASSERT_NE(idx, INVALID_INDEX);
        EXPECT_TRUE(seen.insert(idx).second);  // unique
        if (prev != INVALID_INDEX) {
            EXPECT_EQ(idx, prev + 1);  // ascending (constructor threads the list in order)
        }
        prev = idx;
    }
    EXPECT_EQ(pool.allocate(), INVALID_INDEX);  // exhausted, no throw
}

TEST(MemoryPoolAllocateTest, ExhaustionReturnsInvalidNoThrow) {
    MemoryPool<Order> pool(4);
    for (int i = 0; i < 4; ++i) EXPECT_NE(pool.allocate(), INVALID_INDEX);
    EXPECT_EQ(pool.allocate(), INVALID_INDEX);
    EXPECT_EQ(pool.allocate(), INVALID_INDEX);  // repeatedly
}

TEST(MemoryPoolAllocateTest, LIFORecyclingAfterFree) {
    MemoryPool<Order> pool(4);
    (void)pool.allocate();         // 0
    uint32_t b = pool.allocate();  // 1
    (void)pool.allocate();         // 2
    (void)pool.allocate();         // 3
    pool.deallocate(b);
    EXPECT_EQ(pool.allocate(), b);  // most-recently-freed first (LIFO)
}

TEST(MemoryPoolAllocateTest, CapacityClampDocumented) {
    // capacity is size_t but indices are uint32_t; the pool clamps to
    // 0xFFFF'FFFE in its constructor instead of wrapping silently.
    // (We cannot instantiate a >2^32-slot pool to observe the clamp directly;
    // the clamp line is verified by review + the small-pool edge below.)
    static_assert(INVALID_INDEX == 0xFFFF'FFFFu);
    MemoryPool<Order> pool(1);
    EXPECT_NE(pool.allocate(), INVALID_INDEX);
    EXPECT_EQ(pool.allocate(), INVALID_INDEX);  // tiny-pool exhaustion edge
    EXPECT_EQ(pool.capacity(), 1u);
}

// ── deallocate() ──────────────────────────────────────────────────────────
#if HFT_DEBUG_TOOLS
TEST(MemoryPoolDeallocateTest, DoubleFreeFaultsAndSkips) {
    FaultRecorder rec;
    MemoryPool<Order> pool(10);
    uint32_t o = pool.allocate();
    pool.deallocate(o);
    EXPECT_EQ(FaultRecorder::count_, 0);
    pool.deallocate(o);
    EXPECT_EQ(FaultRecorder::count_, 1);
    EXPECT_NE(FaultRecorder::last_message_.find("double free"), std::string::npos);
    // Pool still consistent:
    EXPECT_EQ(pool.stats().live_count, 0);
    EXPECT_EQ(pool.stats().free_list_depth, 10);
}

TEST(MemoryPoolDeallocateTest, OutOfRangeFaultsAndSkips) {
    FaultRecorder rec;
    MemoryPool<Order> pool(10);
    pool.deallocate(10'000);
    EXPECT_EQ(FaultRecorder::count_, 1);
    EXPECT_NE(FaultRecorder::last_message_.find("does not belong to this pool"), std::string::npos)
        << "message: " << FaultRecorder::last_message_;
    EXPECT_EQ(pool.stats().live_count, 0);
    EXPECT_EQ(pool.stats().free_list_depth, 10);
}

TEST(MemoryPoolDeallocateTest, WildFreeOfNeverAllocatedSlot) {
    FaultRecorder rec;
    MemoryPool<Order> pool(10);
    pool.deallocate(5);  // in-range but never allocated
    EXPECT_EQ(FaultRecorder::count_, 1);
    EXPECT_NE(FaultRecorder::last_message_.find("freeing unallocated"), std::string::npos);
}
#else
TEST(MemoryPoolDeallocateTest, ReleaseDoubleFreeIsDocumentedTrust) {
    // Release contract: guards are compiled out; the fault handler has
    // nothing to intercept. This test pins that the release shapes compile
    // and stay on the fast path.
    MemoryPool<Order> pool(4);
    uint32_t a = pool.allocate();
    pool.deallocate(a);
    SUCCEED();
}
#endif

// ── deallocate_chain() ──────────────────────────────────────────────
#if HFT_DEBUG_TOOLS
TEST(MemoryPoolChainTest, FreesWholeChainInLinkPath) {
    MemoryPool<Order> pool(8);
    // Build a 3-order queue as PriceLevel would: indices a -> b -> c via Order::next
    uint32_t a = pool.allocate();
    uint32_t b = pool.allocate();
    uint32_t c = pool.allocate();
    Order* d = pool.data();
    d[a].next = b;
    d[a].prev = INVALID_INDEX;
    d[b].next = c;
    d[b].prev = a;
    d[c].next = INVALID_INDEX;
    d[c].prev = b;
    pool.deallocate_chain(a, c);
    EXPECT_FALSE(pool.is_allocated(a));
    EXPECT_FALSE(pool.is_allocated(b));
    EXPECT_FALSE(pool.is_allocated(c));
    EXPECT_EQ(pool.stats().live_count, 0);
    EXPECT_EQ(pool.stats().free_list_depth, 8);
}

TEST(MemoryPoolChainTest, SingleElementChain) {
    MemoryPool<Order> pool(4);
    uint32_t a = pool.allocate();
    pool.data()[a].next = INVALID_INDEX;
    pool.deallocate_chain(a, a);
    EXPECT_FALSE(pool.is_allocated(a));
    EXPECT_EQ(pool.stats().free_list_depth, 4);
}

// An out-of-range tail must fault cleanly instead of splicing a write past
// the slab: `deallocate_chain(0, 500)` on a capacity-2 pool.
TEST(MemoryPoolChainTest, ChainWithInvalidTailNoLongerOverflows) {
    FaultRecorder rec;
    MemoryPool<Order> pool(2);
    uint32_t a = pool.allocate();
    pool.data()[a].next = INVALID_INDEX;
    pool.deallocate_chain(a, 500);  // tail out of range — MUST be caught
    EXPECT_EQ(FaultRecorder::count_, 1);
    EXPECT_NE(FaultRecorder::last_message_.find("out of range"), std::string::npos)
        << "message: " << FaultRecorder::last_message_;
    // a is still allocated (operation skipped, no corruption):
    EXPECT_TRUE(pool.is_allocated(a));
}

TEST(MemoryPoolChainTest, ChainWithInvalidHeadCaught) {
    FaultRecorder rec;
    MemoryPool<Order> pool(4);
    uint32_t a = pool.allocate();
    pool.data()[a].next = INVALID_INDEX;
    pool.deallocate_chain(777, a);  // head out of range
    EXPECT_EQ(FaultRecorder::count_, 1);
    EXPECT_TRUE(pool.is_allocated(a));  // untouched
}

TEST(MemoryPoolChainTest, DoubleFreeWithinChainCaught) {
    FaultRecorder rec;
    MemoryPool<Order> pool(4);
    uint32_t a = pool.allocate();
    uint32_t b = pool.allocate();
    pool.data()[a].next = b;
    pool.data()[b].next = INVALID_INDEX;
    pool.deallocate(a);  // a already freed — chain contains a stale slot
    pool.deallocate_chain(a, b);
    EXPECT_GE(FaultRecorder::count_, 1);
    EXPECT_NE(FaultRecorder::last_message_.find("not live"), std::string::npos)
        << "message: " << FaultRecorder::last_message_;
}

TEST(MemoryPoolChainTest, BrokenChainBeforeTailCaught) {
    FaultRecorder rec;
    MemoryPool<Order> pool(4);
    uint32_t a = pool.allocate();
    uint32_t b = pool.allocate();
    uint32_t c = pool.allocate();
    pool.data()[a].next = b;
    pool.data()[b].next = INVALID_INDEX;  // chain breaks before c
    pool.data()[c].next = INVALID_INDEX;
    pool.deallocate_chain(a, c);  // tail unreachable -> fault
    EXPECT_GE(FaultRecorder::count_, 1);
    EXPECT_NE(FaultRecorder::last_message_.find("broken before reaching tail"), std::string::npos)
        << "message: " << FaultRecorder::last_message_;
}
#endif

// ── is_allocated() ────────────────────────────────────────────────────────
#if HFT_DEBUG_TOOLS
TEST(MemoryPoolLifecycleTest, IsAllocatedAccurate) {
    MemoryPool<Order> pool(8);
    uint32_t a = pool.allocate();
    uint32_t b = pool.allocate();
    EXPECT_TRUE(pool.is_allocated(a));
    EXPECT_TRUE(pool.is_allocated(b));
    pool.deallocate(a);
    EXPECT_FALSE(pool.is_allocated(a));
    EXPECT_TRUE(pool.is_allocated(b));
    EXPECT_FALSE(pool.is_allocated(1000));  // out of range is false, not a lie
}
#endif

// ── Poison canaries & generation stamps ────────────────────────────────
#if HFT_DEBUG_TOOLS
TEST(MemoryPoolCanaryTest, FreedSlotPayloadIsPoisoned) {
    MemoryPool<Order> pool(4);
    uint32_t a = pool.allocate();
    pool.data()[a].id = 42;
    pool.data()[a].price = 1234;
    pool.data()[a].quantity = 99;
    pool.deallocate(a);
    EXPECT_TRUE(pool.slot_payload_is_poisoned(a));  // 0xDD everywhere past the link
    // Direct byte check: a stale read of id must see poison, not old data.
    EXPECT_EQ(pool.data()[a].id, 0xDDDDDDDDu);
    EXPECT_EQ(pool.data()[a].price, 0xDDDDDDDDu);
    EXPECT_EQ(pool.data()[a].quantity, 0xDDDDDDDDu);
}

TEST(MemoryPoolCanaryTest, FreshSlotIsValueInitializedThenFreshPoisoned) {
    MemoryPool<Order> pool(4);
    uint32_t a = pool.allocate();
    // Debug allocate() value-initializes then poisons with 0xDE — any field
    // read before the caller initializes it shows the fresh pattern:
    EXPECT_EQ(pool.data()[a].id, 0xDEDEDEDEu);
    EXPECT_EQ(pool.data()[a].price, 0xDEDEDEDEu);
}

TEST(MemoryPoolCanaryTest, GenerationIncrementsPerReallocation) {
    MemoryPool<Order> pool(4);
    uint32_t a = pool.allocate();
    EXPECT_EQ(pool.slot_generation(a), 1);
    pool.deallocate(a);
    EXPECT_EQ(pool.slot_generation(a), 1);  // survives the free
    uint32_t b = pool.allocate();
    EXPECT_EQ(b, a);
    EXPECT_EQ(pool.slot_generation(b), 2);  // bumped on realloc
}
#endif

// ── Allocation tracking / statistics ─────────────────────────────────
#if HFT_DEBUG_TOOLS
TEST(MemoryPoolStatsTest, CountersAndHighWater) {
    MemoryPool<Order> pool(10);
    EXPECT_EQ(pool.stats().live_count, 0);
    uint32_t a = pool.allocate();
    uint32_t b = pool.allocate();
    (void)b;  // allocated to exercise the alloc counter
    EXPECT_EQ(pool.stats().total_allocs, 2);
    EXPECT_EQ(pool.stats().live_count, 2);
    EXPECT_EQ(pool.stats().high_water, 2);
    pool.deallocate(a);
    EXPECT_EQ(pool.stats().total_frees, 1);
    EXPECT_EQ(pool.stats().live_count, 1);
    EXPECT_EQ(pool.stats().high_water, 2);  // high-water never decreases
    uint32_t c = pool.allocate();
    (void)c;
    EXPECT_EQ(pool.stats().live_count, 2);
    EXPECT_EQ(pool.stats().high_water, 2);
    uint32_t d = pool.allocate();  // 3 live — new high water
    (void)d;
    EXPECT_EQ(pool.stats().high_water, 3);
    EXPECT_EQ(pool.stats().free_list_depth, 10 - 3);
}

TEST(MemoryPoolStatsTest, FreeListDepthReconcilesWithLiveCount) {
    MemoryPool<Order> pool(16);
    std::vector<uint32_t> live;
    for (int i = 0; i < 10; ++i) live.push_back(pool.allocate());
    pool.deallocate(live[3]);
    pool.deallocate(live[7]);
    pool.deallocate(live[1]);
    const auto st = pool.stats();
    EXPECT_EQ(st.live_count, 7);
    EXPECT_EQ(st.free_list_depth, 16 - 7);  // depth == capacity - live, always
}
#endif

// ── Property test: random alloc/free vs a shadow model (10,000 ops) ──────
#if HFT_DEBUG_TOOLS
TEST(MemoryPoolPropertyTest, RandomAllocFreeMatchesShadowModel) {
    struct FastRng {
        uint64_t s = 0x9E3779B97F4A7C15ULL;
        uint32_t next() {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
        }
    };
    FastRng rng;
    constexpr size_t CAP = 64;
    MemoryPool<Order> pool(CAP);
    std::set<uint32_t> shadow_live;  // the model: which slots must be live
    std::vector<uint32_t> live;

    for (int op = 0; op < 10'000; ++op) {
        bool do_alloc = live.empty() || (rng.next() % 100) < 55;
        if (do_alloc) {
            uint32_t idx = pool.allocate();
            if (shadow_live.size() >= CAP) {
                EXPECT_EQ(idx, INVALID_INDEX);
            } else {
                ASSERT_NE(idx, INVALID_INDEX);
                EXPECT_EQ(shadow_live.count(idx), 0u);  // never hand out a live slot twice
                shadow_live.insert(idx);
                live.push_back(idx);
            }
        } else {
            size_t pick = rng.next() % live.size();
            uint32_t idx = live[pick];
            live[pick] = live.back();
            live.pop_back();
            pool.deallocate(idx);
            EXPECT_EQ(shadow_live.erase(idx), 1u);
        }
        // Continuous reconciliation:
        if ((op % 97) == 0) {
            const auto st = pool.stats();
            EXPECT_EQ(st.live_count, shadow_live.size());
            EXPECT_EQ(st.free_list_depth, CAP - shadow_live.size());
        }
    }
    // Drain and confirm full recovery:
    for (uint32_t idx : live) pool.deallocate(idx);
    EXPECT_EQ(pool.stats().live_count, 0);
    EXPECT_EQ(pool.stats().free_list_depth, CAP);
}
#endif
