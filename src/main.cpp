#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include "OrderBook.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

int main() {
    std::cout << "=== HFT Matching Engine Benchmark ===\n\n";

#if defined(__linux__)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::cerr << "Warning: Failed to lock memory. Tail latency may spike due to page faults.\n";
    } else {
        std::cout << "[*] Memory locked successfully (mlockall).\n";
    }
#endif

    // -------------------------------------------------------
    // Benchmark 0: Warmup
    // -------------------------------------------------------
    {
        std::cout << "[0] Warming up the engine (I-Cache and Branch Predictors)...\n";
        constexpr uint64_t W = 100'000;
        hft::OrderBook warmup(W + 10, 0, 20000);
        for (uint64_t i = 1; i <= W; ++i) {
            warmup.add_order(i, hft::OrderType::Limit, 1000 + (i % 1000), 10, hft::Side::Sell, [](const auto&) {});
            warmup.add_order(i, hft::OrderType::Cancel, 0, 0, hft::Side::Buy, [](const auto&) {});
        }
        std::cout << "    Warmup complete.\n\n";
    }
    
    // -------------------------------------------------------
    // Benchmark 1: Pure Insertion (no matching)
    // -------------------------------------------------------
    {
        constexpr uint64_t N = 1'000'000;
        hft::OrderBook ob(N + 10, 0, 2000);

        std::cout << "[1] Pure Insertion: " << N << " passive sell orders...\n";
        auto start = std::chrono::high_resolution_clock::now();

        for (uint64_t i = 1; i <= N; ++i) {
            ob.add_order(i, hft::OrderType::Limit, 1000 + (i % 1000), 10, hft::Side::Sell, [](const auto&){});
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "    Inserted " << N << " orders in " << us << " us\n";
        std::cout << "    Avg latency: " << std::fixed << std::setprecision(1)
                  << (static_cast<double>(us) * 1000.0 / static_cast<double>(N)) << " ns/op\n\n";
    }

    // -------------------------------------------------------
    // Benchmark 2: Pure Matching (1:1 taker vs resting)
    // -------------------------------------------------------
    {
        constexpr uint64_t SEED = 100'000;
        constexpr uint64_t MATCH = 100'000;
        hft::OrderBook ob(SEED + MATCH + 10, 0, 2000);

        // Seed the book
        for (uint64_t i = 1; i <= SEED; ++i) {
            ob.add_order(i, hft::OrderType::Limit, 1000 + (i % 100), 10, hft::Side::Sell, [](const auto&){});
        }

        std::cout << "[2] Pure Matching: " << MATCH << " aggressive buy orders against " << SEED << " resting sells...\n";
        auto start = std::chrono::high_resolution_clock::now();

        uint64_t start_id = SEED + 1;
        for (uint64_t i = 0; i < MATCH; ++i) {
            ob.add_order(start_id + i, hft::OrderType::Limit, 1100, 10, hft::Side::Buy, [](const auto&){});
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "    Matched " << MATCH << " orders in " << us << " us\n";
        std::cout << "    Avg latency: " << std::fixed << std::setprecision(1)
                  << (static_cast<double>(us) * 1000.0 / static_cast<double>(MATCH)) << " ns/op\n\n";
    }

    // -------------------------------------------------------
    // Benchmark 3: Pure Cancellation
    // -------------------------------------------------------
    {
        constexpr uint64_t N = 500'000;
        hft::OrderBook ob(N + 10, 0, 2000);

        // Seed
        for (uint64_t i = 1; i <= N; ++i) {
            ob.add_order(i, hft::OrderType::Limit, 1000 + (i % 500), 10, hft::Side::Sell, [](const auto&){});
        }

        std::cout << "[3] Pure Cancellation: " << N << " cancel operations...\n";
        auto start = std::chrono::high_resolution_clock::now();

        for (uint64_t i = 1; i <= N; ++i) {
            ob.add_order(i, hft::OrderType::Cancel, 0, 0, hft::Side::Buy, [](const auto&){});
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "    Cancelled " << N << " orders in " << us << " us\n";
        std::cout << "    Avg latency: " << std::fixed << std::setprecision(1)
                  << (static_cast<double>(us) * 1000.0 / static_cast<double>(N)) << " ns/op\n\n";
    }

    // -------------------------------------------------------
    // Benchmark 4: Mixed Workload (realistic)
    // -------------------------------------------------------
    {
        constexpr uint64_t N = 1'000'000;
        hft::OrderBook ob(N + 10, 0, 2000);

        std::cout << "[4] Mixed Workload: " << N << " ops (70% insert, 20% match, 10% cancel)...\n";
        auto start = std::chrono::high_resolution_clock::now();

        uint64_t id = 1;
        for (uint64_t i = 0; i < N; ++i) {
            uint64_t op = i % 10;
            if (op < 7) {
                // Insert passive
                ob.add_order(id++, hft::OrderType::Limit, 1000 + (i % 200), 10, hft::Side::Sell, [](const auto&){});
            } else if (op < 9) {
                // Aggressive match
                ob.add_order(id++, hft::OrderType::Limit, 1200, 10, hft::Side::Buy, [](const auto&){});
            } else {
                // Cancel a recent order
                uint64_t cancel_id = (id > 5) ? id - 5 : 1;
                ob.add_order(cancel_id, hft::OrderType::Cancel, 0, 0, hft::Side::Buy, [](const auto&){});
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "    Processed " << N << " ops in " << us << " us\n";
        std::cout << "    Avg latency: " << std::fixed << std::setprecision(1)
                  << (static_cast<double>(us) * 1000.0 / static_cast<double>(N)) << " ns/op\n\n";
    }

    std::cout << "Engine run complete. Built for microsecond latency.\n";
    return 0;
}
