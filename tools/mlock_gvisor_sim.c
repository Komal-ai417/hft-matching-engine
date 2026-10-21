/*
 * mlock_gvisor_sim.c — LD_PRELOAD runtime that simulates gVisor-style
 * locked-memory behavior for the hft_engine smoke matrix.
 *
 * WHAT IT SIMULATES
 * -----------------
 * On certain runtimes (gVisor/sandbox containers) mlockall() reports success
 * but every subsequent "locked" allocation — a fresh anonymous mapping under
 * MCL_FUTURE — fails with ENOMEM. That kills the engine mid-startup: the
 * order-pool reservation fails inside std::vector's allocator, surfaces as
 * std::bad_alloc, and terminates the process outright under -fno-exceptions.
 * The engine probes one page right after mlockall() and self-heals to
 * pageable memory (see src/main.cpp).
 *
 * This preload interposes exactly the four calls involved:
 *   mlockall()   -> best-effort real MCL_CURRENT, then LIE about success
 *                   and record that "MCL_FUTURE is active".
 *   mmap()       -> while "MCL_FUTURE is active", fail writable anonymous
 *                   private mappings with ENOMEM (the gVisor failure mode).
 *   mlock()      -> while "MCL_FUTURE is active", fail with EPERM
 *                   (belt for the probe's individual-lock branch).
 *   munlockall() -> clear the flag (this is the engine's self-heal trigger)
 *                   and best-effort release the real lock.
 * Everything else (madvise, brk, file mmaps, ...) passes through untouched,
 * so the engine's MADV_POPULATE_WRITE pre-touch keeps working exactly as on
 * a real host.
 *
 * BUILD
 * -----
 *   cc -shared -fPIC -O2 -o mlock_gvisor_sim.so tools/mlock_gvisor_sim.c -ldl
 *
 * RUN (expected engine behavior: exit 0, warning, pageable recap)
 * ----------------------------------------------------------------
 *   LD_PRELOAD=./mlock_gvisor_sim.so ./hft_engine
 *   [hft] mlockall succeeded but locked allocations fail on this runtime
 *   (container/sandbox?) — reverting to pageable memory
 *
 * The smoke stage in scripts/run_all.sh greps for exactly that message, so a
 * regression back to abort-on-locked-alloc failure fails the pipeline.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>

/* "MCL_FUTURE is active" — set by our fake mlockall, cleared by munlockall.
 * Single-threaded engine contract (README "Thread Safety"); no atomics. */
static int g_future_locked = 0;

typedef int (*mlockall_fn)(int);
typedef int (*munlockall_fn)(void);
typedef int (*mlock_fn)(const void*, size_t);
typedef void* (*mmap_fn)(void*, size_t, int, int, int, long);

int mlockall(int flags) {
    /* Lock what is resident today (best effort — the point of the sim is
     * the FUTURE-mapping failure, and on some hosts even MCL_CURRENT is
     * denied; we still report success so the probe path is exercised). */
    const mlockall_fn real = (mlockall_fn)dlsym(RTLD_NEXT, "mlockall");
    if (real != NULL) {
        (void)real(flags & ~MCL_FUTURE);
    }
    g_future_locked = (flags & MCL_FUTURE) ? 1 : 0;
    return 0; /* gVisor-style lie */
}

int munlockall(void) {
    const munlockall_fn real = (munlockall_fn)dlsym(RTLD_NEXT, "munlockall");
    if (real != NULL) {
        (void)real();
    }
    g_future_locked = 0; /* the engine's self-heal lands here */
    return 0;
}

int mlock(const void* addr, size_t len) {
    if (g_future_locked) {
        errno = EPERM; /* locked page acquisition is broken on this runtime */
        return -1;
    }
    const mlock_fn real = (mlock_fn)dlsym(RTLD_NEXT, "mlock");
    return real != NULL ? real(addr, len) : -1;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset) {
    if (g_future_locked && (prot & PROT_WRITE) != 0
        && (flags & MAP_PRIVATE) != 0 && (flags & MAP_ANONYMOUS) != 0) {
        errno = ENOMEM; /* the gVisor locked-allocation failure mode */
        return MAP_FAILED;
    }
    const mmap_fn real = (mmap_fn)dlsym(RTLD_NEXT, "mmap");
    return real != NULL ? real(addr, length, prot, flags, fd, offset) : MAP_FAILED;
}
