#include <benchmark/benchmark.h>
#include "../src/OrderBook.h"
#include "StdOrderBook.h"

using namespace hft;

// ============================================================
// 1. Passive Insertion (no matching)
//    Tests: MemoryPool O(1) slab alloc vs heap new/malloc
// ============================================================
template <class BookType>
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    BookType ob(state.max_iterations + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
    }
}
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, OrderBook)->Iterations(1000000);
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, StdOrderBook)->Iterations(200000);

// ============================================================
// 2. Single match against a deep book
//    Pre-seeds across 5000 price levels to stress cache hierarchy.
//    Tests: bitset best-price scan vs std::map tree walk,
//           pool dealloc vs heap free, array lookup vs hash
// ============================================================
template <class BookType>
static void BM_Matching_DeepBook(benchmark::State& state) {
    const int64_t seed = static_cast<int64_t>(state.max_iterations) + 5000;
    BookType ob(static_cast<size_t>(seed * 2 + 10), 0, 20000);
    OrderId id = 1;
    // Seed sells across 5000 price levels (wide spread stresses tree traversal)
    for (int64_t i = 0; i < seed; ++i) {
        ob.add_order(id++, OrderType::Limit,
                     10000 + static_cast<Price>(i % 5000), 10, Side::Sell,
                     [](const Trade& t){ benchmark::DoNotOptimize(t); });
    }
    // Each aggressive buy matches one resting sell at the best ask
    for (auto _ : state) {
        ob.add_order(id++, OrderType::Limit, 15000, 10, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
    }
}
BENCHMARK_TEMPLATE(BM_Matching_DeepBook, OrderBook)->Iterations(100000);
BENCHMARK_TEMPLATE(BM_Matching_DeepBook, StdOrderBook)->Iterations(50000);

// ============================================================
// 3. Batch match — one aggressive buy fills 25 resting sells
//    at the same price level in a single operation.
//    Tests: intrusive linked-list traversal over contiguous
//           pool memory vs std::list node-hopping
// ============================================================
template <class BookType>
static void BM_BatchMatching(benchmark::State& state) {
    constexpr int BATCH = 25;
    BookType ob(static_cast<size_t>(state.max_iterations) * (BATCH + 1) + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < BATCH; ++i) {
            ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell, [](const Trade& t){
                benchmark::DoNotOptimize(t);
            });
        }
        state.ResumeTiming();
        // One aggressive buy sweeps all resting sells
        ob.add_order(id++, OrderType::Limit, 100, 10 * BATCH, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
    }
}
BENCHMARK_TEMPLATE(BM_BatchMatching, OrderBook)->Iterations(10000);
BENCHMARK_TEMPLATE(BM_BatchMatching, StdOrderBook)->Iterations(5000);

// ============================================================
// 4. Add and immediate Cancel
//    Tests: pool alloc+dealloc cycle vs heap alloc+dealloc
// ============================================================
template <class BookType>
static void BM_AddAndCancel(benchmark::State& state) {
    BookType ob(state.max_iterations + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        ob.add_order(id, OrderType::Limit, 100, 10, Side::Sell, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
        ob.cancel_order(id);
        id++;
    }
}
BENCHMARK_TEMPLATE(BM_AddAndCancel, OrderBook)->Iterations(1000000);
BENCHMARK_TEMPLATE(BM_AddAndCancel, StdOrderBook)->Iterations(200000);

// ============================================================
// 5. Cancel orders from a deep pre-populated book
//    Tests: O(1) direct order_map_[id] array access vs
//           unordered_map.find() hash lookup with millions
//           of entries causing cache misses & hash collisions
// ============================================================
template <class BookType>
static void BM_CancelInDeepBook(benchmark::State& state) {
    const int64_t count = static_cast<int64_t>(state.max_iterations) + 5000;
    BookType ob(static_cast<size_t>(count + 10), 0, 20000);
    // Pre-fill the book across 5000 price levels
    for (int64_t i = 1; i <= count; ++i) {
        ob.add_order(static_cast<OrderId>(i), OrderType::Limit,
                     10000 + static_cast<Price>(i % 5000), 10, Side::Sell,
                     [](const Trade& t){ benchmark::DoNotOptimize(t); });
    }
    OrderId cancel_id = 1;
    for (auto _ : state) {
        ob.cancel_order(cancel_id++);
    }
}
BENCHMARK_TEMPLATE(BM_CancelInDeepBook, OrderBook)->Iterations(100000);
BENCHMARK_TEMPLATE(BM_CancelInDeepBook, StdOrderBook)->Iterations(50000);

// ============================================================
// 6. Market Order against deep book
// ============================================================
template <class BookType>
static void BM_MarketOrder_DeepBook(benchmark::State& state) {
    const int64_t seed = static_cast<int64_t>(state.max_iterations) + 5000;
    BookType ob(static_cast<size_t>(seed * 2 + 10), 0, 20000);
    OrderId id = 1;
    for (int64_t i = 0; i < seed; ++i) {
        ob.add_order(id++, OrderType::Limit,
                     10000 + static_cast<Price>(i % 5000), 10, Side::Sell,
                     [](const Trade& t){ benchmark::DoNotOptimize(t); });
    }
    for (auto _ : state) {
        ob.add_order(id++, OrderType::Market, 0, 10, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
    }
}
BENCHMARK_TEMPLATE(BM_MarketOrder_DeepBook, OrderBook)->Iterations(100000);
BENCHMARK_TEMPLATE(BM_MarketOrder_DeepBook, StdOrderBook)->Iterations(50000);

// ============================================================
// 7. Sweep 50 price levels with one aggressive order
//    Tests: bitset-accelerated level scanning vs std::map
//           tree traversal + repeated tree rebalancing on erase
// ============================================================
template <class BookType>
static void BM_SweepMultipleLevels(benchmark::State& state) {
    constexpr int LEVELS = 50;
    BookType ob(state.max_iterations * (LEVELS + 1) + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        for (int lvl = 0; lvl < LEVELS; ++lvl) {
            ob.add_order(id++, OrderType::Limit, 100 + lvl, 10, Side::Sell, [](const Trade& t){
                benchmark::DoNotOptimize(t);
            });
        }
        state.ResumeTiming();
        // Sweep all levels
        ob.add_order(id++, OrderType::Limit, 200, 10 * LEVELS, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
    }
}
BENCHMARK_TEMPLATE(BM_SweepMultipleLevels, OrderBook)->Iterations(5000);
BENCHMARK_TEMPLATE(BM_SweepMultipleLevels, StdOrderBook)->Iterations(2000);

// ============================================================
// 8. Wide Spread Matching — orders across 10000+ levels
//    Destroys std::map cache locality with deep tree traversal
// ============================================================
template <class BookType>
static void BM_WideSpreadMatching(benchmark::State& state) {
    const int64_t seed = static_cast<int64_t>(state.max_iterations) + 10000;
    BookType ob(static_cast<size_t>(seed * 2 + 10), 0, 20000);
    OrderId id = 1;
    // Seed sells across 10000 price levels (maximum cache pressure)
    for (int64_t i = 0; i < seed; ++i) {
        ob.add_order(id++, OrderType::Limit,
                     5000 + static_cast<Price>(i % 10000), 10, Side::Sell,
                     [](const Trade& t){ benchmark::DoNotOptimize(t); });
    }
    for (auto _ : state) {
        ob.add_order(id++, OrderType::Limit, 15000, 10, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
    }
}
BENCHMARK_TEMPLATE(BM_WideSpreadMatching, OrderBook)->Iterations(100000);
BENCHMARK_TEMPLATE(BM_WideSpreadMatching, StdOrderBook)->Iterations(50000);

// ============================================================
// 9. Mixed Workload — realistic 70/20/10 insert/match/cancel
// ============================================================
template <class BookType>
static void BM_MixedWorkload(benchmark::State& state) {
    BookType ob(state.max_iterations * 2 + 10, 0, 5000);
    OrderId id = 1;
    uint64_t i = 0;
    for (auto _ : state) {
        uint64_t op = i % 10;
        if (op < 7) {
            // Insert passive
            ob.add_order(id++, OrderType::Limit, 1000 + (i % 200), 10, Side::Sell, [](const Trade& t){
                benchmark::DoNotOptimize(t);
            });
        } else if (op < 9) {
            // Aggressive match
            ob.add_order(id++, OrderType::Limit, 1200, 10, Side::Buy, [](const Trade& t){
                benchmark::DoNotOptimize(t);
            });
        } else {
            // Cancel a recent order
            OrderId cancel_id = (id > 5) ? id - 5 : 1;
            ob.cancel_order(cancel_id);
        }
        ++i;
    }
}
BENCHMARK_TEMPLATE(BM_MixedWorkload, OrderBook)->Iterations(500000);
BENCHMARK_TEMPLATE(BM_MixedWorkload, StdOrderBook)->Iterations(100000);

BENCHMARK_MAIN();
