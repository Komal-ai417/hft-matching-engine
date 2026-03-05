// MemoryPool.h
// Provides a zero-dynamic-allocation memory pool for the HFT OrderBook.
// By allocating a huge slab of memory up front on startup, we avoid
// expensive operating system context switches caused by new/malloc/delete
// during the critical matching paths.

#pragma once

#include <vector>
#include <stdexcept>

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
 */
template <typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t max_capacity) : capacity_(max_capacity) {
        pool_.resize(max_capacity);
        free_indices_.reserve(max_capacity);
        
        // Initialize free list with all available indices in reverse order
        for (size_t i = max_capacity; i > 0; --i) {
            free_indices_.push_back(i - 1);
        }
    }

    // Allocate an object from the pool.
    // Returns a pointer to the uninitialized memory (actually initialized by vector default,
    // but placement new can be used if needed. Here we just return the pointer).
    T* allocate() {
        if (free_indices_.empty()) {
            throw std::bad_alloc(); // Pool is exhausted
        }
        size_t index = free_indices_.back();
        free_indices_.pop_back();
        return &pool_[index];
    }

    // Return an object to the pool.
    void deallocate(T* ptr) {
        // Calculate index based on pointer arithmetic
        size_t index = ptr - pool_.data();
        if (index >= capacity_) {
            throw std::out_of_range("Pointer does not belong to this memory pool.");
        }
        free_indices_.push_back(index);
    }

    size_t available() const noexcept {
        return free_indices_.size();
    }
    
    size_t capacity() const noexcept {
        return capacity_;
    }

private:
    size_t capacity_;
    std::vector<T> pool_;
    std::vector<size_t> free_indices_; // Stack of available indices
};

} // namespace hft
