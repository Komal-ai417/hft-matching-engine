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
#if !defined(NDEBUG) || defined(HFT_AUDIT_MODE)
        allocated_.resize(max_capacity, false);
#endif
        
        // Build intrusive free-list through pool slots
        // Each free slot's first 4 bytes (uint32_t) point to the next free slot index
        free_head_ = INVALID_INDEX;
        for (size_t i = max_capacity; i > 0; --i) {
            T* slot = &pool_[i - 1];
            *reinterpret_cast<uint32_t*>(slot) = free_head_;
            free_head_ = static_cast<uint32_t>(i - 1);
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
    HFT_FORCEINLINE uint32_t allocate() noexcept {
        if (HFT_UNLIKELY(free_head_ == INVALID_INDEX)) {
            return INVALID_INDEX; // Pool exhausted — caller silently drops the order
        }
        
        uint32_t idx = free_head_;
        T* obj = &pool_[idx];
        free_head_ = *reinterpret_cast<uint32_t*>(obj);
        
#if !defined(NDEBUG) || defined(HFT_AUDIT_MODE)
        allocated_[idx] = true;
        // Value-initialize to zero in debug builds to catch stale-data bugs.
        new (obj) T(); 
#endif

        return idx;
    }

    // Return an object to the pool via intrusive free-list push.
#if !defined(NDEBUG) || defined(HFT_AUDIT_MODE)
    // Debug deallocate: throws on invalid memory or double free
    void deallocate(uint32_t index) {
        if (index >= capacity_) {
            throw std::out_of_range("Index does not belong to this memory pool.");
        }
        if (!allocated_[index]) {
            throw std::logic_error("Double free detected in MemoryPool!");
        }
        allocated_[index] = false;
        
        T* ptr = &pool_[index];
        *reinterpret_cast<uint32_t*>(ptr) = free_head_;
        free_head_ = index;
    }
#else
    // Release deallocate: zero overhead, no branching, no exceptions
    HFT_FORCEINLINE void deallocate(uint32_t index) noexcept {
        T* ptr = &pool_[index];
        *reinterpret_cast<uint32_t*>(ptr) = free_head_;
        free_head_ = index;
    }
#endif

#if !defined(NDEBUG) || defined(HFT_AUDIT_MODE)
    void deallocate_chain(uint32_t head, uint32_t tail) {
        uint32_t cur = head;
        while (cur != INVALID_INDEX) {
            if (cur >= capacity_) {
                throw std::out_of_range("Index does not belong to this memory pool.");
            }
            if (!allocated_[cur]) {
                throw std::logic_error("Double free detected in MemoryPool!");
            }
            allocated_[cur] = false;
            if (cur == tail) break;
            cur = pool_[cur].next;
        }
        T* ptr = &pool_[tail];
        *reinterpret_cast<uint32_t*>(ptr) = free_head_;
        free_head_ = head;
    }
#else
    HFT_FORCEINLINE void deallocate_chain(uint32_t head, uint32_t tail) noexcept {
        T* ptr = &pool_[tail];
        *reinterpret_cast<uint32_t*>(ptr) = free_head_;
        free_head_ = head;
    }
#endif

    size_t capacity() const noexcept {
        return capacity_;
    }

    /// Access the underlying array directly
    T* data() noexcept {
        return pool_.data();
    }
    
    const T* data() const noexcept {
        return pool_.data();
    }

    /// Returns true if the given index is currently allocated from this pool.
    bool is_allocated(uint32_t index) const noexcept {
#if !defined(NDEBUG) || defined(HFT_AUDIT_MODE)
        return index < capacity_ && allocated_[index];
#else
        return index < capacity_;
#endif
    }

private:
    size_t capacity_;
    std::vector<T> pool_;
    uint32_t free_head_ = INVALID_INDEX;
#if !defined(NDEBUG) || defined(HFT_AUDIT_MODE)
    std::vector<uint8_t> allocated_;
#endif
};

} // namespace hft
