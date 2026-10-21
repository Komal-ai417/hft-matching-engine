// ============================================================================
// README example — compiled verbatim as a regression test (audit C-2:
// the README "API" example did NOT compile against the engine's enforced
// noexcept-callback contract; documentation drifted from the API. This file
// is the compile-guard: if the README example is updated, update this file
// in the same commit).
// ============================================================================

#include <gtest/gtest.h>

#include "../src/OrderBook.h"

using namespace hft;

namespace {
// Mirrors the README "Quick Start" API section. The callback is noexcept.
// (If you edit the README example, mirror the edit here.)
void readme_example() {
    OrderBook ob(1'000'000, 1'000'000, 0, 200'000);  // max ids, pool size, price range

    // A resting sell order. The callback fires per fill; must be noexcept.
    RejectReason r1 =
        ob.add_order(1, OrderType::Limit, /*price=*/100'500, /*qty=*/10, Side::Sell, [](const Trade&) noexcept {});

    // An aggressive buy that crosses it:
    std::vector<Trade> fills;
    RejectReason r2 =
        ob.add_order(2, OrderType::Limit, 100'500, 10, Side::Buy, [&](const Trade& t) noexcept { fills.push_back(t); });

    // Cancel via the order-type entry path:
    RejectReason r3 = ob.add_order(3, OrderType::Cancel, 0, 0, Side::Buy, [](const Trade&) noexcept {});

    // Handle the results (add_order is [[nodiscard]]):
    if (r1 == RejectReason::Accepted && r2 == RejectReason::Accepted && fills.size() == 1 &&
        r3 == RejectReason::CancelFailed) {
        // expected: the buy crossed the sell; order 3 never rested, so its
        // cancel fails.
    }
}
}  // namespace

TEST(ReadmeExample, CompilesAndRuns) {
    readme_example();
    SUCCEED();
}
