// MemoryPool.h
// Provides a zero-dynamic-allocation memory pool for the HFT OrderBook.
// By allocating a huge slab of memory up front on startup, we avoid
// expensive operating system context switches caused by new/malloc/delete
// during the critical matching paths.

#pragma once

#include <vector>
#include <stdexcept>
#include <cstring>

namespace hft {

/**
 * @class MemoryPool
 * @brief A vector-backed memory pool designed for zero-allocation runtime.
 * 
 * Works by pre-allocating a maximum capacity of objects (`T`). In an HFT environment,
 * you would set this capacity high enough (e.g., millions) so it never exhausts
 * during a single trading day. It maintains a stack (LIFO) of available indices
 * to hand out pointers on `allocate()` and reclaim them on `deallocate()`.
 * Allocation operations are O(1).
 *
 * SAFETY: An allocation bitmap (`allocated_`) guards against double-free bugs,
 * which would otherwise cause two callers to receive the same memory address —
 * an extremely hard-to-debug corruption scenario.
 */
template <typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t max_capacity) : capacity_(max_capacity), available_count_(max_capacity) {
        pool_.resize(max_capacity);
#if !defined(NDEBUG)
        allocated_.resize(max_capacity, false);
#endif
        
        // Link all blocks together via their internal 'next' pointers
        for (size_t i = 0; i < max_capacity - 1; ++i) {
            pool_[i].next = &pool_[i + 1];
        }
        if (max_capacity > 0) {
            pool_[max_capacity - 1].next = nullptr;
            next_free_ = &pool_[0];
        } else {
            next_free_ = nullptr;
        }
    }

    // Allocate an object from the pool.
    // Returns a pointer to pool memory. In debug builds, the memory is zeroed
    // to catch stale-data bugs early.
    T* allocate() {
        if (!next_free_) {
            throw std::bad_alloc(); // Pool is exhausted
        }
        T* obj = next_free_;
        next_free_ = obj->next; // Advance free list
        
#if !defined(NDEBUG)
        size_t index = static_cast<size_t>(obj - pool_.data());
        allocated_[index] = true;
#endif
        --available_count_;

#ifndef NDEBUG
        // Value-initialize to zero in debug builds to catch stale-data bugs.
        // Using placement-new with value initialization instead of memset to avoid -Wclass-memaccess
        new (obj) T(); 
#endif

        return obj;
    }

    // Return an object to the pool.
    // Guards against double-free: if the slot is not currently allocated,
    // this is a logic error that would corrupt the free list.
    void deallocate(T* ptr) {
        // Calculate index based on pointer arithmetic
#if !defined(NDEBUG)
        size_t index = static_cast<size_t>(ptr - pool_.data());
        if (index >= capacity_) {
            throw std::out_of_range("Pointer does not belong to this memory pool.");
        }
        if (!allocated_[index]) {
            throw std::logic_error("Double free detected in MemoryPool!");
        }
        allocated_[index] = false;
#endif
        
        ptr->next = next_free_;
        next_free_ = ptr;
        ++available_count_;
    }

    size_t available() const noexcept {
        return available_count_;
    }
    
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
    T* next_free_ = nullptr;
    size_t available_count_ = 0;
#if !defined(NDEBUG)
    std::vector<uint8_t> allocated_;        // Tracks which slots are in use (byte per slot, avoids vector<bool> proxy overhead)
#endif
};

} // namespace hft
