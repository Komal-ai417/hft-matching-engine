#include <benchmark/benchmark.h>
#include "../src/OrderBook.h"

using namespace hft;

static void BM_AddOrder_NoMatch(benchmark::State& state) {
    OrderBook ob(state.max_iterations + 10);
    OrderId id = 1;
    for (auto _ : state) {
        // Just adding to the book without matching (best case insert)
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell);
    }
}
BENCHMARK(BM_AddOrder_NoMatch);

static void BM_Matching(benchmark::State& state) {
    OrderBook ob(state.max_iterations * 2 + 10);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell);
        state.ResumeTiming();
        
        // Immediate match
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Buy);
    }
}
BENCHMARK(BM_Matching);

static void BM_AddAndCancel(benchmark::State& state) {
    OrderBook ob(state.max_iterations + 10);
    OrderId id = 1;
    for (auto _ : state) {
        ob.add_order(id, OrderType::Limit, 100, 10, Side::Sell);
        ob.cancel_order(id);
        id++;
    }
}
BENCHMARK(BM_AddAndCancel);

BENCHMARK_MAIN();
