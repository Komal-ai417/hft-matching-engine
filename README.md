# High-Frequency Trading (HFT) Matching Engine

A blazingly fast Limit Order Book (LOB) matching engine written in modern C++. Designed with the strict latency constraints of High-Frequency Trading (HFT) and quantitative finance in mind.

## Performance Metrics

- **Average Match Latency:** `~229 nanoseconds`
- **Total Throughput:** `~4.3+ million operations per second` 
*(Measured on a standard consumer CPU utilizing GCC 6.3 Windows)*

## Core Design Principles

To achieve sub-microsecond latency, this engine adheres to the following strict C++ design principles:

### 1. Zero Dynamic Allocation on the Critical Path
In C++, `new` and `malloc` require expensive context switches to the Operating System. During active trading hours, we cannot afford this. 
- **Solution:** A custom `MemoryPool<Order>` pre-allocates a massive contiguous block of memory on startup `(e.g., 1,000,000 orders)`. The matching engine simply hands out pointers to pre-allocated memory using a stack-based free-list. Allocation and deallocation are $O(1)$ and never hit the OS.

### 2. Cache Locality & Intrusive Data Structures
Standard library containers (like `std::list`) allocate individual nodes on the heap, causing severe memory fragmentation and destroying CPU cache coherence.
- **Solution:** We use **Intrusive Doubly-Linked Lists**. The `Order` struct itself contains the `next` and `prev` pointers. When an order is added to a `PriceLevel`, we just update the pointers. This keeps the memory dense and cache-friendly.

### 3. Price-Time Priority
Orders are matched primarily on Price (highest bid vs lowest ask), and secondarily on Time (First-In, First-Out).
- **Price tracking:** `std::map` (ordered by `<Price, PriceLevel>`).
- **Time tracking:** The intrusive linked list within each `PriceLevel`.
- **$O(1)$ Cancellations:** An `std::unordered_map<OrderId, Order*>` provides instant lookup to cancel an order by simply unlinking its pointers from the `PriceLevel`.

## Build Instructions

Because we are optimizing for raw speed, we compile directly using `g++` with the `-O3` flag.

### Prerequisites
- GCC / G++ (or Clang)

### Building the Engine
Open your terminal and navigate to the project directory, then run:

```bash
g++ -std=c++14 -O3 src\main.cpp src\OrderBook.cpp -o hft_engine.exe
```

### Running the Benchmark
```bash
.\hft_engine.exe
```

## Example Output

```text
Initializing HFT Limit Order Book...
Seeding the book with 100,000 orders...
Measuring latency of 1,000,000 match operations...
Matched 1,000,000 orders in 229233 microseconds.
Average Latency: 229.233 nanoseconds per operation.

Engine run complete. Built for microsecond latency.
```

## About the implementation

This project represents the core "matching loop" used by exchanges (like NASDAQ or Binance) to process raw FIX or ITCH protocol feeds. The next steps for scaling this into production would be:
1. Adding lock-free queues (like SPSC rings) to receive network packets from a separate I/O thread.
2. Replacing `std::map` with an array-backed flat map to eliminate tree-traversal pointer chasing for dense price increments.
