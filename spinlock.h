#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <time.h>

#if defined(__x86_64__)
# include <immintrin.h>
#elif !defined(__aarch64__)
# error "spinlock.h supports x86-64 (__x86_64__) and aarch64 (__aarch64__) only"
#endif

#if defined(SPINLOCK_DEBUG)
# include <stdio.h>
# include <stdlib.h>
#endif

/*
 * Cache line size for modern x86-64 and aarch64 processors to prevent "False Sharing".
 * Without padding, multiple locks might reside on the same 64-byte line,
 * causing CPU cores to fight for ownership (MESI protocol) even if they
 * access different locks.
 */
#define CACHE_LINE_SIZE 64

/*
 * Misuse checks compiled in only when SPINLOCK_DEBUG is defined (the debug
 * and sanitize builds do this). Each check is a plain load on a path the caller already
 * owns, so it never perturbs the lock protocol; a failed check aborts with the
 * offending call site so a debugger stops on the bug rather than on the
 * corruption it would otherwise cause later.
 */
#if defined(SPINLOCK_DEBUG)
# define SPINLOCK_DEBUG_ASSERT(condition, message)                                     \
     do {                                                                              \
         if (!(condition)) {                                                           \
             fprintf(stderr, "spinlock: %s (%s:%d)\n", (message), __FILE__, __LINE__); \
             abort();                                                                  \
         }                                                                             \
     } while (0)
#else
# define SPINLOCK_DEBUG_ASSERT(condition, message) \
     do {                                          \
     } while (0)
#endif

/*
 * Backoff window of the TTAS disciplines in nanoseconds, defined in
 * spinlock.c with these defaults and re-tuned by the benchmark's -m / -M.
 * The wait is timed with the CPU tick counter, so the window is the same
 * wall time on every core type, whatever a pause or yield costs there.
 */
#define SPINLOCK_DEFAULT_BACKOFF_MIN_NS 32
#define SPINLOCK_DEFAULT_BACKOFF_MAX_NS 1024
#define SPINLOCK_TICK_CALIBRATION_NS 200000

typedef int spinlock_word;

enum spinlock_state {
    SPINLOCK_UNLOCKED,
    SPINLOCK_LOCKED,
};

/*
 * Three lock disciplines, each named for its algorithm and wait policy, all
 * running on x86-64 and aarch64:
 *
 *   - spin_lock_ttas      : test-and-test-and-set with exponential backoff
 *     capped at g_conf_backoff_max_ns; each wait is drawn from the upper half
 *     of the current window so waiters do not retry in lockstep. Pure spin,
 *     never sleeps.
 *   - spin_lock_ttas_park : the same loop, but once the cap is reached every
 *     further failed CAS calls nanosleep(1 us). The real sleep is 1 us plus
 *     the thread's timer slack (50 us by default on Linux), so waiters park
 *     while the holder re-acquires unopposed: throughput over fairness.
 *   - spin_lock_mcs       : an MCS queue lock. Each waiter spins on its own
 *     cache line and hand-off is FIFO-fair, at the cost of a cache-line
 *     transfer per hand-off; it convoys when threads oversubscribe the cores.
 */
struct spinlock_ttas {
    volatile spinlock_word is_locked;
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(spinlock_word)];
} __attribute__((aligned(CACHE_LINE_SIZE)));

/*
 * One MCS waiter record. A thread enqueues its own node on the lock's tail and
 * then spins on its private 'is_waiting_for_handoff' flag; the predecessor
 * clears that flag to pass ownership. Because the flag is local to the waiter,
 * the queue never bounces a single shared line between all the spinners.
 * Aligned to its own cache line so one waiter's polled flag never shares a
 * line with another node.
 */
struct mcs_node {
    struct mcs_node *volatile next_waiter;
    volatile spinlock_word is_waiting_for_handoff;
} __attribute__((aligned(CACHE_LINE_SIZE)));

struct spinlock_mcs {
    /* Tail of the waiter queue; NULL when the lock is free and unqueued. */
    struct mcs_node *volatile queue_tail;
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(struct mcs_node *)];
} __attribute__((aligned(CACHE_LINE_SIZE)));

extern int g_conf_backoff_min_ns;
extern int g_conf_backoff_max_ns;
extern unsigned long long g_spin_ticks_per_ns_q16;
extern __thread unsigned int g_spin_backoff_jitter_state;

/*
 * Per-thread node behind the single-argument spin_lock_mcs / spin_unlock_mcs
 * wrappers, defined once in spinlock.c so every translation unit shares the
 * same thread-local record: a lock taken through the wrapper in one file may be
 * released in another. The wrapper still cannot nest, because one node can sit
 * in only one queue at a time; spin_lock_mcs_node / spin_unlock_mcs_node take
 * the node explicitly and have no such limit. The in-use flag is only read and
 * written under SPINLOCK_DEBUG, where it turns a nested wrapper call into an
 * abort instead of a corrupted queue.
 */
extern __thread struct mcs_node g_spin_mcs_thread_local_node;
extern __thread int g_is_spin_mcs_thread_local_node_in_use;

unsigned long long spin_measure_ticks_per_ns_q16(void);
void spin_calibrate_backoff_clock(void);

/*
 * Architecture spin-wait hint. Maps to the platform's pause primitive
 * (x86 PAUSE, aarch64 YIELD): it relaxes the pipeline and frees shared
 * front-end resources while busy-waiting, without generating bus traffic.
 */
static inline void cpu_relax(void)
{
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

static inline unsigned long long spin_read_ticks(void)
{
#if defined(__x86_64__)
    unsigned int low_word;
    unsigned int high_word;

    __asm__ volatile("rdtsc" : "=a"(low_word), "=d"(high_word));
    return ((unsigned long long)high_word << 32) | low_word;
#elif defined(__aarch64__)
    unsigned long long virtual_count;

    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(virtual_count));
    return virtual_count;
#endif
}

static inline unsigned long long spin_get_ticks_per_ns_q16(void)
{
    unsigned long long ticks_per_ns_q16 = __atomic_load_n(&g_spin_ticks_per_ns_q16,
                                                          __ATOMIC_RELAXED);

    if (__builtin_expect(ticks_per_ns_q16 == 0, 0)) {
        ticks_per_ns_q16 = spin_measure_ticks_per_ns_q16();
        __atomic_store_n(&g_spin_ticks_per_ns_q16, ticks_per_ns_q16, __ATOMIC_RELAXED);
    }
    return ticks_per_ns_q16;
}

static inline unsigned int spin_draw_backoff_jitter(void)
{
    unsigned int jitter_state = g_spin_backoff_jitter_state;

    if (jitter_state == 0) {
        jitter_state = (unsigned int)spin_read_ticks() ^
                       (unsigned int)((unsigned long)&g_spin_backoff_jitter_state) ^ 0x9E3779B9U;
        if (jitter_state == 0) {
            jitter_state = 0x9E3779B9U;
        }
    }
    jitter_state ^= (jitter_state << 13);
    jitter_state ^= (jitter_state >> 17);
    jitter_state ^= (jitter_state << 5);
    g_spin_backoff_jitter_state = jitter_state;
    return jitter_state;
}

static inline void spin_wait_backoff(int backoff_ns)
{
    const unsigned long long ticks_per_ns_q16 = spin_get_ticks_per_ns_q16();
    const unsigned long long window_ticks = ((unsigned long long)backoff_ns * ticks_per_ns_q16) >> 16;
    const unsigned int jitter_fraction = spin_draw_backoff_jitter() & 0xFFFFU;
    const unsigned long long jitter_ticks = ((window_ticks / 2) * jitter_fraction) >> 16;
    const unsigned long long wait_ticks = window_ticks - (window_ticks / 2) + jitter_ticks;
    const unsigned long long begin_ticks = spin_read_ticks();

    while (spin_read_ticks() - begin_ticks < wait_ticks) {
        cpu_relax();
    }
}

static inline void spin_init_ttas(struct spinlock_ttas *const mut_lock)
{
    mut_lock->is_locked = SPINLOCK_UNLOCKED;
}

static inline int ttas_try_acquire(struct spinlock_ttas *const mut_lock)
{
    int desired_word = SPINLOCK_LOCKED;
    /*
     * 'expected_word' is UNLOCKED on entry to every attempt. A failed CAS
     * overwrites it with the lock's current value (x86 'cmpxchg' leaves it
     * in EAX, aarch64 'ldaxr' loads it into the output register, and aarch64
     * LSE 'casa' always writes the prior value back into it), so the
     * comparison baseline must be reloaded for the next try.
     */
    int expected_word = SPINLOCK_UNLOCKED;

#if defined(__x86_64__)
    /*
     * Test-and-Set (Atomic CAS)
     * Operates on three values: memory (%1), EAX (%0), and desired (%2).
     * - SUCCESS: memory == EAX(0). Memory becomes 1. EAX stays 0.
     * - FAILURE: memory != EAX(0). EAX becomes 1 (loads memory).
     */
    __asm__ volatile("lock cmpxchgl %2, %1"
                     : "+a"(expected_word), "+m"(mut_lock->is_locked)
                     : "r"(desired_word)
                     : "memory");
#elif defined(__aarch64__)
# if defined(__ARM_FEATURE_ATOMICS)
    /*
     * Test-and-Set via the v8.1 LSE atomics extension.
     * 'casa' is a single-instruction load-ACQUIRE compare-and-swap:
     *   - it compares memory (%[lock_word]) against 'expected_word'
     *     (preloaded to UNLOCKED above),
     *   - if they match it stores 'desired_word' (LOCKED),
     *   - and it ALWAYS writes the prior memory value back into
     *     'expected_word'.
     * The acquire variant establishes the critical-section ordering, so
     * unlike the LL/SC fallback there is no exclusive monitor to lose and
     * no retry loop. This scales far better under heavy contention on
     * high-core-count machines. Selected only when the toolchain targets an
     * LSE-capable CPU (build with e.g. -march=armv8.1-a or
     * -march=armv8-a+lse); otherwise __ARM_FEATURE_ATOMICS is undefined and
     * the ldaxr/stlxr path below is emitted instead.
     */
    __asm__ volatile("casa %w[expected], %w[desired], %[lock_word]"
                     : [expected] "+r"(expected_word), [lock_word] "+Q"(mut_lock->is_locked)
                     : [desired] "r"(desired_word)
                     : "memory");
# else
    /*
     * Test-and-Set (Atomic CAS) on weakly-ordered aarch64 without LSE.
     * 'ldaxr' is a load-ACQUIRE exclusive, so a successful acquire also
     * establishes acquire ordering for the critical section. The LL/SC
     * pair retries only when the exclusive reservation is lost; a value
     * mismatch leaves the observed value in 'expected_word' (mirroring x86
     * cmpxchg) and clears the monitor via 'clrex'.
     */
    {
        int has_store_failed;

        __asm__ volatile("1: ldaxr   %w[observed], %[lock_word]\n\t"
                         "   cmp     %w[observed], %w[expected]\n\t"
                         "   b.ne    2f\n\t"
                         "   stlxr   %w[store_failed], %w[desired], %[lock_word]\n\t"
                         "   cbnz    %w[store_failed], 1b\n\t"
                         "   b       3f\n\t"
                         "2: clrex\n\t"
                         "3:"
                         : [observed] "=&r"(expected_word), [store_failed] "=&r"(has_store_failed),
                           [lock_word] "+Q"(mut_lock->is_locked)
                         : [expected] "r"(SPINLOCK_UNLOCKED), [desired] "r"(desired_word)
                         : "memory", "cc");
    }
# endif /* defined(__ARM_FEATURE_ATOMICS) */
#endif /* defined(__x86_64__) */

    /*
     * If expected_word is still 0, we won the race and successfully
     * flipped the bit from 0 to 1.
     */
    return (expected_word == SPINLOCK_UNLOCKED) ? 0 : -1;
}

static inline void ttas_acquire(struct spinlock_ttas *const mut_lock, int should_park_when_capped)
{
    static const struct timespec s_park_duration = {
        .tv_sec = 0,
        .tv_nsec = 1000
    };
    const int backoff_max_ns = g_conf_backoff_max_ns;
    int backoff_ns = g_conf_backoff_min_ns;

    for (;;) {
        /*
         * Test (Read-only observation)
         * Spinning on a read prevents generating "Invalidate" traffic
         * on the bus. We only proceed to the atomic "Set" phase when
         * we observe the lock is likely free (is_locked == 0).
         */
        while (__builtin_expect(mut_lock->is_locked, SPINLOCK_LOCKED) == SPINLOCK_LOCKED) {
            cpu_relax();
        }

        if (__builtin_expect(ttas_try_acquire(mut_lock) == 0, 1)) {
            return;
        }

        spin_wait_backoff(backoff_ns);

        /*
         * Double the backoff until it would pass the cap, then hold it at the
         * cap. Comparing against half the cap instead of doubling first keeps
         * the arithmetic inside int for every cap value a caller can set.
         */
        if (backoff_ns <= backoff_max_ns / 2) {
            backoff_ns *= 2;
        } else {
            backoff_ns = backoff_max_ns;
            if (should_park_when_capped != 0) {
                nanosleep(&s_park_duration, NULL);
            }
        }
    }
}

static inline void spin_lock_ttas(struct spinlock_ttas *const mut_lock)
{
    ttas_acquire(mut_lock, 0);
}

static inline void spin_lock_ttas_park(struct spinlock_ttas *const mut_lock)
{
    ttas_acquire(mut_lock, 1);
}

static inline void spin_unlock_ttas(struct spinlock_ttas *const mut_lock)
{
    SPINLOCK_DEBUG_ASSERT(mut_lock->is_locked == SPINLOCK_LOCKED,
                          "spin_unlock_ttas on a lock that is not held");
#if defined(__x86_64__)
    /*
     * It prevents the compiler from moving any memory operations from
     * the critical section below this point. On x86, Store-Store
     * reordering is prohibited by hardware, so this barrier is
     * sufficient to ensure data visibility before the lock is set to 0.
     */
    __asm__ volatile("" ::: "memory");
    mut_lock->is_locked = SPINLOCK_UNLOCKED;
#elif defined(__aarch64__)
    /*
     * aarch64 is weakly ordered: a plain store is NOT a release. A store-release
     * (STLR) guarantees every critical-section write is globally observable
     * before the lock flag is seen as free, providing the release that pairs
     * with the 'ldaxr' acquire in spin_lock_ttas.
     */
    __asm__ volatile("stlr %w[unlocked], %[lock_word]"
                     : [lock_word] "=Q"(mut_lock->is_locked)
                     : [unlocked] "r"(SPINLOCK_UNLOCKED)
                     : "memory");
#endif
}

/*
 * MCS is built from the ordered primitives below, which mirror the TTAS lock's
 * philosophy exactly: on x86-64 (TSO) an acquire load and a release store are
 * plain accesses fenced against the compiler only, while the true atomics
 * ('xchg', 'lock cmpxchg') carry a full hardware barrier; on the weakly ordered
 * aarch64 each site emits the matching one-way barrier instruction, with the
 * swap and CAS selecting the LSE form when available and an ldaxr/stlxr LL/SC
 * loop otherwise.
 *   - mcs_swap_queue_tail       publishes a node as the new tail and returns
 *                               the previous one (ACQUIRE+RELEASE: xchg /
 *                               swpal / ldaxr+stlxr).
 *   - mcs_try_reset_queue_tail  CASes the tail from 'expected_tail' to NULL,
 *                               returning 0 on success (RELEASE: lock cmpxchg
 *                               / casl / ldxr+stlxr).
 *   - mcs_load_acquire_*        ACQUIRE-load a node pointer or waiter flag
 *                               (plain / ldar).
 *   - mcs_store_release_*       RELEASE-store a node pointer or waiter flag
 *                               (plain / stlr).
 */
static inline struct mcs_node *mcs_swap_queue_tail(struct mcs_node *volatile *const mut_queue_tail,
                                                   struct mcs_node *const mut_new_tail)
{
    struct mcs_node *previous_tail = mut_new_tail;
#if defined(__x86_64__)
    /* xchg with a memory operand is implicitly LOCKed and is a full barrier. */
    __asm__ volatile("xchg %[value], %[tail_slot]"
                     : [value] "+r"(previous_tail), [tail_slot] "+m"(*mut_queue_tail)
                     :
                     : "memory");
#elif defined(__aarch64__)
# if defined(__ARM_FEATURE_ATOMICS)
    __asm__ volatile("swpal %[value], %[previous], %[tail_slot]"
                     : [previous] "=&r"(previous_tail), [tail_slot] "+Q"(*mut_queue_tail)
                     : [value] "r"(mut_new_tail)
                     : "memory");
# else
    {
        int has_store_failed;

        __asm__ volatile("1: ldaxr   %[previous], %[tail_slot]\n\t"
                         "   stlxr   %w[store_failed], %[value], %[tail_slot]\n\t"
                         "   cbnz    %w[store_failed], 1b"
                         : [previous] "=&r"(previous_tail), [store_failed] "=&r"(has_store_failed),
                           [tail_slot] "+Q"(*mut_queue_tail)
                         : [value] "r"(mut_new_tail)
                         : "memory");
    }
# endif
#endif /* defined(__x86_64__) */
    return previous_tail;
}

static inline int mcs_try_reset_queue_tail(struct mcs_node *volatile *const mut_queue_tail,
                                           const struct mcs_node *const expected_tail)
{
#if defined(__x86_64__)
    const struct mcs_node *compare_value = expected_tail;
    char has_swapped;

    __asm__ volatile("lock cmpxchg %[desired], %[tail_slot]\n\t"
                     "sete %[succeeded]"
                     : [succeeded] "=q"(has_swapped), "+a"(compare_value),
                       [tail_slot] "+m"(*mut_queue_tail)
                     : [desired] "r"(NULL)
                     : "memory");
    return (has_swapped != 0) ? 0 : -1;
#elif defined(__aarch64__)
# if defined(__ARM_FEATURE_ATOMICS)
    const struct mcs_node *compare_value = expected_tail;

    __asm__ volatile("casl %[compare], %[desired], %[tail_slot]"
                     : [compare] "+r"(compare_value), [tail_slot] "+Q"(*mut_queue_tail)
                     : [desired] "r"(NULL)
                     : "memory");
    return (compare_value == expected_tail) ? 0 : -1;
# else
    struct mcs_node *observed_tail;
    int has_store_failed;

    __asm__ volatile("1: ldxr    %[observed], %[tail_slot]\n\t"
                     "   cmp     %[observed], %[expected]\n\t"
                     "   b.ne    2f\n\t"
                     "   stlxr   %w[store_failed], %[desired], %[tail_slot]\n\t"
                     "   cbnz    %w[store_failed], 1b\n\t"
                     "   b       3f\n\t"
                     "2: clrex\n\t"
                     "3:"
                     : [observed] "=&r"(observed_tail), [store_failed] "=&r"(has_store_failed),
                       [tail_slot] "+Q"(*mut_queue_tail)
                     : [expected] "r"(expected_tail), [desired] "r"(NULL)
                     : "memory", "cc");
    return (observed_tail == expected_tail) ? 0 : -1;
# endif /* defined(__ARM_FEATURE_ATOMICS) */
#endif /* defined(__x86_64__) */
}

static inline struct mcs_node *mcs_load_acquire_node(struct mcs_node *const volatile *const node_slot)
{
    struct mcs_node *loaded_node;
#if defined(__x86_64__)
    loaded_node = *node_slot;
    __asm__ volatile("" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("ldar %[loaded], %[slot]"
                     : [loaded] "=r"(loaded_node)
                     : [slot] "Q"(*node_slot)
                     : "memory");
#endif
    return loaded_node;
}

static inline void mcs_store_release_node(struct mcs_node *volatile *const mut_node_slot,
                                          struct mcs_node *const mut_node)
{
#if defined(__x86_64__)
    __asm__ volatile("" ::: "memory");
    *mut_node_slot = mut_node;
#elif defined(__aarch64__)
    __asm__ volatile("stlr %[value], %[slot]"
                     : [slot] "=Q"(*mut_node_slot)
                     : [value] "r"(mut_node)
                     : "memory");
#endif
}

static inline spinlock_word mcs_load_acquire_flag(const volatile spinlock_word *const flag_slot)
{
    spinlock_word loaded_flag;
#if defined(__x86_64__)
    loaded_flag = *flag_slot;
    __asm__ volatile("" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("ldar %w[loaded], %[slot]"
                     : [loaded] "=r"(loaded_flag)
                     : [slot] "Q"(*flag_slot)
                     : "memory");
#endif
    return loaded_flag;
}

static inline void mcs_store_release_flag(volatile spinlock_word *const mut_flag_slot,
                                          spinlock_word flag_value)
{
#if defined(__x86_64__)
    __asm__ volatile("" ::: "memory");
    *mut_flag_slot = flag_value;
#elif defined(__aarch64__)
    __asm__ volatile("stlr %w[value], %[slot]"
                     : [slot] "=Q"(*mut_flag_slot)
                     : [value] "r"(flag_value)
                     : "memory");
#endif
}

static inline void spin_init_mcs(struct spinlock_mcs *const mut_lock)
{
    mut_lock->queue_tail = NULL;
}

static inline void spin_lock_mcs_node(struct spinlock_mcs *const mut_lock,
                                      struct mcs_node *const mut_self_node)
{
    mut_self_node->next_waiter = NULL;

    /*
     * Atomically install ourselves as the tail. 'predecessor_node' is the node
     * that was there before: NULL means the lock was free and we own it now
     * with no hand-off. The swap's ACQUIRE pairs with the predecessor's release
     * below.
     */
    struct mcs_node *predecessor_node = mcs_swap_queue_tail(&mut_lock->queue_tail, mut_self_node);
    if (predecessor_node != NULL) {
        /*
         * Set our private flag, link ourselves behind the predecessor, then
         * spin only on our OWN cache line. The predecessor clears the flag when
         * it hands the lock over. Publishing 'next_waiter' with RELEASE makes
         * our initialised node visible before the predecessor can dereference
         * it.
         */
        mut_self_node->is_waiting_for_handoff = 1;
        mcs_store_release_node(&predecessor_node->next_waiter, mut_self_node);
        while (mcs_load_acquire_flag(&mut_self_node->is_waiting_for_handoff) == 1) {
            cpu_relax();
        }
    }
}

static inline void spin_unlock_mcs_node(struct spinlock_mcs *const mut_lock,
                                        const struct mcs_node *const self_node)
{
    SPINLOCK_DEBUG_ASSERT(mut_lock->queue_tail != NULL,
                          "spin_unlock_mcs_node on a lock with an empty queue");

    struct mcs_node *successor_node = mcs_load_acquire_node(&self_node->next_waiter);

    if (successor_node == NULL) {
        /*
         * We see no successor. If the tail is still us, reset it to NULL and we
         * are done. The CAS carries RELEASE so the critical section is published
         * before the lock becomes free.
         */
        if (mcs_try_reset_queue_tail(&mut_lock->queue_tail, self_node) == 0) {
            return;
        }
        /*
         * The CAS failed: a successor has already swapped into the tail but has
         * not finished linking its node into ours. Wait for that link to appear.
         */
        for (successor_node = mcs_load_acquire_node(&self_node->next_waiter);
             successor_node == NULL;
             successor_node = mcs_load_acquire_node(&self_node->next_waiter)) {
            cpu_relax();
        }
    }

    /* Hand the lock over by clearing the successor's private flag (RELEASE). */
    mcs_store_release_flag(&successor_node->is_waiting_for_handoff, 0);
}

static inline void spin_lock_mcs(struct spinlock_mcs *const mut_lock)
{
    SPINLOCK_DEBUG_ASSERT(g_is_spin_mcs_thread_local_node_in_use == 0,
                          "spin_lock_mcs nested: the thread-local node is already queued, use spin_lock_mcs_node with a second node");
#if defined(SPINLOCK_DEBUG)
    g_is_spin_mcs_thread_local_node_in_use = 1;
#endif
    spin_lock_mcs_node(mut_lock, &g_spin_mcs_thread_local_node);
}

static inline void spin_unlock_mcs(struct spinlock_mcs *const mut_lock)
{
    SPINLOCK_DEBUG_ASSERT(g_is_spin_mcs_thread_local_node_in_use != 0,
                          "spin_unlock_mcs without a matching spin_lock_mcs on this thread");
    spin_unlock_mcs_node(mut_lock, &g_spin_mcs_thread_local_node);
#if defined(SPINLOCK_DEBUG)
    g_is_spin_mcs_thread_local_node_in_use = 0;
#endif
}

#endif /* SPINLOCK_H */
