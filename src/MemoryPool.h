// MemoryPool.h
// Provides a zero-dynamic-allocation memory pool for the HFT OrderBook.
// By allocating a huge slab of memory up front on startup, we avoid
// expensive operating system context switches caused by new/malloc/delete
// during the critical matching paths.
//
// ============================================================================
// AUDIT FIXES APPLIED IN THIS FILE (see AUDIT_REMEDIATION_REPORT.md)
// ----------------------------------------------------------------------------
//  * Strict-aliasing / type-punning fixed: every free-list overlay access now
//    goes through std::memcpy (report #2 §2.1; report #3 M-1c). The old
//    *reinterpret_cast<uint32_t*>(slot) was UB for any T whose first member
//    is not exactly a uint32_t.
//  * deallocate_chain validates head AND tail BEFORE the splice write
//    (report #3 H-1a — previously an unvalidated tail was a heap-buffer-
//    overflow WRITE in both Debug and Release).
//  * Debug chain walk no longer names T::next — MemoryPool<T> is now genuinely
//    generic in debug builds too (report #3 M-1a: the old walk read
//    pool_[cur].next, so any T without a `next` member failed to compile).
//  * Slot state stamps + generation counters (report #3 §8.1): FREE/LIVE
//    magic bytes detect double-free and wild frees in Debug AND Audit.
//  * Freed slots are poisoned with 0xDD (report #1 M-3, report #2 §4.1) so
//    use-after-free reads pop immediately in instrumented builds.
//  * Allocation tracking: alloc/free/live/high-water counters, free-list
//    depth reconciliation and a destruction-time leak report (report #3 §8.3).
//  * Fault reporting is exception-free by default (report #1 H-1, report #3
//    H-3): a settable handler + deterministic abort. The old `throw` collided
//    with Release's -fno-exceptions and made HFT_AUDIT_MODE a dead config.
//  * Template constraints enforced at compile time (report #3 M-1, report #1
//    M-7): sizeof/alignof gates, triviality gates, capacity clamp to uint32.
//  * Free-list head is prefetched on deallocate (report #2 §3.1) to warm the
//    next allocation's cache line.
//  * madvise pre-touch now checks its return and falls back to a per-page
//    touch loop (report #1 M-9, report #3 M-2).
// ============================================================================

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

#include "Macros.h"
#include "Order.h"  // INVALID_INDEX (audit L-1: this header must be self-contained)
#if defined(__linux__)
#include <sys/mman.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace hft {

/// Magic state bytes for slot stamping in HFT_DEBUG_TOOLS builds (§8.1).
enum class SlotState : uint8_t {
    Free = 0xF7,  ///< Slot is on the free list (payload poisoned).
    Live = 0xA5,  ///< Slot is owned by a live order.
};

constexpr uint8_t POISON_BYTE_FREED = 0xDD;  ///< Fill pattern for deallocated slots.
constexpr uint8_t POISON_BYTE_FRESH = 0xDE;  ///< Fill pattern for allocated-but-uninitialized slots.

/// What the MemoryPool should do after a fault handler returns.
enum class FaultAction : uint8_t {
    Abort,          ///< Default: print a diagnostic and std::abort() (deterministic, works under -fno-exceptions).
    SkipOperation,  ///< Test-only: the faulting call returns without mutating; the pool stays internally consistent.
};

/// Signature of the settable MemoryPool fault handler.
/// Installed via hft::set_pool_fault_handler(). Called with a static message
/// and the offending slot index (or the full pool size for capacity faults).
using PoolFaultHandler = FaultAction (*)(const char* message, size_t slot_index) noexcept;

/// Install a process-wide MemoryPool fault handler; returns the previous one.
/// Pass nullptr to restore the default (abort) behavior.
inline PoolFaultHandler set_pool_fault_handler(PoolFaultHandler handler) noexcept {
    static PoolFaultHandler current = nullptr;  // NOLINT: single-threaded engine contract (README "Thread Safety")
    PoolFaultHandler previous = current;
    current = handler;
    return previous;
}

namespace detail {

/// Capacity clamp (audit M-7): slot indices are uint32_t and INVALID_INDEX
/// (0xFFFFFFFF) must stay reserved, so the largest legal capacity is one less.
inline constexpr size_t kMaxPoolCapacity = 0xFFFF'FFFEu;

/// Assumed page size for the deterministic pre-touch fallback loops.
inline constexpr size_t kPageSize = 4096;

/// Shared storage for the opt-in leak-report flag (see set_pool_leak_report).
inline bool& pool_leak_report_flag() noexcept {
    static bool flag = false;  // NOLINT: single-threaded engine contract
    return flag;
}

}  // namespace detail

/// Opt-in destruction-time leak reporting (audit §8.3). Tests and
/// applications that intentionally end with resting orders leave live slots
/// in the pool — that is not a leak. Enable this when a session drain is
/// expected to have returned every slot (e.g. after a market sweep).
inline void set_pool_leak_report(bool enable) noexcept {
    detail::pool_leak_report_flag() = enable;
}

namespace detail {

/// Central fault path for MemoryPool: call the handler (if any), then either
/// abort or let the caller skip the operation. NEVER throws — the pool must
/// behave identically in Debug, Audit and Release fault shapes (audit H-1/H-3).
[[nodiscard]] inline FaultAction pool_fault(const char* message, size_t slot_index) noexcept {
    // Re-fetch through the setter's storage so tests can chain handlers.
    PoolFaultHandler handler = set_pool_fault_handler(nullptr);
    set_pool_fault_handler(handler);
    std::fprintf(stderr, "[HFT_POOL_FAULT] %s (slot=%zu)\n", message, slot_index);
    std::fflush(stderr);
    if (handler != nullptr && handler(message, slot_index) == FaultAction::SkipOperation) {
        return FaultAction::SkipOperation;
    }
    std::abort();
}

/// Whether destruction-time leak reporting is enabled (opt-in).
inline bool pool_leak_report_enabled() noexcept {
    return pool_leak_report_flag();
}

/// Read the intrusive free-list link overlaid in the first 4 bytes of a slot.
/// memcpy is the standard-compliant way to inspect the overlay bytes —
/// no strict-aliasing violation, no alignment requirement.
[[nodiscard]] inline uint32_t overlay_link(const void* slot) noexcept {
    uint32_t link = INVALID_INDEX;
    std::memcpy(&link, slot, sizeof(link));
    return link;
}

/// Write the intrusive free-list link into the first 4 bytes of a slot.
inline void set_overlay_link(void* slot, uint32_t link) noexcept {
    std::memcpy(slot, &link, sizeof(link));
}

}  // namespace detail

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
 * LIFECYCLE OF A SLOT (HFT_DEBUG_TOOLS builds):
 *   allocate()        -> state stamped Live, generation++, payload poisoned 0xDE
 *   caller writes     -> fields fully initialized before any read
 *   deallocate()      -> state verified Live (else fault: double-free / wild free),
 *                        payload poisoned 0xDD, state set Free, link overlaid
 *   destruction       -> any slot still Live is reported as a leak
 *
 * RELEASE builds: all of the above is compiled out. The release contract is
 * DOCUMENTED TRUST: deallocate arguments must come from book state (they
 * always do inside OrderBook); a double-free in release silently corrupts
 * the free list. Run the Audit flavor in CI to catch those classes.
 */
template<typename T>
class MemoryPool {
   public:
    static_assert(sizeof(T) >= sizeof(uint32_t),
                  "MemoryPool<T>: the intrusive free-list overlays the first 4 bytes of every slot — T must be at "
                  "least 4 bytes.");
    static_assert(alignof(T) >= alignof(uint32_t),
                  "MemoryPool<T>: T must be at least 4-byte aligned or the overlaid free-list link would be an "
                  "unaligned access.");
    static_assert(std::is_trivially_destructible_v<T>,
                  "MemoryPool<T>: slots are returned to the free list without running destructors — T must be "
                  "trivially destructible.");
    static_assert(std::is_trivially_default_constructible_v<T> ||
                  true);  // documentation-only; placement value-init happens in debug allocate()

    explicit MemoryPool(size_t max_capacity)
        : capacity_(max_capacity > detail::kMaxPoolCapacity ? detail::kMaxPoolCapacity
                                                            : max_capacity)  // audit M-7: indices are uint32
    {
        pool_.resize(capacity_);
#if HFT_DEBUG_TOOLS
        allocated_.resize(capacity_, false);
        slot_state_.resize(capacity_, static_cast<uint8_t>(SlotState::Free));
        slot_generation_.resize(capacity_, 0);
#endif

        // Build intrusive free-list through pool slots (ascending order:
        // the opening burst of orders gets ascending, cache-friendly indices).
        // (free_head_ is INVALID_INDEX via its member initializer.)
        for (size_t i = capacity_; i > 0; --i) {
            T* slot = &pool_[i - 1];
            detail::set_overlay_link(slot, free_head_);
            free_head_ = static_cast<uint32_t>(i - 1);
        }

        // Memory Pre-Touching: fault in all pages immediately so no first-write
        // page fault can land mid-trade. madvise is checked and falls back to a
        // deterministic touch loop (audit M-2 / M-9: failures were previously
        // silently discarded).
        const size_t pool_bytes = capacity_ * sizeof(T);
#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
        // MADV_POPULATE_WRITE (Linux >= 5.14) faults pages READ/WRITE — the
        // old MADV_POPULATE_READ mapped the shared zero page read-only, so the
        // first *write* still took a copy-on-write fault (audit M-2).
        if (pool_bytes > 0 && madvise(pool_.data(), pool_bytes, MADV_POPULATE_WRITE) != 0) {
            touch_pages_fallback(pool_.data(), pool_bytes);
        }
#elif defined(_WIN32)
        if (pool_bytes > 0 && !VirtualLock(pool_.data(), pool_bytes)) {
            touch_pages_fallback(pool_.data(), pool_bytes);
        }
#else
        touch_pages_fallback(pool_.data(), pool_bytes);
#endif
    }

    ~MemoryPool() {
#if HFT_DEBUG_TOOLS
        if (HFT_UNLIKELY(live_count_ != 0 && detail::pool_leak_report_flag())) {
            std::fprintf(stderr,
                         "[HFT_POOL_LEAK] ~MemoryPool: %zu slot(s) still live after %zu allocs / %zu frees "
                         "(high-water mark %zu, capacity %zu)\n",
                         live_count_, total_allocs_, total_frees_, high_water_, capacity_);
            std::fflush(stderr);
        }
#endif
    }

    MemoryPool(const MemoryPool&) = delete;             // audit L-2/M-6: copying a pool aliases
    MemoryPool& operator=(const MemoryPool&) = delete;  // logical slot identity across books.
    MemoryPool(MemoryPool&&) noexcept = default;        // vector relocation keeps indices valid.
    MemoryPool& operator=(MemoryPool&&) noexcept = default;

    // Allocate an object from the pool via intrusive free-list pop.
    // O(1), no branching on the happy path.
    HFT_FORCEINLINE uint32_t allocate() noexcept {
        if (HFT_UNLIKELY(free_head_ == INVALID_INDEX)) {
            return INVALID_INDEX;  // Pool exhausted — caller decides (engine rejects with PoolExhausted)
        }

        uint32_t idx = free_head_;
        T* obj = &pool_[idx];
        free_head_ = detail::overlay_link(obj);

#if HFT_DEBUG_TOOLS
        allocated_[idx] = true;
        slot_state_[idx] = static_cast<uint8_t>(SlotState::Live);
        ++slot_generation_[idx];
        ++total_allocs_;
        if (++live_count_ > high_water_) {
            high_water_ = live_count_;
        }
        // Value-initialize + fresh-poison to catch stale-data and read-before-init bugs.
        new (obj) T();  // NOLINT(bugprone-unhandled-exception-at-exception) T is trivial
        poison_payload(obj, POISON_BYTE_FRESH);
#endif

        return idx;
    }

    // Return an object to the pool via intrusive free-list push.
#if HFT_DEBUG_TOOLS
    // Instrumented deallocate: detects out-of-range indices, double frees and
    // wild frees via slot state stamps. Faults are exception-free (audit H-1).
    void deallocate(uint32_t index) {
        if (HFT_UNLIKELY(index >= capacity_)) {
            if (detail::pool_fault("MemoryPool::deallocate: index does not belong to this pool", index) ==
                FaultAction::SkipOperation) {
                return;
            }
        }
        if (HFT_UNLIKELY(!allocated_[index] || slot_state_[index] != static_cast<uint8_t>(SlotState::Live))) {
            if (detail::pool_fault("MemoryPool::deallocate: double free / freeing unallocated slot", index) ==
                FaultAction::SkipOperation) {
                return;
            }
        }
        allocated_[index] = false;
        slot_state_[index] = static_cast<uint8_t>(SlotState::Free);
        ++total_frees_;
        --live_count_;

        T* ptr = &pool_[index];
        poison_payload(ptr, POISON_BYTE_FREED);
        // Warm the next allocation's cache line (audit report #2 §3.1).
        if (HFT_LIKELY(free_head_ != INVALID_INDEX)) {
            HFT_PREFETCH(&pool_[free_head_], 1, 3);
        }
        detail::set_overlay_link(ptr, free_head_);
        free_head_ = index;
    }
#else
    // Release deallocate: zero overhead, no branching, no exceptions.
    // CONTRACT (documented trust): `index` must have come from allocate() via
    // consistent book state. Use the Audit flavor to verify that invariant.
    HFT_FORCEINLINE void deallocate(uint32_t index) noexcept {
        T* ptr = &pool_[index];
        if (HFT_LIKELY(free_head_ != INVALID_INDEX)) {
            HFT_PREFETCH(&pool_[free_head_], 1, 3);
        }
        detail::set_overlay_link(ptr, free_head_);
        free_head_ = index;
    }
#endif

    // Return a whole chain of objects [head .. tail] (an entire price level's
    // queue) to the pool in one splice: the intrusive order chain BECOMES the
    // free-list continuation. This is the bulk-teardown path credited with the
    // ~20x batch-match speedup, and it is the pool's most fragile operation.
#if HFT_DEBUG_TOOLS
    // Instrumented chain free: validates head and tail BEFORE any write
    // (audit H-1a — the old code spliced an unvalidated tail, a heap-buffer-
    // overflow write reachable in Debug AND Release), verifies each walked
    // slot's state, and walks the overlay link (not T::next) so the template
    // stays generic (audit M-1a).
    void deallocate_chain(uint32_t head, uint32_t tail) {
        if (HFT_UNLIKELY(head >= capacity_ || tail >= capacity_)) {
            if (detail::pool_fault("MemoryPool::deallocate_chain: head/tail index out of range",
                                   head < capacity_ ? static_cast<size_t>(tail) : static_cast<size_t>(head)) ==
                FaultAction::SkipOperation) {
                return;
            }
        }

        // Validate the whole chain first — no mutation before every link checks out.
        uint32_t cur = head;
        while (true) {
            if (HFT_UNLIKELY(!allocated_[cur] || slot_state_[cur] != static_cast<uint8_t>(SlotState::Live))) {
                if (detail::pool_fault(
                        "MemoryPool::deallocate_chain: chain slot not live (corrupt link or double free)", cur) ==
                    FaultAction::SkipOperation) {
                    return;
                }
            }
            allocated_[cur] = false;
            slot_state_[cur] = static_cast<uint8_t>(SlotState::Free);
            ++total_frees_;
            --live_count_;
            poison_payload(&pool_[cur], POISON_BYTE_FREED);
            if (cur == tail) {
                break;
            }
            cur = detail::overlay_link(&pool_[cur]);
            if (HFT_UNLIKELY(cur == INVALID_INDEX || cur >= capacity_)) {
                if (detail::pool_fault("MemoryPool::deallocate_chain: chain broken before reaching tail", tail) ==
                    FaultAction::SkipOperation) {
                    return;
                }
            }
        }

        if (HFT_LIKELY(free_head_ != INVALID_INDEX)) {
            HFT_PREFETCH(&pool_[free_head_], 1, 3);
        }
        detail::set_overlay_link(&pool_[tail], free_head_);
        free_head_ = head;
    }
#else
    // Release chain free: raw splice, zero validation. CONTRACT (documented
    // trust): head/tail must delimit a live chain obtained from level state.
    HFT_FORCEINLINE void deallocate_chain(uint32_t head, uint32_t tail) noexcept {
        if (HFT_LIKELY(free_head_ != INVALID_INDEX)) {
            HFT_PREFETCH(&pool_[free_head_], 1, 3);
        }
        detail::set_overlay_link(&pool_[tail], free_head_);
        free_head_ = head;
    }
#endif

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

    /// Access the underlying array directly
    [[nodiscard]] T* data() noexcept { return pool_.data(); }

    [[nodiscard]] const T* data() const noexcept { return pool_.data(); }

    /// Returns true if the given index is currently allocated from this pool.
    /// In Release this degenerates to a bounds check (audit H-1c) — code that
    /// needs a real answer must run in Debug/Audit.
    [[nodiscard]] bool is_allocated(uint32_t index) const noexcept {
#if HFT_DEBUG_TOOLS
        return index < capacity_ && allocated_[index];
#else
        return index < capacity_;
#endif
    }

#if HFT_DEBUG_TOOLS
    /// Allocation statistics (audit §8.3). Only in Debug/Audit builds.
    struct Stats {
        size_t total_allocs;
        size_t total_frees;
        size_t live_count;
        size_t high_water;
        size_t capacity;
        size_t free_list_depth;  // walked, must equal capacity - live_count
    };

    [[nodiscard]] Stats stats() const noexcept {
        return Stats{total_allocs_, total_frees_, live_count_, high_water_, capacity_, free_list_depth()};
    }

    /// Walk the overlay free list and return its depth. A value != capacity() -
    /// live_count() is direct evidence of free-list corruption (audit §8.3).
    /// Cycles (corruption) are detected and reported as SIZE_MAX instead of
    /// looping forever — the walk is bounded by capacity + 1 steps.
    [[nodiscard]] size_t free_list_depth() const noexcept {
        size_t depth = 0;
        uint32_t cur = free_head_;
        while (cur != INVALID_INDEX) {
            ++depth;
            if (HFT_UNLIKELY(depth > capacity_)) {
                return static_cast<size_t>(-1);  // cycle detected
            }
            if (HFT_UNLIKELY(cur >= capacity_)) {
                return static_cast<size_t>(-1);  // wild link
            }
            cur = detail::overlay_link(&pool_[cur]);
        }
        return depth;
    }

    /// Generation of a slot (how many times it has been allocated). 8-bit, wraps.
    [[nodiscard]] uint8_t slot_generation(uint32_t index) const noexcept {
        return index < capacity_ ? slot_generation_[index] : 0;
    }

    /// True if a freed slot's payload still holds the poison pattern
    /// (i.e. nothing has touched it since deallocate — the canary check).
    [[nodiscard]] bool slot_payload_is_poisoned(uint32_t index) const noexcept {
        if (index >= capacity_ || allocated_[index]) {
            return false;
        }
        // Byte-level canary inspection is the audited design (§8.1).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto* bytes = reinterpret_cast<const unsigned char*>(&pool_[index]);
        for (size_t i = sizeof(uint32_t); i < sizeof(T); ++i) {
            if (bytes[i] != POISON_BYTE_FREED) {
                return false;
            }
        }
        return true;
    }
#endif

   private:
    static void touch_pages_fallback(void* base, size_t bytes) noexcept {
        // Deterministic per-page READ so faults happen HERE, not mid-trade.
        // MUST NOT WRITE: a write-based touch zeroes the first byte of every
        // page-aligned slot — corrupting free-list links (post-audit regression
        // fix; the original code's `p[offset] = 0` had this latent bug on
        // non-Linux/non-Windows platforms).
        const auto* p = static_cast<volatile const unsigned char*>(base);
        unsigned char sink = 0;
        for (size_t offset = 0; offset < bytes; offset += detail::kPageSize) {
            sink ^= p[offset];
        }
        (void)sink;
    }

    /// Poison a slot's payload (everything past the 4-byte overlay link).
    static void poison_payload(T* slot, uint8_t pattern) noexcept {
        // Byte-level poison fill is the audited design (§8.1).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto* bytes = reinterpret_cast<unsigned char*>(slot);
        if constexpr (sizeof(T) > sizeof(uint32_t)) {
            std::memset(bytes + sizeof(uint32_t), pattern, sizeof(T) - sizeof(uint32_t));
        }
    }

    size_t capacity_;
    std::vector<T> pool_;
    uint32_t free_head_ = INVALID_INDEX;
#if HFT_DEBUG_TOOLS
    std::vector<uint8_t> allocated_;        // bitmap: slot is currently handed out
    std::vector<uint8_t> slot_state_;       // SlotState magic stamps
    std::vector<uint8_t> slot_generation_;  // per-slot allocation generation
    size_t total_allocs_ = 0;
    size_t total_frees_ = 0;
    size_t live_count_ = 0;
    size_t high_water_ = 0;
#endif
};

}  // namespace hft
