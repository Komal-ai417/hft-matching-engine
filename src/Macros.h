#pragma once

// ============================================================================
// HFT Compiler Macros & Built-ins
// ============================================================================

#if defined(__GNUC__) || defined(__clang__)
    // Force aggressive inlining even when not strictly requested by -O3
    #define HFT_FORCEINLINE __attribute__((always_inline)) inline

    // Branch Predictor Hints (for older compilers; C++20 [[likely]]/[[unlikely]] is preferred)
    #define HFT_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)

    // Prefetch into L1/L2 Cache
    // rw: 0 = read, 1 = write
    // locality: 0 = none, 3 = temporal locality
    #define HFT_PREFETCH(addr, rw, locality) __builtin_prefetch((addr), (rw), (locality))

    // Assumption for compiler optimization
    #if !defined(NDEBUG)
        #include <cassert>
        #define HFT_ASSUME(cond) assert(cond)
    #else
        #define HFT_ASSUME(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
    #endif
    
    // HFT_VERIFY: if condition fails, returns from enclosing function.
    // Safe alternative to HFT_ASSUME for guarded paths — no UB on violation.
    #define HFT_VERIFY(cond) do { if (!(cond)) [[unlikely]] return; } while(0)

#elif defined(_MSC_VER)
    #define HFT_FORCEINLINE __forceinline

    // MSVC does not have a direct __builtin_expect equivalent for branch prediction
    #define HFT_LIKELY(x)   (x)
    #define HFT_UNLIKELY(x) (x)

    #include <intrin.h>
    #define HFT_PREFETCH(addr, rw, locality) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)

    #if !defined(NDEBUG)
        #include <cassert>
        #define HFT_ASSUME(cond) assert(cond)
    #else
        #define HFT_ASSUME(cond) __assume(cond)
    #endif

    #define HFT_VERIFY(cond) do { if (!(cond)) [[unlikely]] return; } while(0)

#else
    #define HFT_FORCEINLINE inline
    #define HFT_LIKELY(x)   (x)
    #define HFT_UNLIKELY(x) (x)
    #define HFT_PREFETCH(addr, rw, locality) do {} while(0)
    #define HFT_ASSUME(cond) do {} while(0)
    #define HFT_VERIFY(cond) do { if (!(cond)) [[unlikely]] return; } while(0)
#endif
