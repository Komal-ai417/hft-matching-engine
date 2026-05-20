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
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, OrderBook);
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, StdOrderBook);

// ============================================================
// 2. Single match against a deep book (NO PauseTiming noise)
//    Pre-seeds the book, then each iteration submits one
//    aggressive order that crosses against the best resting.
//    Tests: bitset best-price scan vs std::map tree walk,
//           pool dealloc vs heap free, array lookup vs hash
// ============================================================
template <class BookType>
static void BM_Matching_DeepBook(benchmark::State& state) {
    const int64_t seed = static_cast<int64_t>(state.max_iterations) + 100;
    BookType ob(static_cast<size_t>(seed * 2 + 10), 0, 20000);
    OrderId id = 1;
    // Seed sells across 1000 price levels
    for (int64_t i = 0; i < seed; ++i) {
        ob.add_order(id++, OrderType::Limit,
                     10000 + static_cast<Price>(i % 1000), 10, Side::Sell,
                     [](const Trade&){});
    }
    // Each aggressive buy matches one resting sell at the best ask
    for (auto _ : state) {
        ob.add_order(id++, OrderType::Limit, 11000, 10, Side::Buy, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_Matching_DeepBook, OrderBook);
BENCHMARK_TEMPLATE(BM_Matching_DeepBook, StdOrderBook);

// ============================================================
// 3. Batch match — one aggressive buy fills 10 resting sells
//    at the same price level in a single operation.
//    Tests: intrusive linked-list traversal over contiguous
//           pool memory vs std::list node-hopping across
//           scattered heap allocations (cache locality)
// ============================================================
template <class BookType>
static void BM_BatchMatching(benchmark::State& state) {
    constexpr int BATCH = 10;
    BookType ob(static_cast<size_t>(state.max_iterations) * (BATCH + 1) + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < BATCH; ++i) {
            ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&){});
        }
        state.ResumeTiming();
        // One aggressive buy sweeps all 10 resting sells
        ob.add_order(id++, OrderType::Limit, 100, 10 * BATCH, Side::Buy, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_BatchMatching, OrderBook);
BENCHMARK_TEMPLATE(BM_BatchMatching, StdOrderBook);

// ============================================================
// 4. Add and immediate Cancel
//    Tests: pool alloc+dealloc cycle vs heap alloc+dealloc
// ============================================================
template <class BookType>
static void BM_AddAndCancel(benchmark::State& state) {
    BookType ob(state.max_iterations + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        ob.add_order(id, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&){});
        ob.cancel_order(id);
        id++;
    }
}
BENCHMARK_TEMPLATE(BM_AddAndCancel, OrderBook);
BENCHMARK_TEMPLATE(BM_AddAndCancel, StdOrderBook);

// ============================================================
// 5. Cancel orders from a deep pre-populated book
//    Tests: O(1) direct order_map_[id] array access vs
//           unordered_map.find() hash lookup with millions
//           of entries causing cache misses & hash collisions
// ============================================================
template <class BookType>
static void BM_CancelInDeepBook(benchmark::State& state) {
    const int64_t count = static_cast<int64_t>(state.max_iterations) + 100;
    BookType ob(static_cast<size_t>(count + 10), 0, 20000);
    // Pre-fill the book across 1000 price levels
    for (int64_t i = 1; i <= count; ++i) {
        ob.add_order(static_cast<OrderId>(i), OrderType::Limit,
                     10000 + static_cast<Price>(i % 1000), 10, Side::Sell,
                     [](const Trade&){});
    }
    OrderId cancel_id = 1;
    for (auto _ : state) {
        ob.cancel_order(cancel_id++);
    }
}
BENCHMARK_TEMPLATE(BM_CancelInDeepBook, OrderBook);
BENCHMARK_TEMPLATE(BM_CancelInDeepBook, StdOrderBook);

// ============================================================
// 6. Market Order against deep book (NO PauseTiming noise)
// ============================================================
template <class BookType>
static void BM_MarketOrder_DeepBook(benchmark::State& state) {
    const int64_t seed = static_cast<int64_t>(state.max_iterations) + 100;
    BookType ob(static_cast<size_t>(seed * 2 + 10), 0, 20000);
    OrderId id = 1;
    for (int64_t i = 0; i < seed; ++i) {
        ob.add_order(id++, OrderType::Limit,
                     10000 + static_cast<Price>(i % 1000), 10, Side::Sell,
                     [](const Trade&){});
    }
    for (auto _ : state) {
        ob.add_order(id++, OrderType::Market, 0, 10, Side::Buy, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_MarketOrder_DeepBook, OrderBook);
BENCHMARK_TEMPLATE(BM_MarketOrder_DeepBook, StdOrderBook);

// ============================================================
// 7. Sweep 20 price levels with one aggressive order
//    Tests: bitset-accelerated level scanning vs std::map
//           tree traversal + repeated tree rebalancing on erase
// ============================================================
template <class BookType>
static void BM_SweepMultipleLevels(benchmark::State& state) {
    constexpr int LEVELS = 20;
    BookType ob(state.max_iterations * (LEVELS + 1) + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        for (int lvl = 0; lvl < LEVELS; ++lvl) {
            ob.add_order(id++, OrderType::Limit, 100 + lvl, 10, Side::Sell, [](const Trade&){});
        }
        state.ResumeTiming();
        // Sweep all 20 levels
        ob.add_order(id++, OrderType::Limit, 200, 10 * LEVELS, Side::Buy, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_SweepMultipleLevels, OrderBook);
BENCHMARK_TEMPLATE(BM_SweepMultipleLevels, StdOrderBook);

BENCHMARK_MAIN();
