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
    // Head and tail of the intrusive linked list (pointers first for prefetch)
    Order* head = nullptr;    // 8 bytes
    Order* tail = nullptr;    // 8 bytes
    Quantity total_quantity = 0; // 4 bytes
    uint32_t order_count = 0;   // 4 bytes
    // alignas(32) forces 8 bytes of implicit padding here

    PriceLevel() noexcept = default;

    // Append an order to the end of the queue (Time priority)
    HFT_FORCEINLINE void append_order(Order* order) noexcept {
        if (HFT_LIKELY(head != nullptr)) [[likely]] {
            order->prev = tail;
            order->next = nullptr;
            tail->next = order;
            tail = order;
        } else {
            head = tail = order;
            order->prev = nullptr;
            order->next = nullptr;
        }
        total_quantity += order->quantity;
        ++order_count;
    }

    // Remove an order from this price level (e.g., cancellation or full fill)
    HFT_FORCEINLINE void remove_order(Order* order) noexcept {
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            head = order->next; // Order was head
        }

        if (order->next) {
            order->next->prev = order->prev;
        } else {
            tail = order->prev; // Order was tail
        }

        order->prev = nullptr;
        order->next = nullptr;
        total_quantity -= order->quantity;
        --order_count;
    }

    bool is_empty() const noexcept {
        return head == nullptr;
    }
};

static_assert(sizeof(PriceLevel) == 32, "PriceLevel struct must be exactly 32 bytes to fit two per 64-byte cache line.");

} // namespace hft
