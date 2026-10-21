#pragma once

#include <cstdint>

#include "Macros.h"
#include "Order.h"

namespace hft {

/// Level geometry (audit §4.1 cache layout): alignment == size, so exactly
/// kLevelsPerLine whole levels fit in one 64-byte cache line, never straddling.
inline constexpr size_t kCacheLineBytes = 64;
inline constexpr size_t kLevelsPerLine = 2;
inline constexpr size_t kPriceLevelBytes = kCacheLineBytes / kLevelsPerLine;  // 32

// Represents an aggregate of orders at a specific price point.
// Contains an intrusive doubly-linked list of Orders to maintain Time priority.
//
// CACHE OPTIMIZATION: alignas(32) ensures exactly two PriceLevel structs
// fit inside a 64-byte L1 cache line without straddling. The price is
// implicit from the vector index (price = min_price + index), so no
// Price field is stored.
//
// The trailing padding bytes host a debug canary (audit §8.2): the invariant
// checker verifies the magic value, catching neighbor-level overruns and
// whole-array memset-style corruption for free in Debug/Audit builds.
struct alignas(kPriceLevelBytes) PriceLevel {
    // Head and tail of the intrusive linked list (indices into MemoryPool)
    uint32_t head = INVALID_INDEX;               // 4 bytes
    uint32_t tail = INVALID_INDEX;               // 4 bytes
    uint64_t total_quantity = 0;                 // 8 bytes
    uint32_t order_count = 0;                    // 4 bytes
    uint32_t debug_canary = PRICE_LEVEL_CANARY;  // 4 bytes (padding region; verified in Debug/Audit)
    // alignas(32) forces 8 bytes of implicit padding here

    static constexpr uint32_t PRICE_LEVEL_CANARY = 0xC0FFEE11u;

    PriceLevel() noexcept = default;

    // Append an order to the end of the queue (Time priority)
    // NOTE (audit M-9c): single hint — the old code carried both HFT_LIKELY()
    // and [[likely]] on the same condition; one of them was dead weight.
    HFT_FORCEINLINE void append_order(uint32_t order_idx, Order* pool_data) noexcept {
        Order& order = pool_data[order_idx];
        if (HFT_LIKELY(head != INVALID_INDEX)) {
            order.prev = tail;
            order.next = INVALID_INDEX;
            pool_data[tail].next = order_idx;
            tail = order_idx;
        } else {
            head = tail = order_idx;
            order.prev = INVALID_INDEX;
            order.next = INVALID_INDEX;
        }
        total_quantity += order.quantity;
        ++order_count;
    }

    // Remove an order from this price level (e.g., cancellation or full fill)
    HFT_FORCEINLINE void remove_order(uint32_t order_idx, Order* pool_data) noexcept {
        Order& order = pool_data[order_idx];
        if (order.prev != INVALID_INDEX) {
            pool_data[order.prev].next = order.next;
        } else {
            head = order.next;  // Order was head
        }

        if (order.next != INVALID_INDEX) {
            pool_data[order.next].prev = order.prev;
        } else {
            tail = order.prev;  // Order was tail
        }

        order.prev = INVALID_INDEX;
        order.next = INVALID_INDEX;
        total_quantity -= order.quantity;
        --order_count;
    }

    [[nodiscard]] bool is_empty() const noexcept { return head == INVALID_INDEX; }

    /// Verify the debug canary (no-op inline in Release — constant folded away
    /// because PRICE_LEVEL_CANARY is constexpr; the check only runs in
    /// validate_book() which itself is Debug/Audit-only).
    [[nodiscard]] bool canary_intact() const noexcept { return debug_canary == PRICE_LEVEL_CANARY; }
};

static_assert(sizeof(PriceLevel) == kPriceLevelBytes,
              "PriceLevel struct must be exactly 32 bytes to fit two per 64-byte cache line.");

}  // namespace hft
