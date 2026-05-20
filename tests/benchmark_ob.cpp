#include <benchmark/benchmark.h>
#include "../src/OrderBook.h"
#include "StdOrderBook.h"

using namespace hft;

template <class BookType>
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    BookType ob(state.max_iterations + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        // Just adding to the book without matching (best case insert)
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, OrderBook);
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, StdOrderBook);

template <class BookType>
static void BM_Matching(benchmark::State& state) {
    BookType ob(state.max_iterations * 2 + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&){});
        state.ResumeTiming();
        
        // Immediate match
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Buy, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_Matching, OrderBook);
BENCHMARK_TEMPLATE(BM_Matching, StdOrderBook);

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

template <class BookType>
static void BM_MarketOrder(benchmark::State& state) {
    BookType ob(state.max_iterations * 2 + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        ob.add_order(id++, OrderType::Limit, 100, 10, Side::Sell, [](const Trade&){});
        state.ResumeTiming();

        ob.add_order(id++, OrderType::Market, 0, 10, Side::Buy, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_MarketOrder, OrderBook);
BENCHMARK_TEMPLATE(BM_MarketOrder, StdOrderBook);

template <class BookType>
static void BM_SweepMultipleLevels(benchmark::State& state) {
    BookType ob(state.max_iterations * 11 + 10, 0, 2000);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        // Seed 10 levels with 1 order each
        for (int lvl = 0; lvl < 10; ++lvl) {
            ob.add_order(id++, OrderType::Limit, 100 + lvl, 10, Side::Sell, [](const Trade&){});
        }
        state.ResumeTiming();

        // Sweep all 10 levels
        ob.add_order(id++, OrderType::Limit, 200, 100, Side::Buy, [](const Trade&){});
    }
}
BENCHMARK_TEMPLATE(BM_SweepMultipleLevels, OrderBook);
BENCHMARK_TEMPLATE(BM_SweepMultipleLevels, StdOrderBook);

BENCHMARK_MAIN();
