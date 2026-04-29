#pragma once

#include "Order.h"

namespace hft {

// Represents an aggregate of orders at a specific price point.
// Contains an intrusive doubly-linked list of Orders to maintain Time priority.
struct PriceLevel {
    Price price = 0;
    Quantity total_quantity = 0;
    uint32_t order_count = 0;  // Track order count for faster empty-level detection
    
    // Head and tail of the intrusive linked list
    Order* head = nullptr;
    Order* tail = nullptr;

    PriceLevel() noexcept = default;
    explicit PriceLevel(Price p) noexcept : price(p) {}

    // Append an order to the end of the queue (Time priority)
    [[gnu::always_inline]] inline void append_order(Order* order) noexcept {
        if (!head) {
            head = tail = order;
            order->prev = nullptr;
            order->next = nullptr;
        } else {
            order->prev = tail;
            order->next = nullptr;
            tail->next = order;
            tail = order;
        }
        total_quantity += order->quantity;
        ++order_count;
    }

    // Remove an order from this price level (e.g., cancellation or full fill)
    [[gnu::always_inline]] inline void remove_order(Order* order) noexcept {
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

} // namespace hft
