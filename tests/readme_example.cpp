// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Karyampudi Komal
//
// ============================================================================
// readme_example.cpp — a compile-guard for the README "API" example.
//
// The README example must compile against the engine's enforced
// noexcept-callback contract, so documentation cannot drift from the API.
// It mirrors the README's calls (limit order, crossing order, cancel) but
// not its literal text — the prices and order types differ so the guard
// exercises the API rather than mirroring a transcript. If the README
// example is updated, mirror the edit here in the same commit.
// ============================================================================

#include <gtest/gtest.h>

#include "../src/OrderBook.h"

using namespace hft;

namespace {
// Mirrors the README "API" section. The callback is noexcept.
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
