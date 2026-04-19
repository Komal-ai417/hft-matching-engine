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
    explicit MemoryPool(size_t max_capacity) : capacity_(max_capacity) {
        pool_.resize(max_capacity);
        free_indices_.reserve(max_capacity);
        allocated_.resize(max_capacity, false);
        
        // Initialize free list with all available indices in reverse order
        for (size_t i = max_capacity; i > 0; --i) {
            free_indices_.push_back(i - 1);
        }
    }

    // Allocate an object from the pool.
    // Returns a pointer to pool memory. In debug builds, the memory is zeroed
    // to catch stale-data bugs early.
    T* allocate() {
        if (free_indices_.empty()) {
            throw std::bad_alloc(); // Pool is exhausted
        }
        size_t index = free_indices_.back();
        free_indices_.pop_back();
        allocated_[index] = true;

#ifndef NDEBUG
        // Zero memory in debug builds to catch stale-data bugs.
        // Skipped in release for performance (~2-5 ns overhead per alloc).
        std::memset(&pool_[index], 0, sizeof(T));
#endif

        return &pool_[index];
    }

    // Return an object to the pool.
    // Guards against double-free: if the slot is not currently allocated,
    // this is a logic error that would corrupt the free list.
    void deallocate(T* ptr) {
        // Calculate index based on pointer arithmetic
        size_t index = static_cast<size_t>(ptr - pool_.data());
        if (index >= capacity_) {
            throw std::out_of_range("Pointer does not belong to this memory pool.");
        }
        if (!allocated_[index]) {
            throw std::logic_error("Double free detected in MemoryPool!");
        }
        allocated_[index] = false;
        free_indices_.push_back(index);
    }

    size_t available() const noexcept {
        return free_indices_.size();
    }
    
    size_t capacity() const noexcept {
        return capacity_;
    }

    /// Returns true if the given pointer is currently allocated from this pool.
    bool is_allocated(const T* ptr) const noexcept {
        size_t index = static_cast<size_t>(ptr - pool_.data());
        return index < capacity_ && allocated_[index];
    }

private:
    size_t capacity_;
    std::vector<T> pool_;
    std::vector<size_t> free_indices_;   // Stack of available indices
    std::vector<bool> allocated_;        // Tracks which slots are in use
};

} // namespace hft
