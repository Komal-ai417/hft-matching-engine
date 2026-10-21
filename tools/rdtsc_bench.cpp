// ============================================================================
// rdtsc_bench.cpp — standalone cycle-accurate latency harness (no deps).
//
// Measures per-operation latency in CPU cycles via RDTSCP (serializing) and
// reports p50 / p90 / p99 / p99.9 / max percentiles plus ops/sec. Scenarios
// cover the four engine paths plus a "churn" fragmentation case. Designed
// to be run:
//   * pinned to an isolated core (taskset), and
//   * with Turbo/SpeedStep disabled if absolute numbers matter.
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

#include "../src/OrderBook.h"

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <x86intrin.h>
static inline uint64_t rdtscp_begin() {
    unsigned aux;
    return __rdtscp(&aux);
}
static inline uint64_t rdtscp_end() {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    return t;
}
static bool estimate_tsc_hz(double& out_hz) {
    out_hz = 0;
    // Calibrate TSC against the steady clock over ~50ms.
    unsigned aux;
    uint64_t t0 = __rdtscp(&aux);
    auto c0 = std::chrono::steady_clock::now();
    for (;;) {
        auto d = std::chrono::steady_clock::now() - c0;
        if (d >= std::chrono::milliseconds(50)) {
            uint64_t t1 = __rdtscp(&aux);
            double secs = std::chrono::duration<double>(d).count();
            out_hz = static_cast<double>(t1 - t0) / secs;
            return true;
        }
    }
}
#else
static uint64_t g_fake_tsc = 0;
static inline uint64_t rdtscp_begin() {
    return ++g_fake_tsc;
}
static inline uint64_t rdtscp_end() {
    return g_fake_tsc += 100;
}  // ~100 "cycles"/op fallback
static bool estimate_tsc_hz(double& out_hz) {
    out_hz = 1.0;
    return false;
}
#endif

using namespace hft;

namespace {

struct PercentileReport {
    double p50, p90, p99, p999, max, mean;
};

PercentileReport report(std::vector<uint64_t> cycles) {
    std::sort(cycles.begin(), cycles.end());
    auto pick = [&](double q) -> double {
        if (cycles.empty()) return 0;
        size_t i = static_cast<size_t>(q * static_cast<double>(cycles.size() - 1));
        return static_cast<double>(cycles[i]);
    };
    double sum = std::accumulate(cycles.begin(), cycles.end(), 0.0);
    return {pick(0.50),
            pick(0.90),
            pick(0.99),
            pick(0.999),
            static_cast<double>(cycles.back()),
            sum / static_cast<double>(cycles.size())};
}

void print_report(const char* name, const PercentileReport& r, size_t n, double tsc_hz) {
    std::printf("%-36s n=%-8zu p50=%7.1f p90=%7.1f p99=%7.1f p99.9=%7.1f max=%9.1f mean=%7.1f cycles\n", name, n, r.p50,
                r.p90, r.p99, r.p999, r.max, r.mean);
    if (tsc_hz > 1e9 && r.mean > 0) {
        std::printf("%-36s    | ~%.1f ns/op mean | ~%.2f M ops/sec (TSC %.3f GHz)\n", "", r.mean / (tsc_hz / 1e9),
                    (tsc_hz / 1e9) / r.mean, tsc_hz / 1e9);
    }
}

constexpr Price MIN_P = 0;
constexpr Price MAX_P = 20000;

}  // namespace

int main() {
    std::printf("=== HFT RDTSC Latency Harness ===\n");
    double tsc_hz = 0;
    const bool have_tsc = estimate_tsc_hz(tsc_hz);
    if (have_tsc)
        std::printf("[*] TSC calibrated: %.3f GHz\n", tsc_hz / 1e9);
    else
        std::printf("[*] RDTSCP unavailable — synthetic counter, RELATIVE numbers only\n");

    const auto noop_cb = [](const Trade&) noexcept {};

    // ── 1. Passive insertion ────────────────────────────────────────────
    {
        constexpr uint32_t N = 200'000;
        OrderBook ob(N + 10, MIN_P, MAX_P);
        std::vector<uint64_t> cyc;
        cyc.reserve(N);
        for (uint32_t i = 1; i <= 10'000; ++i) {  // warmup
            (void)ob.add_order(i, OrderType::Limit, 1000 + (i % 1000), 10, Side::Sell, noop_cb);
        }
        for (uint32_t i = 10'001; i <= 10'000 + N; ++i) {
            uint64_t t0 = rdtscp_begin();
            (void)ob.add_order(i, OrderType::Limit, 1000 + (i % 1000), 10, Side::Sell, noop_cb);
            cyc.push_back(rdtscp_end() - t0);
        }
        print_report("insert (passive limit)", report(std::move(cyc)), N, tsc_hz);
    }

    // ── 2. Match (aggressive consume, single maker) ─────────────────────
    {
        OrderBook ob(400'010, 400'010, MIN_P, MAX_P);
        for (uint32_t i = 1; i <= 100'000; ++i) {
            (void)ob.add_order(i, OrderType::Limit, 1000 + (i % 1000), 10, Side::Sell, noop_cb);
        }
        std::vector<uint64_t> cyc;
        cyc.reserve(100'000);
        for (uint32_t i = 0; i < 100'000; ++i) {
            OrderId id = 200'001 + i;
            uint64_t t0 = rdtscp_begin();
            (void)ob.add_order(id, OrderType::Limit, 2000, 10, Side::Buy, noop_cb);
            cyc.push_back(rdtscp_end() - t0);
        }
        print_report("match (aggressive limit vs 1 maker)", report(std::move(cyc)), cyc.size(), tsc_hz);
    }

    // ── 3. Sweep (multi-level, 10 levels per taker) ─────────────────────
    {
        OrderBook ob(40'010, 40'010, MIN_P, MAX_P);
        std::vector<uint64_t> cyc;
        cyc.reserve(2'000);
        for (uint32_t round = 0; round < 2'000; ++round) {
            OrderId base = 1 + round * 12;
            for (int k = 0; k < 10; ++k) {
                (void)ob.add_order(base + k, OrderType::Limit, 1000 + k, 5, Side::Sell, noop_cb);
            }
            uint64_t t0 = rdtscp_begin();
            (void)ob.add_order(base + 10, OrderType::Limit, 1200, 50, Side::Buy, noop_cb);
            cyc.push_back(rdtscp_end() - t0);
        }
        print_report("sweep (10 levels per taker)", report(std::move(cyc)), cyc.size(), tsc_hz);
    }

    // ── 4. Cancel ────────────────────────────────────────────────────────
    {
        OrderBook ob(210'010, 210'010, MIN_P, MAX_P);
        for (uint32_t i = 1; i <= 200'000; ++i) {
            (void)ob.add_order(i, OrderType::Limit, 1000 + (i % 1000), 10, Side::Sell, noop_cb);
        }
        std::vector<uint64_t> cyc;
        cyc.reserve(200'000);
        for (uint32_t i = 1; i <= 200'000; ++i) {
            uint64_t t0 = rdtscp_begin();
            (void)ob.cancel_order(i);
            cyc.push_back(rdtscp_end() - t0);
        }
        print_report("cancel (resting order)", report(std::move(cyc)), cyc.size(), tsc_hz);
    }

    // ── 5. Churn fragmentation ─────────────────────────────────────────
    {
        struct FastRng {
            uint64_t s = 0x1234'5678'9ABC'DEF0ULL;
            uint32_t next() {
                s ^= s >> 12;
                s ^= s << 25;
                s ^= s >> 27;
                return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
            }
            uint32_t next(uint32_t max) { return max ? next() % max : 0; }
        };
        FastRng rng;
        OrderBook ob(200'010, 150'010, MIN_P, MAX_P);
        std::vector<OrderId> ids;
        ids.reserve(100'000);
        for (uint32_t i = 1; i <= 100'000; ++i) {
            (void)ob.add_order(i, OrderType::Limit, 1000 + rng.next(10'000), 10, rng.next(2) ? Side::Buy : Side::Sell,
                               noop_cb);
            ids.push_back(i);
        }
        for (size_t i = ids.size() - 1; i > 0; --i) {  // shuffle, cancel half
            size_t j = rng.next(static_cast<uint32_t>(i + 1));
            std::swap(ids[i], ids[j]);
        }
        for (size_t i = 0; i < ids.size() / 2; ++i) {
            (void)ob.cancel_order(ids[i]);
        }
        std::vector<uint64_t> cyc;
        cyc.reserve(10'000);
        for (uint32_t i = 0; i < 10'000; ++i) {  // adds on the fragmented pool
            OrderId id = 150'001 + i;
            uint64_t t0 = rdtscp_begin();
            (void)ob.add_order(id, OrderType::Limit, 1000 + rng.next(10'000), 10, Side::Sell, noop_cb);
            cyc.push_back(rdtscp_end() - t0);
        }
        print_report("churn (adds on fragmented pool)", report(std::move(cyc)), cyc.size(), tsc_hz);
    }

    // ── 6. Mixed workload (50/25/25) ────────────────────────────────────
    {
        struct FastRng {
            uint64_t s = 0x0F0F'0F0F'0F0F'0F0FULL;
            uint32_t next() {
                s ^= s >> 12;
                s ^= s << 25;
                s ^= s >> 27;
                return static_cast<uint32_t>(s * 0x2545F4914F6CDD1DULL >> 32);
            }
            uint32_t next(uint32_t max) { return max ? next() % max : 0; }
        };
        FastRng rng;
        OrderBook ob(60'010, 60'010, MIN_P, MAX_P);
        std::vector<uint64_t> cyc;
        cyc.reserve(50'000);
        OrderId id = 1;
        for (int i = 0; i < 50'000; ++i) {
            uint32_t roll = rng.next(100);
            uint64_t t0 = rdtscp_begin();
            if (roll < 50) {
                (void)ob.add_order(id++, OrderType::Limit, 9000 + rng.next(2000), 10, Side::Sell, noop_cb);
            } else if (roll < 75) {
                (void)ob.add_order(id++, OrderType::Limit, 11'000, 25, Side::Buy, noop_cb);
            } else {
                (void)ob.cancel_order(1 + rng.next(static_cast<uint32_t>(id - 1)));
            }
            cyc.push_back(rdtscp_end() - t0);
        }
        print_report("mixed (50pct add / 25pct match / 25pct cancel)", report(std::move(cyc)), cyc.size(), tsc_hz);
    }

    std::printf("=== done ===\n");
    return 0;
}
