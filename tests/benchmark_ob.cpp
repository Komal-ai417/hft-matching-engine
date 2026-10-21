#include <benchmark/benchmark.h>

#include <memory>

#include "../src/OrderBook.h"
#include "StdOrderBook.h"

using namespace hft;

// ============================================================
// 1. Passive Insertion (no matching)
//    Tests: MemoryPool O(1) slab alloc vs heap new/malloc
// ============================================================
template<class BookType>
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    auto ob = std::make_unique<BookType>(10000000, 10000000, 0, 2000);
    OrderId id = 1;
    uint64_t orders_added = 0;
    for (auto _ : state) {
        if (HFT_UNLIKELY(id >= 9990000)) {
            state.PauseTiming();
            ob = std::make_unique<BookType>(10000000, 10000000, 0, 2000);
            id = 1;
            state.ResumeTiming();
        }
        (void)ob->add_order(id++, OrderType::Limit, 100, 10, Side::Sell,
                            [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        orders_added++;
    }
    state.counters["orders"] = benchmark::Counter(orders_added, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, OrderBook);
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, StdOrderBook);

// ============================================================
// 2. Continuous match against a deep book
//    (audit L-10: name and comment now say what this actually measures —
//    each iteration adds one resting sell and immediately consumes the
//    whole best level with a crossing buy; 5000 price levels are resident,
//    stressing the cache hierarchy and both bitset directions.)
//    Tests: bitset best-price scan vs std::map tree walk,
//           pool dealloc vs heap free, array lookup vs hash
// ============================================================
template<class BookType>
static void BM_Matching_DeepBook(benchmark::State& state) {
    const int64_t seed = 5000;
    BookType ob(10000000, 10000000, 0, 20000);
    OrderId id = 1;
    // Seed sells across 5000 price levels
    for (int64_t i = 0; i < seed; ++i) {
        (void)ob.add_order(id++, OrderType::Limit, 10000 + static_cast<Price>(i % 5000), 10, Side::Sell,
                           [](const Trade& t) noexcept {
                               Trade sink = t;
                               benchmark::DoNotOptimize(sink);
                           });
    }
    uint64_t matches = 0;
    OrderId base_id = seed + 1;
    for (auto _ : state) {
        // IDs rotate safely without exhaustion because the orders are immediately consumed.
        OrderId sell_id = base_id + (matches % 100000) * 2;
        OrderId buy_id = base_id + (matches % 100000) * 2 + 1;

        (void)ob.add_order(sell_id, OrderType::Limit, 10000 + static_cast<Price>(matches % 5000), 10, Side::Sell,
                           [](const Trade&) noexcept {});
        (void)ob.add_order(buy_id, OrderType::Limit, 15000, 10, Side::Buy,
                           [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        matches++;
    }
    // Divide by 2 because each iteration does 1 Add and 1 Match
    state.counters["ops"] = benchmark::Counter(matches * 2, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_Matching_DeepBook, OrderBook);
BENCHMARK_TEMPLATE(BM_Matching_DeepBook, StdOrderBook);

// ============================================================
// 3. Batch match — one aggressive buy fills 25 resting sells
//    at the same price level in a single operation.
//    Tests: intrusive linked-list traversal over contiguous
//           pool memory vs std::list node-hopping
// ============================================================
template<class BookType>
static void BM_BatchMatching(benchmark::State& state) {
    constexpr int BATCH = 25;
    BookType ob(10000000, 10000000, 0, 2000);
    uint64_t batches = 0;
    OrderId base_id = 1;
    for (auto _ : state) {
        OrderId batch_base = base_id + (batches % 100000) * (BATCH + 1);
        for (int i = 0; i < BATCH; ++i) {
            (void)ob.add_order(batch_base + i, OrderType::Limit, 100, 10, Side::Sell,
                               [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        }
        // One aggressive buy sweeps all resting sells
        (void)ob.add_order(batch_base + BATCH, OrderType::Limit, 100, 10 * BATCH, Side::Buy,
                           [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        batches++;
    }
    // Ops = BATCH additions + 1 aggressive addition (which triggers BATCH trades)
    state.counters["ops"] = benchmark::Counter(batches * (BATCH + 1), benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_BatchMatching, OrderBook);
BENCHMARK_TEMPLATE(BM_BatchMatching, StdOrderBook);

// ============================================================
// 4. Add and immediate Cancel
//    Tests: pool alloc+dealloc cycle vs heap alloc+dealloc
// ============================================================
template<class BookType>
static void BM_AddAndCancel(benchmark::State& state) {
    BookType ob(10000000, 10000000, 0, 2000);
    uint64_t ops = 0;
    OrderId base_id = 1;
    for (auto _ : state) {
        OrderId id = base_id + (ops % 100000);
        (void)ob.add_order(id, OrderType::Limit, 100, 10, Side::Sell,
                           [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        (void)ob.cancel_order(id);
        ops++;
    }
    state.counters["cycles"] = benchmark::Counter(ops, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_AddAndCancel, OrderBook);
BENCHMARK_TEMPLATE(BM_AddAndCancel, StdOrderBook);

// ============================================================
// 5. Cancel orders from a deep pre-populated book
//    Tests: O(1) direct order_map_[id] array access vs
//           unordered_map.find() hash lookup with millions
//           of entries causing cache misses & hash collisions
// ============================================================
template<class BookType>
static void BM_CancelInDeepBook(benchmark::State& state) {
    const int64_t count = 5000;
    BookType ob(10000000, 10000000, 0, 20000);
    for (int64_t i = 1; i <= count; ++i) {
        (void)ob.add_order(static_cast<OrderId>(i), OrderType::Limit, 10000 + static_cast<Price>(i % 5000), 10,
                           Side::Sell, [](const Trade& t) noexcept {
                               Trade sink = t;
                               benchmark::DoNotOptimize(sink);
                           });
    }
    OrderId base_id = count + 1;
    uint64_t ops = 0;
    for (auto _ : state) {
        OrderId current_id = base_id + (ops % 100000);
        (void)ob.add_order(current_id, OrderType::Limit, 10000 + static_cast<Price>(ops % 5000), 10, Side::Sell,
                           [](const Trade&) noexcept {});
        (void)ob.cancel_order(current_id);
        ops++;
    }
    state.counters["ops"] = benchmark::Counter(ops * 2, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_CancelInDeepBook, OrderBook);
BENCHMARK_TEMPLATE(BM_CancelInDeepBook, StdOrderBook);

// ============================================================
// 6. Market Order against deep book
// ============================================================
template<class BookType>
static void BM_MarketOrder_DeepBook(benchmark::State& state) {
    const int64_t seed = 5000;
    BookType ob(10000000, 10000000, 0, 20000);
    OrderId id = 1;
    for (int64_t i = 0; i < seed; ++i) {
        (void)ob.add_order(id++, OrderType::Limit, 10000 + static_cast<Price>(i % 5000), 10, Side::Sell,
                           [](const Trade& t) noexcept {
                               Trade sink = t;
                               benchmark::DoNotOptimize(sink);
                           });
    }
    uint64_t matches = 0;
    OrderId base_id = seed + 1;
    for (auto _ : state) {
        OrderId sell_id = base_id + (matches % 100000) * 2;
        OrderId buy_id = base_id + (matches % 100000) * 2 + 1;
        (void)ob.add_order(sell_id, OrderType::Limit, 10000 + static_cast<Price>(matches % 5000), 10, Side::Sell,
                           [](const Trade&) noexcept {});
        (void)ob.add_order(buy_id, OrderType::Market, 0, 10, Side::Buy,
                           [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        matches++;
    }
    state.counters["ops"] = benchmark::Counter(matches * 2, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_MarketOrder_DeepBook, OrderBook);
BENCHMARK_TEMPLATE(BM_MarketOrder_DeepBook, StdOrderBook);

// ============================================================
// 7. Sweep 50 price levels with one aggressive order
//    Tests: bitset-accelerated level scanning vs std::map
//           tree traversal + repeated tree rebalancing on erase
// ============================================================
template<class BookType>
static void BM_SweepMultipleLevels(benchmark::State& state) {
    constexpr int LEVELS = 50;
    BookType ob(10000000, 10000000, 0, 2000);
    uint64_t sweeps = 0;
    OrderId base_id = 1;
    for (auto _ : state) {
        OrderId sweep_base = base_id + (sweeps % 10000) * (LEVELS + 1);
        for (int lvl = 0; lvl < LEVELS; ++lvl) {
            (void)ob.add_order(sweep_base + lvl, OrderType::Limit, 100 + lvl, 10, Side::Sell,
                               [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        }
        // Sweep all levels
        (void)ob.add_order(sweep_base + LEVELS, OrderType::Limit, 200, 10 * LEVELS, Side::Buy,
                           [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        sweeps++;
    }
    state.counters["ops"] = benchmark::Counter(sweeps * (LEVELS + 1), benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_SweepMultipleLevels, OrderBook);
BENCHMARK_TEMPLATE(BM_SweepMultipleLevels, StdOrderBook);

// ============================================================
// 8. Wide Spread Matching — orders across 10000+ levels
//    Destroys std::map cache locality with deep tree traversal
// ============================================================
template<class BookType>
static void BM_WideSpreadMatching(benchmark::State& state) {
    const int64_t seed = 10000;
    BookType ob(10000000, 10000000, 0, 20000);
    OrderId id = 1;
    // Seed sells across 10000 price levels
    for (int64_t i = 0; i < seed; ++i) {
        (void)ob.add_order(id++, OrderType::Limit, 5000 + static_cast<Price>(i % 10000), 10, Side::Sell,
                           [](const Trade& t) noexcept {
                               Trade sink = t;
                               benchmark::DoNotOptimize(sink);
                           });
    }
    uint64_t matches = 0;
    OrderId base_id = seed + 1;
    for (auto _ : state) {
        OrderId sell_id = base_id + (matches % 100000) * 2;
        OrderId buy_id = base_id + (matches % 100000) * 2 + 1;
        (void)ob.add_order(sell_id, OrderType::Limit, 5000 + static_cast<Price>(matches % 10000), 10, Side::Sell,
                           [](const Trade&) noexcept {});
        (void)ob.add_order(buy_id, OrderType::Limit, 15000, 10, Side::Buy,
                           [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        matches++;
    }
    state.counters["ops"] = benchmark::Counter(matches * 2, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_WideSpreadMatching, OrderBook);
BENCHMARK_TEMPLATE(BM_WideSpreadMatching, StdOrderBook);

// ============================================================
// 9. Mixed Workload — realistic 50/20/30 insert/match/cancel
// ============================================================
template<class BookType>
static void BM_MixedWorkload(benchmark::State& state) {
    BookType ob(10000000, 10000000, 0, 5000);
    uint64_t i = 0;
    for (auto _ : state) {
        uint64_t op = i % 10;
        OrderId current_id = 1 + (i % 100000);
        if (op < 5) {
            // Insert passive
            (void)ob.add_order(current_id, OrderType::Limit, 1000 + (i % 200), 10, Side::Sell,
                               [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        } else if (op < 7) {
            // Aggressive match
            (void)ob.add_order(current_id, OrderType::Limit, 1200, 10, Side::Buy,
                               [](const Trade& t) noexcept { benchmark::DoNotOptimize(&t); });
        } else {
            // Cancel a recent order (ensure it's not the one we just added if we can help it, though it might be
            // missing)
            OrderId cancel_id = 1 + ((i + 99995) % 100000);
            (void)ob.cancel_order(cancel_id);
        }
        ++i;
    }
    state.counters["ops"] = benchmark::Counter(i, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_MixedWorkload, OrderBook);
BENCHMARK_TEMPLATE(BM_MixedWorkload, StdOrderBook);

// ============================================================
// 10. The "Churn" Test (audit report #2 §7) — fragmentation stress.
//     Setup: 100k orders across random prices; cancel a random 50k.
//     Measured: the next 10k adds on the maximally-fragmented free list.
//     Why: verifies the pool's LIFO free-list traversal under realistic
//     fragmentation and whether the deallocate-side prefetch warms L1.
// ============================================================
template<class BookType>
static void BM_Churn(benchmark::State& state) {
    struct FastRng {
        uint64_t s = 0x1234'5678'9ABC'DEF0ULL;
        uint32_t next() {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
        }
        uint32_t next(uint32_t max) { return max ? next() % max : 0; }
    };
    BookType ob(200'010, 150'010, 0, 20000);
    FastRng rng;
    std::vector<OrderId> ids;
    ids.reserve(100'000);
    for (uint32_t i = 1; i <= 100'000; ++i) {
        (void)ob.add_order(i, OrderType::Limit, 1000 + rng.next(10'000), 10, rng.next(2) ? Side::Buy : Side::Sell,
                           [](const Trade&) noexcept {});
        ids.push_back(i);
    }
    for (size_t i = ids.size() - 1; i > 0; --i) {
        size_t j = rng.next(static_cast<uint32_t>(i + 1));
        std::swap(ids[i], ids[j]);
    }
    for (size_t i = 0; i < ids.size() / 2; ++i) {
        (void)ob.cancel_order(ids[i]);
    }
    uint64_t adds = 0;
    uint32_t i = 0;
    for (auto _ : state) {
        OrderId id = 150'001 + (i % 40'000);
        (void)ob.add_order(id, OrderType::Limit, 1000 + rng.next(10'000), 10, Side::Sell,
                           [](const Trade&) noexcept { benchmark::DoNotOptimize(0); });
        ++i;
        ++adds;
    }
    state.counters["adds"] = benchmark::Counter(adds, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_Churn, OrderBook);
BENCHMARK_TEMPLATE(BM_Churn, StdOrderBook);

// ============================================================
// 11. Allocation path — isolated pool alloc+dealloc cycle.
//     One add + one cancel per iteration at the SAME price level: the
//     purest measurement of MemoryPool allocate/deallocate plus level
//     append/remove, with zero matching and zero bitset work.
// ============================================================
template<class BookType>
static void BM_AllocationPath(benchmark::State& state) {
    BookType ob(1'000'010, 1'000'010, 0, 2000);
    uint64_t ops = 0;
    OrderId id = 1;
    for (auto _ : state) {
        (void)ob.add_order(id, OrderType::Limit, 1000, 10, Side::Sell, [](const Trade&) noexcept {});
        (void)ob.cancel_order(id);
        if (++id > 1'000'000) id = 1;  // IDs are free again after cancel
        ++ops;
    }
    state.counters["cycles"] = benchmark::Counter(ops, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_AllocationPath, OrderBook);
BENCHMARK_TEMPLATE(BM_AllocationPath, StdOrderBook);

// ============================================================
// 12. Branch predictability — same op, alternately crossing and not.
//     The engine's match loop exits on `taker_price >= best_ask_`; an
//     alternating stream defeats the branch predictor where a steady
//     stream is perfectly predicted. The delta between the two variants
//     isolates misprediction cost on the hot path.
// ============================================================
template<class BookType>
static void BM_BranchPredictability(benchmark::State& state) {
    BookType ob(2'000'010, 1'000'010, 0, 4000);
    // A stable resting bid at 1000 and ask at 3000.
    (void)ob.add_order(1, OrderType::Limit, 1000, 1'000'000, Side::Buy, [](const Trade&) noexcept {});
    (void)ob.add_order(2, OrderType::Limit, 3000, 1'000'000, Side::Sell, [](const Trade&) noexcept {});
    uint64_t ops = 0;
    OrderId id = 10;
    bool cross = false;
    for (auto _ : state) {
        // cross=false: sell at 2999 rests (no cross). cross=true: sell at
        // 1000 crosses the bid immediately (match branch taken).
        Price p = cross ? 1000 : 2999;
        (void)ob.add_order(id, OrderType::Limit, p, 1, Side::Sell,
                           [](const Trade&) noexcept { benchmark::DoNotOptimize(0); });
        (void)ob.cancel_order(id);  // keep the book shape constant
        cross = !cross;
        ++id;
        ++ops;
    }
    state.counters["ops"] = benchmark::Counter(ops, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_BranchPredictability, OrderBook);
BENCHMARK_TEMPLATE(BM_BranchPredictability, StdOrderBook);

// ============================================================
// 13. Cache locality — hot single level vs round-robin across levels.
//     hot:  every op touches price 1000 (L1-resident level + warm slots).
//     cold: every op touches a different price out of 2000 (defeats the
//     level-array cache and spreads pool slots across the slab).
// ============================================================
template<class BookType>
static void BM_CacheLocality(benchmark::State& state) {
    const bool hot = state.range(0) == 1;
    BookType ob(1'000'010, 1'000'010, 0, 4000);
    uint64_t ops = 0;
    OrderId id = 1;
    uint32_t rotator = 0;
    for (auto _ : state) {
        Price p = hot ? 1000 : (500 + (rotator++ % 2000));
        (void)ob.add_order(id, OrderType::Limit, p, 10, Side::Sell, [](const Trade&) noexcept {});
        (void)ob.cancel_order(id);
        ++id;
        ++ops;
    }
    state.counters["ops"] = benchmark::Counter(ops, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_CacheLocality, OrderBook)->Arg(1)->Arg(0);
BENCHMARK_TEMPLATE(BM_CacheLocality, StdOrderBook)->Arg(1)->Arg(0);

// ============================================================
// 14. Book-depth scaling — how does match latency scale with the number
//     of populated levels behind the touch? Parameterized depth; the
//     taker consumes only the best level, so the depth cost is purely
//     best-price maintenance + cache pressure, not sweep length.
// ============================================================
template<class BookType>
static void BM_BookDepthScaling(benchmark::State& state) {
    const int64_t depth = state.range(0);
    BookType ob(2'000'010, 2'000'010, 0, 20000);
    OrderId id = 1;
    for (int64_t i = 0; i < depth; ++i) {
        (void)ob.add_order(id++, OrderType::Limit, 10000 + static_cast<Price>(i), 10, Side::Sell,
                           [](const Trade&) noexcept {});
    }
    uint64_t matches = 0;
    for (auto _ : state) {
        // Seed one maker at the touch, then consume it with a taker.
        OrderId maker = 1'500'000 + (matches % 100'000) * 2;
        OrderId taker = maker + 1;
        (void)ob.add_order(maker, OrderType::Limit, 10000, 10, Side::Sell, [](const Trade&) noexcept {});
        (void)ob.add_order(taker, OrderType::Limit, 10000, 10, Side::Buy, [](const Trade& t) noexcept {
            Trade sink = t;
            benchmark::DoNotOptimize(sink);
        });
        ++matches;
    }
    state.counters["matches"] = benchmark::Counter(matches, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_BookDepthScaling, OrderBook)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);
BENCHMARK_TEMPLATE(BM_BookDepthScaling, StdOrderBook)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

// ============================================================
// 15. Workload-mix scaling — insert-heavy vs match-heavy vs cancel-heavy.
//     Parameterized mix isolates which operation dominates throughput in
//     each regime (allocation path vs matching path vs unlinking path).
// ============================================================
template<class BookType>
static void BM_WorkloadMix(benchmark::State& state) {
    const int insert_pct = static_cast<int>(state.range(0));
    const int match_pct = static_cast<int>(state.range(1));  // remainder = cancel
    BookType ob(1'000'010, 1'000'010, 0, 5000);
    struct FastRng {
        uint64_t s = 0xC0FF'EE11'2233'4455ULL;
        uint32_t next() {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
        }
        uint32_t next(uint32_t max) { return max ? next() % max : 0; }
    };
    FastRng rng;
    uint64_t ops = 0;
    OrderId next_id = 1;
    for (auto _ : state) {
        const uint32_t roll = rng.next(100);
        if (static_cast<int>(roll) < insert_pct) {
            (void)ob.add_order(next_id++, OrderType::Limit, 1000 + rng.next(200), 10, Side::Sell,
                               [](const Trade&) noexcept {});
        } else if (static_cast<int>(roll) < insert_pct + match_pct) {
            (void)ob.add_order(next_id++, OrderType::Limit, 1200, 10, Side::Buy, [](const Trade& t) noexcept {
                Trade sink = t;
                benchmark::DoNotOptimize(sink);
            });
        } else {
            (void)ob.cancel_order(1 + rng.next(static_cast<uint32_t>(next_id - 1)));
        }
        ++ops;
    }
    state.counters["ops"] = benchmark::Counter(ops, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_WorkloadMix, OrderBook)->Args({90, 5})->Args({50, 25})->Args({10, 80});
BENCHMARK_TEMPLATE(BM_WorkloadMix, StdOrderBook)->Args({90, 5})->Args({50, 25})->Args({10, 80});

BENCHMARK_MAIN();
