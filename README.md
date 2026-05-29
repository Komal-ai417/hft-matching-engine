# High-Frequency Trading (HFT) Matching Engine

A blazingly fast Limit Order Book (LOB) matching engine written in modern C++20. Designed with the strict latency constraints of High-Frequency Trading (HFT) and quantitative finance in mind.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![C++](https://img.shields.io/badge/C++-20-blue.svg)
[![C++ CI](https://github.com/Komal-ai417/hft-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Komal-ai417/hft-matching-engine/actions/workflows/ci.yml)

## System Architecture

The matching engine is built to maximize CPU cache hits and minimize operating system interruptions.

```mermaid
graph TD
    subgraph Client Space
        A["Incoming FIX/ITCH Message"]
    end

    subgraph Order Book Engine
        B["Network Ring Buffer / Lock-Free Queue"]
        C{"Order Router"}
        
        subgraph Memory Management
            P[("Pre-Allocated MemoryPool")]
        end
        
        subgraph Data Structures
            M["O(1) Flat Array: OrderId -> Order*"]
            Tree["Flat Array: Price offset -> Bids"]
            Tree2["Flat Array: Price offset -> Asks"]
            
            subgraph Price Level
                L["Intrusive Doubly-Linked List"]
                O1["Order 1"]
                O2["Order 2"]
                O3["Order n"]
                O1 <--> O2
                O2 <--> O3
            end
        end

        C --> |"New Array Pointer"| P
        C --> |"O(1) Lookup/Cancel"| M
        C --> |"Time Priority"| L
        C --> |"Price Priority"| Tree
        C --> |"Price Priority"| Tree2
        Tree --> L
        Tree2 --> L
    end

    A --> B
    B --> C
    
    style P fill:#4CAF50,stroke:#388E3C,stroke-width:2px,color:#fff
    style L fill:#2196F3,stroke:#1976D2,stroke-width:2px,color:#fff
    style M fill:#FF9800,stroke:#F57C00,stroke-width:2px,color:#fff
```

## Performance Metrics

**Google Benchmark** (MSVC, `-O2`, Windows, 12 × 2611 MHz CPU):

| Benchmark | OrderBook CPU (ns) | StdOrderBook CPU (ns) | Speedup Ratio |
| :--- | ---: | ---: | ---: |
| `BM_AddOrder_NoMatch` | **21.8** | 351 | **16.10×** |
| `BM_Matching_DeepBook` | **26.2** | 457 | **17.44×** |
| `BM_BatchMatching` | **93.8** | 1880 | **20.04×** |
| `BM_AddAndCancel` | **16.4** | 265 | **16.16×** |
| `BM_CancelInDeepBook` | **12.1** | 209 | **17.27×** |
| `BM_MarketOrder_DeepBook` | **27.2** | 746 | **27.43×** |
| `BM_SweepMultipleLevels` | **201.0** | 4938 | **24.57×** |
| `BM_WideSpreadMatching` | **26.2** | 505 | **19.27×** |
| `BM_MixedWorkload` | **14.1** | 357 | **25.32×** |

- **Zero-Allocation Stack Matching:** Taker orders match using stack-allocated variables, completely bypassing memory pool overhead for immediate matches to achieve **17.44× speedup** in matching.
- **Bulk Maker Deallocation:** Exited price levels are unlinked in a single $O(1)$ assignment via `deallocate_chain`, pushing batch matches to a **20.04× speedup**.
- **Bound Caching:** Boundary checking calls were optimized by caching capacity scalars, maintaining the high-throughput passive insertion at **21.8 ns/op**.

## Core Design Principles

To achieve sub-microsecond latency, this engine adheres to the following strict C++ design principles:

### 1. Zero Dynamic Allocation on the Critical Path
In C++, `new` and `malloc` require expensive context switches to the Operating System. During active trading hours, we cannot afford this unpredictability. 
- **Solution:** A custom `MemoryPool<Order>` pre-allocates a massive contiguous block of memory on startup `(e.g., 1,000,000 orders)`. The matching engine simply hands out pointers to pre-allocated memory using an intrusive stack-based LIFO free-list embedded directly within unused Orders. Allocation and deallocation are strictly $O(1)$ and never hit the OS.
- **Safety:** An allocation bitmap detects double-free bugs at runtime, preventing the same memory address from being returned to two callers.

### 2. Cache Locality & Intrusive Data Structures
Standard library containers (like `std::list`) allocate individual nodes across the heap, causing severe memory fragmentation and destroying CPU L1/L2 cache coherence via cache misses.
- **Solution:** We explicitly avoid `std::list`. Instead, we use **Intrusive Doubly-Linked Lists**. The `Order` struct itself contains the `next` and `prev` pointers. When an order is added to a `PriceLevel`, we merely update the pointers. This keeps the memory incredibly dense and cache-friendly.
- **Compact Layout:** The `Order` struct is kept at a lean 40 bytes with natural 8-byte alignment, allowing maximum cache density. Since the engine is single-threaded, cache-line alignment (which wastes padding) is intentionally avoided to pack more orders per cache line.

### 3. Price-Time Priority Matching
Orders are matched primarily on Price (highest bid vs lowest ask), and secondarily on Time (First-In, First-Out).
- **Price tracking:** Flat Arrays (`std::vector`) pre-allocated up to a maximum tick price. These arrays completely eradicate $O(\log N)$ tree traversal pointer chasing, making matching contiguous and blazing fast.
- **Time tracking:** The intrusive linked list anchored within each `PriceLevel`.
- **$O(1)$ Cancellations:** A flat array `std::vector<Order*>` provides instant array lookup to cancel an order by simply unlinking its pointers from the `PriceLevel` without any branching or hashing overhead.

### 4. Structured Results & Input Validation
- Validation errors (duplicate IDs, out-of-bounds prices, zero quantity) cause early returns on the hot-path, avoiding exception overhead entirely.
- **Trade Callbacks** provide "zero-copy" execution by passing matched `Trade` structs directly to caller-defined lambdas without ever pushing to a `std::vector` heap buffer.
- **Duplicate order IDs** are rejected before allocation, preventing memory pool leaks and dangling pointer corruption.

## API

```cpp
#include "OrderBook.h"
#include <iostream>

// Pre-allocate for 1M resting orders, and set price bounds (min_price=0, max_price=20000)
hft::OrderBook ob(1'000'000, 0, 20000);

// Submit a limit order with a callback lambda for trades
ob.add_order(
    1, hft::OrderType::Limit, 10050, 100, hft::Side::Buy,
    [](const hft::Trade& trade) {
        std::cout << "Trade! " << trade.quantity << " @ " << trade.price << "\n";
    }
);

// Cancel an order
ob.cancel_order(1);
```

## Build Instructions

Dependencies (GoogleTest, Google Benchmark) are automatically downloaded via CMake FetchContent — no manual installation required.

### Prerequisites
- GCC / G++ (or Clang) with C++20 support
- CMake ≥ 3.14

### Building (Release)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j$(nproc)
```

### Building (Debug with Sanitizers)

Debug builds automatically enable AddressSanitizer and UndefinedBehaviorSanitizer to catch memory bugs:

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --config Debug -j$(nproc)
```

### Running

```bash
# Custom benchmark (4 separate workloads)
./build/hft_engine

# Google Test suite (30 test cases)
./build/hft_tests

# Google Benchmark (microbenchmarks)
./build/hft_bench
```

## Example Output

```text
=== HFT Matching Engine Benchmark ===

[0] Warming up the engine (I-Cache and Branch Predictors)...
    Warmup complete.

[1] Pure Insertion: 1000000 passive sell orders...
    Inserted 1000000 orders in 10920 us
    Avg latency: 10.9 ns/op

[2] Pure Matching: 100000 aggressive buy orders against 100000 resting sells...
    Matched 100000 orders in 1443 us
    Avg latency: 14.4 ns/op

[3] Pure Cancellation: 500000 cancel operations...
    Cancelled 500000 orders in 4641 us
    Avg latency: 9.3 ns/op

[4] Mixed Workload: 1000000 ops (70% insert, 20% match, 10% cancel)...
    Processed 1000000 ops in 8150 us
    Avg latency: 8.2 ns/op

Engine run complete. Built for microsecond latency.
```

### Google Benchmark Output (`./hft_bench`)

```text
Running hft-matching-engine\build\hft_bench.exe
Run on (12 X 2611 MHz CPU s)
CPU Caches:
  L1 Data 48 KiB (x6)
  L1 Instruction 32 KiB (x6)
  L2 Unified 1280 KiB (x6)
  L3 Unified 12288 KiB (x1)
--------------------------------------------------------------------------------
Benchmark                                      Time             CPU   Iterations
--------------------------------------------------------------------------------
BM_AddOrder_NoMatch<OrderBook>              19.3 ns         17.7 ns    128000000
BM_AddOrder_NoMatch<StdOrderBook>            411 ns          402 ns      3733333
BM_Matching_DeepBook<OrderBook>              147 ns          137 ns     22974359
BM_Matching_DeepBook<StdOrderBook>           772 ns          748 ns      4072727
BM_BatchMatching<OrderBook>                  454 ns          446 ns      2800000
BM_BatchMatching<StdOrderBook>              2339 ns         2086 ns       689231
BM_AddAndCancel<OrderBook>                  22.1 ns         20.3 ns     64000000
BM_AddAndCancel<StdOrderBook>                339 ns          317 ns      3895652
BM_CancelInDeepBook<OrderBook>              12.7 ns         12.3 ns    128000000
BM_CancelInDeepBook<StdOrderBook>            247 ns          234 ns      5600000
BM_MarketOrder_DeepBook<OrderBook>           135 ns          126 ns     22400000
BM_MarketOrder_DeepBook<StdOrderBook>        728 ns          714 ns      4072727
BM_SweepMultipleLevels<OrderBook>            929 ns          866 ns      1659259
BM_SweepMultipleLevels<StdOrderBook>        7209 ns         7677 ns       248320
BM_WideSpreadMatching<OrderBook>             146 ns          136 ns     33185185
BM_WideSpreadMatching<StdOrderBook>          608 ns          590 ns      4977778
BM_MixedWorkload<OrderBook>                 28.7 ns         24.0 ns    112000000
BM_MixedWorkload<StdOrderBook>               569 ns          558 ns      2297436
```

## Architectural Isolation (Production Environments)

In a true high-frequency trading production environment running on Linux, OS scheduling and networking interrupts represent massive latency spikes. 

To achieve consistent hardware-sympathetic execution, the following techniques are typically applied to this engine:
1. **Thread Pinning**: Using `pthread_setaffinity_np()` to lock the matching thread to a specific CPU core.
2. **CPU Isolation**: Configuring the kernel boot parameter `isolcpus` to hide specific cores from the Linux OS scheduler so background processes (cron jobs, SSH) do not steal CPU time from the matching engine.
3. **Kernel Bypass**: Utilizing specialized network interface cards (like Solarflare/Xilinx with OpenOnload or DPDK) to move network packets directly from the NIC hardware straight into user-space memory, completely skipping the Linux kernel networking stack.
4. **Jitter Management (Memory Locking)**: Using `mlockall(MCL_CURRENT | MCL_FUTURE)` to prevent the Linux kernel from swapping the engine's memory to disk, which would cause catastrophic latency spikes due to page faults.
5. **I-Cache Management**: While `template <Side>` creates incredible pipeline efficiency, adding overly complex Trade callbacks into the hot-loop can blow out the 32KB L1 Instruction Cache. Ensure the callback remains extremely lean, or explicitly mark the callback as `[[noinline]]` to protect the core matching loop.

## Documentation

Comprehensive technical documentation is available in the [docs/](docs/) directory:

| Document | Description |
| :--- | :--- |
| [Hardware Sympathy & Profiling](docs/1_Hardware_Sympathy_And_Profiling.md) | Cache miss analysis, IPC profiling, bitset operation costs, and environment specifications. |
| [Architecture & Memory Layout](docs/2_Architecture_And_Memory_Layout.md) | Deep-dive into `MemoryPool` mechanics, intrusive linked lists, data structure justifications with $O(1)$ complexity proofs. |
| [Test Plan & Correctness Matrix](docs/3_Test_Plan_And_Correctness_Matrix.md) | Formal test matrix covering priority logic, boundary inputs, memory safety, and sanitizer CI integration. |
| [API Reference](docs/4_API_Reference.md) | Complete reference for all public types, methods, return states, memory layouts, and thread-safety contracts. |

## Future Enhancements
1. Adding SPSC (Single-Producer Single-Consumer) Lock-Free Queues to receive network packets from a separate I/O network thread.
2. Implementing Self-Trade Prevention (STP) for regulatory compliance.
3. Adding transactional exception safety guards around the matching loop.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
