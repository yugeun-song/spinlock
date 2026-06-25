#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <time.h>

#if defined(__x86_64__)
#include <immintrin.h>
#elif !defined(__aarch64__)
#error "spinlock.h supports x86-64 (__x86_64__) and aarch64 (__aarch64__) only"
#endif

/*
 * Cache line size for modern x86-64 and arm64 processors to prevent "False Sharing".
 * Without padding, multiple locks might reside on the same 64-byte line,
 * causing CPU cores to fight for ownership (MESI protocol) even if they
 * access different locks.
 */
#define CACHE_LINE_SIZE 64
#define IS_SPINLOCK_UNLOCKED 0
#define IS_SPINLOCK_LOCKED 1

extern int g_conf_spin_min;
extern int g_conf_spin_max;

typedef int spinlock_val_t;

typedef struct {
    volatile spinlock_val_t is_locked;
    /*
     * Cache line for modern x86-64 and arm64 processors to prevent "False Sharing".
     * Without padding, multiple locks might reside on the same 64-byte line,
     * causing CPU cores to fight for ownership (MESI protocol) even if they
     * access different locks.
     */
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(spinlock_val_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) spinlock_t;

/*
 * Architecture spin-wait hint. Maps to the platform's pause primitive
 * (x86 PAUSE, arm64 YIELD): it relaxes the pipeline and frees shared
 * front-end resources while busy-waiting, without generating bus traffic.
 */
static inline void cpu_relax(void)
{
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

static inline void spin_init(spinlock_t *lock)
{
    if (!lock) {
        return;
    }

    lock->is_locked = IS_SPINLOCK_UNLOCKED;
}

static inline void spin_lock(spinlock_t *lock)
{
    int spin_max = g_conf_spin_max;
    int backoff = g_conf_spin_min;
    int desired = IS_SPINLOCK_LOCKED;
    int expected;
    int i;

    if (!lock) {
        return;
    }

    while (1) {
        /*
         * Test (Read-only observation)
         * Spinning on a read prevents generating "Invalidate" traffic
         * on the bus. We only proceed to the atomic "Set" phase when
         * we observe the lock is likely free (is_locked == 0).
         */
        while (__builtin_expect(lock->is_locked, IS_SPINLOCK_LOCKED) == desired) {
            cpu_relax();
        }

        /*
         * Reset 'expected' to UNLOCKED before every attempt. A failed CAS
         * overwrites it with the lock's current value (x86 'cmpxchg' leaves
         * it in EAX, arm64 'ldaxr' loads it into the output register), so the
         * comparison baseline must be reloaded for the next try.
         */
        expected = IS_SPINLOCK_UNLOCKED;

#if defined(__x86_64__)
        /*
         * Test-and-Set (Atomic CAS)
         * Operates on three values: memory (%1), EAX (%0), and desired (%2).
         * - SUCCESS: memory == EAX(0). Memory becomes 1. EAX stays 0.
         * - FAILURE: memory != EAX(0). EAX becomes 1 (loads memory).
         */
        asm volatile("lock cmpxchgl %2, %1"
                     : "+a"(expected), "+m"(lock->is_locked)
                     : "r"(desired)
                     : "memory");
#elif defined(__aarch64__)
        /*
         * Test-and-Set (Atomic CAS) on weakly-ordered arm64.
         * 'ldaxr' is a load-ACQUIRE exclusive, so a successful acquire also
         * establishes acquire ordering for the critical section. The LL/SC
         * pair retries only when the exclusive reservation is lost; a value
         * mismatch leaves the observed value in 'expected' (mirroring x86
         * cmpxchg) and clears the monitor via 'clrex'.
         */
        {
            int fail;
            asm volatile("1: ldaxr   %w[old], %[mem]\n\t"
                         "   cmp     %w[old], %w[exp]\n\t"
                         "   b.ne    2f\n\t"
                         "   stlxr   %w[st], %w[des], %[mem]\n\t"
                         "   cbnz    %w[st], 1b\n\t"
                         "   b       3f\n\t"
                         "2: clrex\n\t"
                         "3:"
                         : [old] "=&r"(expected), [st] "=&r"(fail), [mem] "+Q"(lock->is_locked)
                         : [exp] "r"(IS_SPINLOCK_UNLOCKED), [des] "r"(desired)
                         : "memory", "cc");
        }
#endif

        /*
         * If expected is still 0, we won the race and successfully
         * flipped the bit from 0 to 1.
         */
        if (expected == IS_SPINLOCK_UNLOCKED) {
            return;
        }

        for (i = 0; i < backoff; ++i) {
            cpu_relax();
        }

        backoff *= 2;
        if (backoff > spin_max) {
            backoff = spin_max;
            const struct timespec sleep_ts = {
                .tv_sec = 0,
                .tv_nsec = 1000
            };
            nanosleep(&sleep_ts, NULL);
        }
    }
}

static inline void spin_unlock(spinlock_t *lock)
{
    if (!lock) {
        return;
    }

#if defined(__x86_64__)
    /*
     * It prevents the compiler from moving any memory operations from
     * the critical section below this point. On x86, Store-Store
     * reordering is prohibited by hardware, so this barrier is
     * sufficient to ensure data visibility before the lock is set to 0.
     */
    asm volatile("" ::: "memory");
    lock->is_locked = IS_SPINLOCK_UNLOCKED;
#elif defined(__aarch64__)
    /*
     * arm64 is weakly ordered: a plain store is NOT a release. A store-release
     * (STLR) guarantees every critical-section write is globally observable
     * before the lock flag is seen as free, providing the release that pairs
     * with the 'ldaxr' acquire in spin_lock.
     */
    asm volatile("stlr %w[v], %[mem]"
                 : [mem] "=Q"(lock->is_locked)
                 : [v] "r"(IS_SPINLOCK_UNLOCKED)
                 : "memory");
#endif
}

#endif
