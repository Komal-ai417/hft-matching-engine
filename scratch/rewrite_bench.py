import sys

bench_content = """#include <benchmark/benchmark.h>
#include "../src/OrderBook.h"
#include "StdOrderBook.h"
#include <memory>

using namespace hft;

// ============================================================
// 1. Passive Insertion (no matching)
//    Tests: MemoryPool O(1) slab alloc vs heap new/malloc
// ============================================================
template <class BookType>
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
        ob->add_order(id++, OrderType::Limit, 100, 10, Side::Sell, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
        orders_added++;
    }
    state.counters["orders"] = benchmark::Counter(orders_added, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, OrderBook);
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, StdOrderBook);

// ============================================================
// 2. Single match against a deep book
//    Pre-seeds across 5000 price levels to stress cache hierarchy.
//    Tests: bitset best-price scan vs std::map tree walk,
//           pool dealloc vs heap free, array lookup vs hash
// ============================================================
template <class BookType>
static void BM_Matching_DeepBook(benchmark::State& state) {
    const int64_t seed = 5000;
    BookType ob(10000000, 10000000, 0, 20000);
    OrderId id = 1;
    // Seed sells across 5000 price levels
    for (int64_t i = 0; i < seed; ++i) {
        ob.add_order(id++, OrderType::Limit,
                     10000 + static_cast<Price>(i % 5000), 10, Side::Sell,
                     [](const Trade& t){ benchmark::DoNotOptimize(t); });
    }
    uint64_t matches = 0;
    OrderId base_id = seed + 1;
    for (auto _ : state) {
        // IDs rotate safely without exhaustion because the orders are immediately consumed.
        OrderId sell_id = base_id + (matches % 100000) * 2;
        OrderId buy_id = base_id + (matches % 100000) * 2 + 1;
        
        ob.add_order(sell_id, OrderType::Limit, 10000 + static_cast<Price>(matches % 5000), 10, Side::Sell, [](const Trade&){});
        ob.add_order(buy_id, OrderType::Limit, 15000, 10, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
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
template <class BookType>
static void BM_BatchMatching(benchmark::State& state) {
    constexpr int BATCH = 25;
    BookType ob(10000000, 10000000, 0, 2000);
    uint64_t batches = 0;
    OrderId base_id = 1;
    for (auto _ : state) {
        OrderId batch_base = base_id + (batches % 100000) * (BATCH + 1);
        for (int i = 0; i < BATCH; ++i) {
            ob.add_order(batch_base + i, OrderType::Limit, 100, 10, Side::Sell, [](const Trade& t){
                benchmark::DoNotOptimize(t);
            });
        }
        // One aggressive buy sweeps all resting sells
        ob.add_order(batch_base + BATCH, OrderType::Limit, 100, 10 * BATCH, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
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
template <class BookType>
static void BM_AddAndCancel(benchmark::State& state) {
    BookType ob(10000000, 10000000, 0, 2000);
    uint64_t ops = 0;
    OrderId base_id = 1;
    for (auto _ : state) {
        OrderId id = base_id + (ops % 100000);
        ob.add_order(id, OrderType::Limit, 100, 10, Side::Sell, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
        ob.cancel_order(id);
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
template <class BookType>
static void BM_CancelInDeepBook(benchmark::State& state) {
    const int64_t count = 5000;
    BookType ob(10000000, 10000000, 0, 20000);
    for (int64_t i = 1; i <= count; ++i) {
        ob.add_order(static_cast<OrderId>(i), OrderType::Limit,
                     10000 + static_cast<Price>(i % 5000), 10, Side::Sell,
                     [](const Trade& t){ benchmark::DoNotOptimize(t); });
    }
    OrderId base_id = count + 1;
    uint64_t ops = 0;
    for (auto _ : state) {
        OrderId current_id = base_id + (ops % 100000);
        ob.add_order(current_id, OrderType::Limit, 10000 + static_cast<Price>(ops % 5000), 10, Side::Sell, [](const Trade&){});
        ob.cancel_order(current_id);
        ops++;
    }
    state.counters["ops"] = benchmark::Counter(ops * 2, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_CancelInDeepBook, OrderBook);
BENCHMARK_TEMPLATE(BM_CancelInDeepBook, StdOrderBook);

// ============================================================
// 6. Market Order against deep book
// ============================================================
template <class BookType>
static void BM_MarketOrder_DeepBook(benchmark::State& state) {
    const int64_t seed = 5000;
    BookType ob(10000000, 10000000, 0, 20000);
    OrderId id = 1;
    for (int64_t i = 0; i < seed; ++i) {
        ob.add_order(id++, OrderType::Limit,
                     10000 + static_cast<Price>(i % 5000), 10, Side::Sell,
                     [](const Trade& t){ benchmark::DoNotOptimize(t); });
    }
    uint64_t matches = 0;
    OrderId base_id = seed + 1;
    for (auto _ : state) {
        OrderId sell_id = base_id + (matches % 100000) * 2;
        OrderId buy_id = base_id + (matches % 100000) * 2 + 1;
        ob.add_order(sell_id, OrderType::Limit, 10000 + static_cast<Price>(matches % 5000), 10, Side::Sell, [](const Trade&){});
        ob.add_order(buy_id, OrderType::Market, 0, 10, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
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
template <class BookType>
static void BM_SweepMultipleLevels(benchmark::State& state) {
    constexpr int LEVELS = 50;
    BookType ob(10000000, 10000000, 0, 2000);
    uint64_t sweeps = 0;
    OrderId base_id = 1;
    for (auto _ : state) {
        OrderId sweep_base = base_id + (sweeps % 10000) * (LEVELS + 1);
        for (int lvl = 0; lvl < LEVELS; ++lvl) {
            ob.add_order(sweep_base + lvl, OrderType::Limit, 100 + lvl, 10, Side::Sell, [](const Trade& t){
                benchmark::DoNotOptimize(t);
            });
        }
        // Sweep all levels
        ob.add_order(sweep_base + LEVELS, OrderType::Limit, 200, 10 * LEVELS, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
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
template <class BookType>
static void BM_WideSpreadMatching(benchmark::State& state) {
    const int64_t seed = 10000;
    BookType ob(10000000, 10000000, 0, 20000);
    OrderId id = 1;
    // Seed sells across 10000 price levels
    for (int64_t i = 0; i < seed; ++i) {
        ob.add_order(id++, OrderType::Limit,
                     5000 + static_cast<Price>(i % 10000), 10, Side::Sell,
                     [](const Trade& t){ benchmark::DoNotOptimize(t); });
    }
    uint64_t matches = 0;
    OrderId base_id = seed + 1;
    for (auto _ : state) {
        OrderId sell_id = base_id + (matches % 100000) * 2;
        OrderId buy_id = base_id + (matches % 100000) * 2 + 1;
        ob.add_order(sell_id, OrderType::Limit, 5000 + static_cast<Price>(matches % 10000), 10, Side::Sell, [](const Trade&){});
        ob.add_order(buy_id, OrderType::Limit, 15000, 10, Side::Buy, [](const Trade& t){
            benchmark::DoNotOptimize(t);
        });
        matches++;
    }
    state.counters["ops"] = benchmark::Counter(matches * 2, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_WideSpreadMatching, OrderBook);
BENCHMARK_TEMPLATE(BM_WideSpreadMatching, StdOrderBook);

// ============================================================
// 9. Mixed Workload — realistic 50/20/30 insert/match/cancel
// ============================================================
template <class BookType>
static void BM_MixedWorkload(benchmark::State& state) {
    BookType ob(10000000, 10000000, 0, 5000);
    uint64_t i = 0;
    for (auto _ : state) {
        uint64_t op = i % 10;
        OrderId current_id = 1 + (i % 100000);
        if (op < 5) {
            // Insert passive
            ob.add_order(current_id, OrderType::Limit, 1000 + (i % 200), 10, Side::Sell, [](const Trade& t){
                benchmark::DoNotOptimize(t);
            });
        } else if (op < 7) {
            // Aggressive match
            ob.add_order(current_id, OrderType::Limit, 1200, 10, Side::Buy, [](const Trade& t){
                benchmark::DoNotOptimize(t);
            });
        } else {
            // Cancel a recent order (ensure it's not the one we just added if we can help it, though it might be missing)
            OrderId cancel_id = 1 + ((i + 99995) % 100000);
            ob.cancel_order(cancel_id);
        }
        ++i;
    }
    state.counters["ops"] = benchmark::Counter(i, benchmark::Counter::kIsRate);
}
BENCHMARK_TEMPLATE(BM_MixedWorkload, OrderBook);
BENCHMARK_TEMPLATE(BM_MixedWorkload, StdOrderBook);

BENCHMARK_MAIN();
"""

with open("C:/Users/karya/Codes/hft-matching-engine/tests/benchmark_ob.cpp", "w") as f:
    f.write(bench_content)
