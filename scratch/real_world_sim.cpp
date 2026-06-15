#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "../src/OrderBook.h"
#include "../tests/StdOrderBook.h"

using namespace hft;

// A simple deterministic random number generator for reproducible events
struct FastRNG {
    uint64_t state = 123456789;
    uint32_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<uint32_t>(state * 0x2545F4914F6CDD1DULL >> 32);
    }
    uint32_t next(uint32_t max) { return next() % max; }
};

struct Event {
    enum Type { MM_Quote, Retail_Limit, Institutional_Sweep };
    Type type;
    OrderId id;
    Side side;
    Price price;
    Quantity qty;
    OrderId cancel_id;
};

size_t getPeakMemoryUsage() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.PeakWorkingSetSize;
    }
#endif
    return 0;
}

size_t getCurrentMemoryUsage() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
#endif
    return 0;
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "      HFT Matching Engine - Real World Simulation       \n";
    std::cout << "========================================================\n";

    constexpr size_t NUM_EVENTS = 5'000'000;
    constexpr Price MIN_PRICE = 0;
    constexpr Price MAX_PRICE = 20000;
    
    std::cout << "[*] Generating " << NUM_EVENTS << " realistic trading events...\n";
    
    std::vector<Event> events;
    events.reserve(NUM_EVENTS);
    
    FastRNG rng;
    Price mid_price = 10000;
    OrderId next_id = 1;
    
    // Track active MM orders to cancel them
    std::vector<OrderId> active_mm_bids;
    std::vector<OrderId> active_mm_asks;
    
    for (size_t i = 0; i < NUM_EVENTS; ++i) {
        uint32_t roll = rng.next(100);
        
        // Random walk the mid price slightly
        if (rng.next(10) == 0) {
            mid_price += (rng.next(2) == 0) ? 1 : -1;
            if (mid_price < 100) mid_price = 100;
            if (mid_price > 19900) mid_price = 19900;
        }

        if (roll < 80) { // 80% - Market Maker Updates Quotes
            Side side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            OrderId cancel_id = 0;
            
            if (side == Side::Buy) {
                if (!active_mm_bids.empty()) {
                    size_t idx = rng.next(active_mm_bids.size());
                    cancel_id = active_mm_bids[idx];
                    active_mm_bids[idx] = active_mm_bids.back();
                    active_mm_bids.pop_back();
                }
                Price p = mid_price - 1 - rng.next(10);
                events.push_back({Event::MM_Quote, next_id, side, p, 100, cancel_id});
                active_mm_bids.push_back(next_id);
            } else {
                if (!active_mm_asks.empty()) {
                    size_t idx = rng.next(active_mm_asks.size());
                    cancel_id = active_mm_asks[idx];
                    active_mm_asks[idx] = active_mm_asks.back();
                    active_mm_asks.pop_back();
                }
                Price p = mid_price + 1 + rng.next(10);
                events.push_back({Event::MM_Quote, next_id, side, p, 100, cancel_id});
                active_mm_asks.push_back(next_id);
            }
            next_id++;
        } 
        else if (roll < 95) { // 15% - Retail Limit Order
            Side side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            Price p = (side == Side::Buy) ? (mid_price - rng.next(5)) : (mid_price + rng.next(5));
            events.push_back({Event::Retail_Limit, next_id++, side, p, 10, 0});
        }
        else { // 5% - Institutional Market Sweep (Toxicity)
            Side side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            Quantity massive_qty = 500 + rng.next(1000); // Will sweep multiple levels
            events.push_back({Event::Institutional_Sweep, next_id++, side, 0, massive_qty, 0});
        }
    }
    
    std::cout << "[*] Events generated. Running Simulation...\n\n";
    size_t base_memory = getCurrentMemoryUsage();

    auto run_sim = [&](auto& book, const char* name, size_t& out_mem) {
        uint64_t total_trades = 0;
        uint64_t volume = 0;
        
        auto on_trade = [&](const Trade& t) {
            total_trades++;
            volume += t.quantity;
        };

        auto start = std::chrono::high_resolution_clock::now();
        
        for (const auto& ev : events) {
            if (ev.cancel_id != 0) {
                book->cancel_order(ev.cancel_id);
            }
            
            if (ev.type == Event::MM_Quote || ev.type == Event::Retail_Limit) {
                book->add_order(ev.id, OrderType::Limit, ev.price, ev.qty, ev.side, on_trade);
            } else if (ev.type == Event::Institutional_Sweep) {
                book->add_order(ev.id, OrderType::Market, 0, ev.qty, ev.side, on_trade);
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        
        out_mem = getCurrentMemoryUsage() - base_memory;
        
        std::cout << "--- " << name << " ---\n";
        std::cout << "Time Elapsed  : " << std::fixed << std::setprecision(2) << ms << " ms\n";
        std::cout << "Throughput    : " << (NUM_EVENTS / (ms / 1000.0)) / 1e6 << " Million Ops/sec\n";
        std::cout << "Total Trades  : " << total_trades << "\n";
        std::cout << "Volume Matched: " << volume << "\n";
        std::cout << "Memory Added  : " << out_mem / (1024.0 * 1024.0) << " MB\n\n";
        
        return ms;
    };

    double std_ms = 0;
    size_t std_mem = 0;
    {
        // Run StdOrderBook
        auto std_book = std::make_unique<StdOrderBook>(next_id + 1, 1'000'000, MIN_PRICE, MAX_PRICE);
        std_ms = run_sim(std_book, "Standard std::map OrderBook", std_mem);
    }
    
    // Reset base memory after standard orderbook goes out of scope and memory is freed
    base_memory = getCurrentMemoryUsage();
    
    double custom_ms = 0;
    size_t custom_mem = 0;
    {
        // Run custom OrderBook
        auto custom_book = std::make_unique<OrderBook>(next_id + 1, 1'000'000, MIN_PRICE, MAX_PRICE);
        custom_ms = run_sim(custom_book, "Custom hft::OrderBook", custom_mem);
    }

    std::cout << "========================================================\n";
    std::cout << "TIME DIFFERENCE: The custom OrderBook is " << std::fixed << std::setprecision(2) 
              << (std_ms / custom_ms) << "x FASTER than std::map!\n";
    std::cout << "MEMORY DIFFERENCE: The custom OrderBook used " << std::fixed << std::setprecision(2) 
              << (double)custom_mem / (1024*1024) << " MB vs std::map used " << (double)std_mem / (1024*1024) << " MB.\n";
    if (std_mem > custom_mem && custom_mem > 0) {
        std::cout << "                   (std::map used " << (double)std_mem / custom_mem << "x more memory)\n";
    }
    std::cout << "========================================================\n";

    return 0;
}
