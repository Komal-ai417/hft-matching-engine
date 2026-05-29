// MemoryPool.h
// Provides a zero-dynamic-allocation memory pool for the HFT OrderBook.
// By allocating a huge slab of memory up front on startup, we avoid
// expensive operating system context switches caused by new/malloc/delete
// during the critical matching paths.

#pragma once

#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#if defined(__linux__)
#include <sys/mman.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#include "Macros.h"

namespace hft {

/**
 * @class MemoryPool
 * @brief An intrusive-free-list memory pool designed for zero-allocation runtime.
 * 
 * Works by pre-allocating a maximum capacity of objects (`T`). In an HFT environment,
 * you would set this capacity high enough (e.g., millions) so it never exhausts
 * during a single trading day. It maintains an intrusive singly-linked free-list
 * embedded directly within unused pool slots, reusing the first pointer-sized
 * field of each T object. Allocation and deallocation are strictly O(1).
 *
 * SAFETY: An allocation bitmap (`allocated_`) guards against double-free bugs
 * in debug builds.
 */
template <typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t max_capacity) : capacity_(max_capacity) {
        pool_.resize(max_capacity);
#if !defined(NDEBUG)
        allocated_.resize(max_capacity, false);
#endif
        
        // Build intrusive free-list through pool slots
        // Each free slot's first pointer-sized bytes point to the next free slot
        free_head_ = nullptr;
        for (size_t i = max_capacity; i > 0; --i) {
            T* slot = &pool_[i - 1];
            *reinterpret_cast<T**>(slot) = free_head_;
            free_head_ = slot;
        }

        // Memory Pre-Touching: fault in all pages immediately
        const size_t pool_bytes = max_capacity * sizeof(T);
#if defined(__linux__)
        madvise(pool_.data(), pool_bytes, MADV_POPULATE_READ);
#elif defined(_WIN32)
        // Lock pages into physical memory to prevent page faults
        VirtualLock(pool_.data(), pool_bytes);
#else
        volatile char* p = reinterpret_cast<volatile char*>(pool_.data());
        for (size_t offset = 0; offset < pool_bytes; offset += 4096) {
            p[offset] = 0;
        }
#endif
    }

    // Allocate an object from the pool via intrusive free-list pop.
    // O(1), no branching on the happy path.
    HFT_FORCEINLINE T* allocate() noexcept {
        if (HFT_UNLIKELY(free_head_ == nullptr)) {
            return nullptr; // Pool exhausted — caller silently drops the order
        }
        
        T* obj = free_head_;
        free_head_ = *reinterpret_cast<T**>(obj);
        
#if !defined(NDEBUG)
        size_t index = static_cast<size_t>(obj - pool_.data());
        allocated_[index] = true;
        // Value-initialize to zero in debug builds to catch stale-data bugs.
        new (obj) T(); 
#endif

        return obj;
    }

    // Return an object to the pool via intrusive free-list push.
#if !defined(NDEBUG)
    // Debug deallocate: throws on invalid memory or double free
    void deallocate(T* ptr) {
        size_t index = static_cast<size_t>(ptr - pool_.data());
        if (index >= capacity_) {
            throw std::out_of_range("Pointer does not belong to this memory pool.");
        }
        if (!allocated_[index]) {
            throw std::logic_error("Double free detected in MemoryPool!");
        }
        allocated_[index] = false;
        
        *reinterpret_cast<T**>(ptr) = free_head_;
        free_head_ = ptr;
    }
#else
    // Release deallocate: zero overhead, no branching, no exceptions
    HFT_FORCEINLINE void deallocate(T* ptr) noexcept {
        *reinterpret_cast<T**>(ptr) = free_head_;
        free_head_ = ptr;
    }
#endif

#if !defined(NDEBUG)
    void deallocate_chain(T* head, T* tail) {
        T* cur = head;
        while (cur != nullptr) {
            size_t index = static_cast<size_t>(cur - pool_.data());
            if (index >= capacity_) {
                throw std::out_of_range("Pointer does not belong to this memory pool.");
            }
            if (!allocated_[index]) {
                throw std::logic_error("Double free detected in MemoryPool!");
            }
            allocated_[index] = false;
            if (cur == tail) break;
            cur = cur->next;
        }
        *reinterpret_cast<T**>(tail) = free_head_;
        free_head_ = head;
    }
#else
    HFT_FORCEINLINE void deallocate_chain(T* head, T* tail) noexcept {
        *reinterpret_cast<T**>(tail) = free_head_;
        free_head_ = head;
    }
#endif

    size_t capacity() const noexcept {
        return capacity_;
    }

    /// Returns true if the given pointer is currently allocated from this pool.
    bool is_allocated(const T* ptr) const noexcept {
        size_t index = static_cast<size_t>(ptr - pool_.data());
#if !defined(NDEBUG)
        return index < capacity_ && allocated_[index];
#else
        return index < capacity_;
#endif
    }

private:
    size_t capacity_;
    std::vector<T> pool_;
    T* free_head_ = nullptr;
#if !defined(NDEBUG)
    std::vector<uint8_t> allocated_;
#endif
};

} // namespace hft
