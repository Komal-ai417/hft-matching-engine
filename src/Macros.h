#pragma once

// ============================================================================
// HFT Compiler Macros & Built-ins
// ============================================================================
//
// Build flavors (see CMakeLists.txt and docs/5_Test_Infrastructure.md):
//   Debug   : NDEBUG undefined            -> HFT_DEBUG_TOOLS active + sanitizers
//   Audit   : -O2 + HFT_AUDIT_MODE        -> HFT_DEBUG_TOOLS active, optimized
//   Release : -O3 + NDEBUG                -> all instrumentation compiled out
//
// HFT_DEBUG_TOOLS governs every debugging aid in the library:
// canaries, slot state stamps, allocation tracking, the event journal,
// the re-entrancy / thread latches and validate_book().
// It is intentionally a single macro so instrumentation can never be
// half-enabled by accident.

#include <cstdint>

#if !defined(NDEBUG) || defined(HFT_AUDIT_MODE)
#define HFT_DEBUG_TOOLS 1
#else
#define HFT_DEBUG_TOOLS 0
#endif

#if defined(__GNUC__) || defined(__clang__)
// Force aggressive inlining even when not strictly requested by -O3
#define HFT_FORCEINLINE __attribute__((always_inline)) inline

// Branch Predictor Hints (for older compilers; C++20 [[likely]]/[[unlikely]] is preferred)
#define HFT_LIKELY(x) __builtin_expect(!!(x), 1)
#define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)

// Prefetch into L1/L2 Cache
// rw: 0 = read, 1 = write
// locality: 0 = none, 3 = temporal locality
#define HFT_PREFETCH(addr, rw, locality) __builtin_prefetch((addr), (rw), (locality))

#elif defined(_MSC_VER)
#define HFT_FORCEINLINE __forceinline

// MSVC does not have a direct __builtin_expect equivalent for branch prediction
#define HFT_LIKELY(x) (x)
#define HFT_UNLIKELY(x) (x)

#include <intrin.h>
// NOTE: _mm_prefetch only exposes a hint argument; the rw/locality
// arguments are approximated away (treated as _MM_HINT_T0). This shim
// is not compiled by CI (no Windows leg); see CONTRIBUTING.md.
#define HFT_PREFETCH(addr, rw, locality) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#else
#define HFT_FORCEINLINE inline
#define HFT_LIKELY(x) (x)
#define HFT_UNLIKELY(x) (x)
#define HFT_PREFETCH(addr, rw, locality) \
    do {                                 \
        (void)(addr);                    \
        (void)(rw);                      \
        (void)(locality);                \
    } while (0)
#endif

// ============================================================================
// Cycle-counter for the audit journal and latency harnesses.
// Falls back to a monotonic counter when RDTSC is unavailable.
// ============================================================================
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define HFT_HAS_RDTSC 1
#include <x86intrin.h>
#endif

namespace hft {

/// Monotonic timestamp for journal entries. Uses RDTSC when available
/// (sub-ns, deterministic within a run), otherwise a process-global counter
/// (deterministic across runs — preferable for replayable tests).
[[nodiscard]] inline uint64_t debug_timestamp() noexcept {
#if defined(HFT_HAS_RDTSC)
    return __rdtsc();
#else
    static uint64_t counter = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) single-threaded engine
    return ++counter;
#endif
}

}  // namespace hft

// Preconditions are guarded with explicit early-returns carrying a
// RejectReason (see OrderBook::add_order) — no UB, fully testable. Assume/verify
// style macros are deliberately absent: an assume expands to
// __builtin_unreachable() in release builds, and a verify whose semantics are
// `return;` silently breaks value-returning functions.
