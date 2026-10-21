// main.cpp — the hft_engine smoke/benchmark binary.
//
// ============================================================================
// MEMORY-LOCKING POLICY (audit C-1 + Round-4 self-heal)
// ----------------------------------------------------------------------------
//  1. HFT_DISABLE_MLOCKALL=1  -> skip locking entirely (pageable).
//  2. HFT_MADVISE_ONLY=1      -> skip mlockall; pools still pre-fault via
//                                MADV_POPULATE_WRITE (exercises the madvise
//                                pre-touch path on hosts that support it).
//  3. Otherwise: raise RLIMIT_MEMLOCK (best effort; a warning is non-fatal),
//     mlockall(MCL_CURRENT|MCL_FUTURE), then PROBE one fresh anonymous page.
//     Some runtimes (gVisor, certain sandboxes/containers) report mlockall
//     success but fail every future locked mapping with ENOMEM — historically
//     that aborted the engine mid-startup (bad_alloc -> terminate under
//     -fno-exceptions). The probe detects the condition up front; the engine
//     then munlockall()s and self-heals to pageable memory with a warning
//     instead of dying. Reproduce anytime with:
//         LD_PRELOAD=tools/mlock_gvisor_sim.so ./hft_engine
// ============================================================================

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>

#include "OrderBook.h"

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/resource.h>

#include <cerrno>
#endif

namespace {

// ── Demo workload tuning — every number is named (zero-magic-number policy) ──
constexpr std::uint64_t kWarmupOps = 100'000;         // I-cache/branch warmup
constexpr std::uint64_t kInsertOps = 1'000'000;       // benchmark 1
constexpr std::uint64_t kMatchSeedOrders = 100'000;   // benchmark 2 resting
constexpr std::uint64_t kMatchTakerOrders = 100'000;  // benchmark 2 aggressive
constexpr std::uint64_t kCancelOps = 500'000;         // benchmark 3
constexpr std::uint64_t kMixedOps = 1'000'000;        // benchmark 4
constexpr std::uint64_t kIdHeadroom = 10;             // pool headroom past the op count
constexpr std::uint64_t kMixCycleLen = 10;            // op-mix period
constexpr std::uint64_t kMixInsertCutoff = 7;         // op < 7  -> insert
constexpr std::uint64_t kMixMatchCutoff = 9;          // op < 9  -> match
constexpr std::uint64_t kMixCancelLookback = 5;       // cancel a recent id
constexpr double kNanosPerMicro = 1000.0;             // us -> ns scale
constexpr std::size_t kProbePageBytes = 4096;         // locked-alloc probe page

constexpr hft::Price kWarmupMaxPrice = 20'000;  // wide warmup book
constexpr hft::Price kMaxBookPrice = 2000;      // benchmarks 1-4
constexpr hft::Price kBasePrice = 1000;         // lowest seeded price
constexpr hft::Price kWarmupSpread = 1000;      // warmup / bench-1 spread
constexpr hft::Price kSeedSpreadNarrow = 100;   // bench-2 resting spread
constexpr hft::Price kSeedSpreadWide = 500;     // bench-3 resting spread
constexpr hft::Price kTakerPrice = 1100;        // bench-2 aggressive price
constexpr hft::Price kMixSpread = 200;          // bench-4 insert spread
constexpr hft::Price kMixTakerPrice = 1200;     // bench-4 aggressive price
constexpr hft::Quantity kDemoQty = 10;          // every demo order's size

/// Outcome of the startup memory-lock negotiation.
enum class LockMode : std::uint8_t { Locked, Disabled, MadviseOnly, Pageable, Reverted };

/// One fresh anonymous page: under MCL_FUTURE, a runtime whose locked
/// allocations fail (gVisor-style) rejects the mapping with ENOMEM. On a
/// healthy host the page maps, locks, takes a write and releases cleanly.
bool locked_alloc_probe_ok() noexcept {
#if defined(__linux__)
    void* page = mmap(nullptr, kProbePageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        return false;
    }
    if (mlock(page, kProbePageBytes) != 0) {
        munmap(page, kProbePageBytes);
        return false;
    }
    *static_cast<volatile unsigned char*>(page) = 1;  // fault it while locked
    munlock(page, kProbePageBytes);
    munmap(page, kProbePageBytes);
    return true;
#else
    return false;
#endif
}

LockMode prepare_memory_locking() noexcept {
#if defined(__linux__)
    if (std::getenv("HFT_DISABLE_MLOCKALL") != nullptr) {
        return LockMode::Disabled;
    }
    if (std::getenv("HFT_MADVISE_ONLY") != nullptr) {
        return LockMode::MadviseOnly;
    }

    struct rlimit rl{};
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
        // Unprivileged without CAP_IPC_LOCK: expected on most hosts. Not fatal.
        std::fprintf(stderr, "[hft] setrlimit(RLIMIT_MEMLOCK) failed (%s) — pools will be pageable, not locked\n",
                     std::strerror(errno));
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[hft] mlockall failed (%s) — running unlocked; tail latency may see page faults\n",
                     std::strerror(errno));
        return LockMode::Pageable;
    }

    if (!locked_alloc_probe_ok()) {
        // gVisor-style runtime: mlockall "succeeded" but locked allocations
        // fail. Self-heal: drop MCL_FUTURE before the pools reserve their
        // storage, then continue pageable (Round-4 fix — the old behavior
        // was bad_alloc -> std::terminate under -fno-exceptions).
        munlockall();
        std::fprintf(stderr,
                     "[hft] mlockall succeeded but locked allocations fail on this runtime "
                     "(container/sandbox?) — reverting to pageable memory\n");
        return LockMode::Reverted;
    }
    return LockMode::Locked;
#else
    return LockMode::Pageable;
#endif
}

void print_lock_recap(LockMode mode) noexcept {
    switch (mode) {
        case LockMode::Locked:
            std::cout << "[*] Memory locked successfully (mlockall).\n\n";
            break;
        case LockMode::Disabled:
            std::cout << "[*] mlockall skipped (HFT_DISABLE_MLOCKALL set) — memory is pageable.\n\n";
            break;
        case LockMode::MadviseOnly:
            std::cout << "[*] madvise-only mode (HFT_MADVISE_ONLY set) — mlockall skipped, "
                         "pools pre-faulted via MADV_POPULATE_WRITE.\n\n";
            break;
        case LockMode::Pageable:
            std::cout << "[*] mlockall unavailable on this host — memory is pageable.\n\n";
            break;
        case LockMode::Reverted:
            std::cout << "[*] Memory is pageable (locked-allocation probe failed; see warning above).\n\n";
            break;
    }
}

template<std::uint64_t OpCount>
void report_timing(std::uint64_t microseconds) {
    std::cout << "    Avg latency: " << std::fixed << std::setprecision(1)
              << (static_cast<double>(microseconds) * kNanosPerMicro / static_cast<double>(OpCount)) << " ns/op\n\n";
}

}  // namespace

int main() {
    const LockMode lock_mode = prepare_memory_locking();

    std::cout << "=== HFT Matching Engine Benchmark ===\n\n";
    print_lock_recap(lock_mode);

    // -------------------------------------------------------
    // Benchmark 0: Warmup
    // -------------------------------------------------------
    {
        std::cout << "[0] Warming up the engine (I-Cache and Branch Predictors)...\n";
        hft::OrderBook warmup(kWarmupOps + kIdHeadroom, 0, kWarmupMaxPrice);
        for (std::uint64_t i = 1; i <= kWarmupOps; ++i) {
            (void)warmup.add_order(i, hft::OrderType::Limit, kBasePrice + (i % kWarmupSpread), kDemoQty,
                                   hft::Side::Sell, [](const auto&) noexcept {});
            (void)warmup.add_order(i, hft::OrderType::Cancel, 0, 0, hft::Side::Buy, [](const auto&) noexcept {});
        }
        std::cout << "    Warmup complete.\n\n";
    }

    // -------------------------------------------------------
    // Benchmark 1: Pure Insertion (no matching)
    // NOTE (audit L-10): all orders rest across kWarmupSpread price points —
    // this measures the allocator + level-append path on a wide-but-shallow
    // book shape, NOT a realistic market-mixed book. See hft_bench.
    // -------------------------------------------------------
    {
        hft::OrderBook ob(kInsertOps + kIdHeadroom, 0, kMaxBookPrice);

        std::cout << "[1] Pure Insertion: " << kInsertOps << " passive sell orders...\n";
        const auto start = std::chrono::high_resolution_clock::now();

        for (std::uint64_t i = 1; i <= kInsertOps; ++i) {
            (void)ob.add_order(i, hft::OrderType::Limit, kBasePrice + (i % kWarmupSpread), kDemoQty, hft::Side::Sell,
                               [](const auto&) noexcept {});
        }

        const auto end = std::chrono::high_resolution_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "    Inserted " << kInsertOps << " orders in " << us << " us\n";
        report_timing<kInsertOps>(static_cast<std::uint64_t>(us));
    }

    // -------------------------------------------------------
    // Benchmark 2: Pure Matching (1:1 taker vs resting)
    // -------------------------------------------------------
    {
        hft::OrderBook ob(kMatchSeedOrders + kMatchTakerOrders + kIdHeadroom, 0, kMaxBookPrice);

        // Seed the book
        for (std::uint64_t i = 1; i <= kMatchSeedOrders; ++i) {
            (void)ob.add_order(i, hft::OrderType::Limit, kBasePrice + (i % kSeedSpreadNarrow), kDemoQty,
                               hft::Side::Sell, [](const auto&) noexcept {});
        }

        std::cout << "[2] Pure Matching: " << kMatchTakerOrders << " aggressive buy orders against " << kMatchSeedOrders
                  << " resting sells...\n";
        const auto start = std::chrono::high_resolution_clock::now();

        const std::uint64_t start_id = kMatchSeedOrders + 1;
        for (std::uint64_t i = 0; i < kMatchTakerOrders; ++i) {
            (void)ob.add_order(start_id + i, hft::OrderType::Limit, kTakerPrice, kDemoQty, hft::Side::Buy,
                               [](const auto&) noexcept {});
        }

        const auto end = std::chrono::high_resolution_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "    Matched " << kMatchTakerOrders << " orders in " << us << " us\n";
        report_timing<kMatchTakerOrders>(static_cast<std::uint64_t>(us));
    }

    // -------------------------------------------------------
    // Benchmark 3: Pure Cancellation
    // -------------------------------------------------------
    {
        hft::OrderBook ob(kCancelOps + kIdHeadroom, 0, kMaxBookPrice);

        // Seed
        for (std::uint64_t i = 1; i <= kCancelOps; ++i) {
            (void)ob.add_order(i, hft::OrderType::Limit, kBasePrice + (i % kSeedSpreadWide), kDemoQty, hft::Side::Sell,
                               [](const auto&) noexcept {});
        }

        std::cout << "[3] Pure Cancellation: " << kCancelOps << " cancel operations...\n";
        const auto start = std::chrono::high_resolution_clock::now();

        for (std::uint64_t i = 1; i <= kCancelOps; ++i) {
            (void)ob.add_order(i, hft::OrderType::Cancel, 0, 0, hft::Side::Buy, [](const auto&) noexcept {});
        }

        const auto end = std::chrono::high_resolution_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "    Cancelled " << kCancelOps << " orders in " << us << " us\n";
        report_timing<kCancelOps>(static_cast<std::uint64_t>(us));
    }

    // -------------------------------------------------------
    // Benchmark 4: Mixed Workload (realistic)
    // -------------------------------------------------------
    {
        hft::OrderBook ob(kMixedOps + kIdHeadroom, 0, kMaxBookPrice);

        std::cout << "[4] Mixed Workload: " << kMixedOps << " ops (70% insert, 20% match, 10% cancel)...\n";
        const auto start = std::chrono::high_resolution_clock::now();

        std::uint64_t id = 1;
        for (std::uint64_t i = 0; i < kMixedOps; ++i) {
            const std::uint64_t op = i % kMixCycleLen;
            if (op < kMixInsertCutoff) {
                // Insert passive
                (void)ob.add_order(id++, hft::OrderType::Limit, kBasePrice + (i % kMixSpread), kDemoQty,
                                   hft::Side::Sell, [](const auto&) noexcept {});
            } else if (op < kMixMatchCutoff) {
                // Aggressive match
                (void)ob.add_order(id++, hft::OrderType::Limit, kMixTakerPrice, kDemoQty, hft::Side::Buy,
                                   [](const auto&) noexcept {});
            } else {
                // Cancel a recent order
                const std::uint64_t cancel_id = (id > kMixCancelLookback) ? id - kMixCancelLookback : 1;
                (void)ob.add_order(cancel_id, hft::OrderType::Cancel, 0, 0, hft::Side::Buy,
                                   [](const auto&) noexcept {});
            }
        }

        const auto end = std::chrono::high_resolution_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "    Processed " << kMixedOps << " ops in " << us << " us\n";
        report_timing<kMixedOps>(static_cast<std::uint64_t>(us));
    }

    std::cout << "Engine run complete. Built for microsecond latency.\n";
    return 0;
}
