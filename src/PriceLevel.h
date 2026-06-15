#pragma once

#include "Order.h"
#include "Macros.h"

namespace hft {

// Represents an aggregate of orders at a specific price point.
// Contains an intrusive doubly-linked list of Orders to maintain Time priority.
//
// CACHE OPTIMIZATION: alignas(32) ensures exactly two PriceLevel structs
// fit inside a 64-byte L1 cache line without straddling. The price is
// implicit from the vector index (price = min_price + index), so no
// Price field is stored.
struct alignas(32) PriceLevel {
    // Head and tail of the intrusive linked list (indices into MemoryPool)
    uint32_t head = INVALID_INDEX; // 4 bytes
    uint32_t tail = INVALID_INDEX; // 4 bytes
    uint64_t total_quantity = 0;   // 8 bytes
    uint32_t order_count = 0;      // 4 bytes
    // alignas(32) forces 12 bytes of implicit padding here

    PriceLevel() noexcept = default;

    // Append an order to the end of the queue (Time priority)
    HFT_FORCEINLINE void append_order(uint32_t order_idx, Order* pool_data) noexcept {
        Order& order = pool_data[order_idx];
        if (HFT_LIKELY(head != INVALID_INDEX)) [[likely]] {
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
            head = order.next; // Order was head
        }

        if (order.next != INVALID_INDEX) {
            pool_data[order.next].prev = order.prev;
        } else {
            tail = order.prev; // Order was tail
        }

        order.prev = INVALID_INDEX;
        order.next = INVALID_INDEX;
        total_quantity -= order.quantity;
        --order_count;
    }

    bool is_empty() const noexcept {
        return head == INVALID_INDEX;
    }
};

static_assert(sizeof(PriceLevel) == 32, "PriceLevel struct must be exactly 32 bytes to fit two per 64-byte cache line.");

} // namespace hft
