// ============================================================================
// fuzz_driver.cpp — libFuzzer entry point around the differential core
// (audit §9.5 strategy: "a LibFuzzer build of the differential core for
// continuous deep fuzzing").
//
//   clang++ -fsanitize=fuzzer,address,undefined ... tools/fuzz_driver.cpp
//
// Input bytes are interpreted as a deterministic op script: each 8-byte
// chunk drives one operation (op type, side, price delta, quantity, id).
// The engine and the StdOrderBook reference must agree on every result
// code, filled quantity and trade; any divergence is reported back to
// libFuzzer as a crash with the offending input preserved.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/OrderBook.h"
#include "StdOrderBook.h"

using namespace hft;

namespace {

struct Script {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint32_t next() {
        uint32_t v = 0;
        for (int b = 0; b < 4; ++b) {
            v = (v << 8) | (pos < size ? data[pos++] : (pos & 0xFF));
        }
        return v;
    }
};

void AssertEq64(uint64_t a, uint64_t b, const char* what, size_t op) {
    if (a != b) {
        std::fprintf(stderr, "FUZZ DIVERGENCE at op %zu (%s): engine=%llu ref=%llu\n", op, what,
                     static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
        // Let libFuzzer capture the stack + input:
        __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    constexpr Price MIN_P = 500;
    constexpr Price MAX_P = 2500;
    constexpr uint32_t ID_SPAN = 400;
    constexpr size_t POOL = 500;

    if (size < 16) return 0;  // need at least two chunks

    OrderBook engine(ID_SPAN, POOL, MIN_P, MAX_P);
    StdOrderBook ref(ID_SPAN, POOL, MIN_P, MAX_P);
    Script s{data, size};

    std::vector<Trade> et, rt;
    auto ecb = [&](const Trade& t) noexcept { et.push_back(t); };
    auto rcb = [&](const Trade& t) noexcept { rt.push_back(t); };

    const size_t max_ops = 512;
    for (size_t op = 0; op < max_ops && s.pos + 4 <= s.size + 4; ++op) {
        uint32_t chunk = s.next();
        uint32_t kind = chunk % 10;
        OrderId id = 1 + (s.next() % ID_SPAN);
        Quantity qty = 1 + (s.next() % 50);
        Price price = MIN_P + (s.next() % (MAX_P - MIN_P + 1));
        Side side = (chunk & 0x8000) ? Side::Buy : Side::Sell;
        OrderType type = OrderType::Limit;
        if (kind == 8) type = OrderType::Market;
        if (kind == 9) type = OrderType::Cancel;

        et.clear();
        rt.clear();
        Quantity ef = 0, rf = 0;
        RejectReason er, rr;
        if (type == OrderType::Cancel) {
            er = engine.cancel_order(id);
            rr = ref.cancel_order(id);
        } else if (type == OrderType::Market) {
            er = engine.add_order(id, OrderType::Market, 0, qty, side, ecb, &ef);
            rr = ref.add_order(id, OrderType::Market, 0, qty, side, rcb, &rf);
        } else {
            er = engine.add_order(id, OrderType::Limit, price, qty, side, ecb, &ef);
            rr = ref.add_order(id, OrderType::Limit, price, qty, side, rcb, &rf);
        }
        AssertEq64(static_cast<uint64_t>(er), static_cast<uint64_t>(rr), "reject code", op);
        AssertEq64(ef, rf, "filled qty", op);
        AssertEq64(et.size(), rt.size(), "trade count", op);
        for (size_t i = 0; i < et.size() && i < rt.size(); ++i) {
            AssertEq64(et[i].maker_id, rt[i].maker_id, "maker", op);
            AssertEq64(et[i].price, rt[i].price, "price", op);
            AssertEq64(et[i].quantity, rt[i].quantity, "qty", op);
        }
        AssertEq64(engine.active_order_count(), ref.active_order_count(), "resting count", op);
    }
    return 0;
}
