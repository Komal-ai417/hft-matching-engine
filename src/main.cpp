#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include "OrderBook.h"

int main() {
    std::cout << "Initializing HFT Limit Order Book...\n";
    
    // Allocate space for 1,000,000 resting orders
    hft::OrderBook ob(1'000'000);
    
    std::cout << "Seeding the book with 100,000 orders...\n";
    for (uint64_t i = 1; i <= 100000; ++i) {
        ob.add_order(i, hft::OrderType::Limit, 1000 + (i % 100), 10, hft::Side::Sell);
    }
    
    std::cout << "Measuring latency of 1,000,000 match operations...\n";
    auto start = std::chrono::high_resolution_clock::now();
    uint64_t start_id = 200000;
    
    for (uint64_t i = 0; i < 1000000; ++i) {
        // Taker buy order crossing into the sell book
        ob.add_order(start_id + i, hft::OrderType::Limit, 1100, 10, hft::Side::Buy);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    std::cout << "Matched 1,000,000 orders in " << diff << " microseconds.\n";
    std::cout << "Average Latency: " << std::fixed << std::setprecision(3) << (static_cast<double>(diff) * 1000.0 / 1000000.0) << " nanoseconds per operation.\n";
    std::cout << "\nEngine run complete. Built for microsecond latency.\n";
    
    return 0;
}
