#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <time.h>

#if defined(__x86_64__)
#include <immintrin.h>
#elif !defined(__aarch64__)
#error "spinlock.h supports x86-64 (__x86_64__) and aarch64 (__aarch64__) only"
#endif

#if defined(SPINLOCK_DEBUG)
#include <stdio.h>
#include <stdlib.h>
#endif

/*
 * Cache line size for modern x86-64 and aarch64 processors to prevent "False Sharing".
 * Without padding, multiple locks might reside on the same 64-byte line,
 * causing CPU cores to fight for ownership (MESI protocol) even if they
 * access different locks.
 */
#define CACHE_LINE_SIZE 64
#define IS_SPINLOCK_UNLOCKED 0
#define IS_SPINLOCK_LOCKED 1

/*
 * Misuse checks compiled in only when SPINLOCK_DEBUG is defined (the trace
 * build does this). Each check is a plain load on a path the caller already
 * owns, so it never perturbs the lock protocol; a failed check aborts with the
 * offending call site so a debugger stops on the bug rather than on the
 * corruption it would otherwise cause later.
 */
#if defined(SPINLOCK_DEBUG)
#define SPINLOCK_DEBUG_ASSERT(condition, message)                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "spinlock: %s (%s:%d)\n", message, __FILE__, __LINE__);                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
#else
#define SPINLOCK_DEBUG_ASSERT(condition, message)                                                  \
    do {                                                                                           \
    } while (0)
#endif

/*
 * Backoff window of the TTAS disciplines in pause iterations, defined in
 * spinlock.c with these defaults and re-tuned by the benchmark's -m / -M.
 */
#define SPINLOCK_DEFAULT_SPIN_MIN 4
#define SPINLOCK_DEFAULT_SPIN_MAX 16000

extern int g_conf_spin_min;
extern int g_conf_spin_max;

typedef int spinlock_word_t;

/*
 * Three lock disciplines, each named for its algorithm and wait policy, all
 * running on x86-64 and aarch64:
 *
 *   - spin_lock_ttas      : test-and-test-and-set with exponential backoff
 *     capped at g_conf_spin_max pause iterations. Pure spin, never sleeps.
 *   - spin_lock_ttas_park : the same loop, but once the cap is reached every
 *     further failed CAS calls nanosleep(1 us). The real sleep is 1 us plus
 *     the thread's timer slack (50 us by default on Linux), so waiters park
 *     while the holder re-acquires unopposed: throughput over fairness.
 *   - spin_lock_mcs       : an MCS queue lock. Each waiter spins on its own
 *     cache line and hand-off is FIFO-fair, at the cost of a cache-line
 *     transfer per hand-off; it convoys when threads oversubscribe the cores.
 */

typedef struct {
    volatile spinlock_word_t is_locked;
    /*
     * Cache line for modern x86-64 and aarch64 processors to prevent "False Sharing".
     * Without padding, multiple locks might reside on the same 64-byte line,
     * causing CPU cores to fight for ownership (MESI protocol) even if they
     * access different locks.
     */
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(spinlock_word_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) spinlock_ttas_t;

/*
 * One MCS waiter record. A thread enqueues its own node on the lock's tail and
 * then spins on its private 'waiting_for_handoff' flag; the predecessor clears
 * that flag to pass ownership. Because the flag is local to the waiter, the
 * queue never bounces a single shared line between all the spinners. Aligned
 * to its own cache line so one waiter's polled flag never shares a line with
 * another node.
 */
typedef struct mcs_node {
    struct mcs_node *volatile next_waiter;
    volatile spinlock_word_t waiting_for_handoff;
} __attribute__((aligned(CACHE_LINE_SIZE))) mcs_node_t;

typedef struct {
    /* Tail of the waiter queue; NULL when the lock is free and unqueued. */
    mcs_node_t *volatile queue_tail;
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(mcs_node_t *)];
} __attribute__((aligned(CACHE_LINE_SIZE))) spinlock_mcs_t;

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
    asm volatile("yield" ::: "memory");
#endif
}

static inline void spin_init_ttas(spinlock_ttas_t *lock)
{
    lock->is_locked = IS_SPINLOCK_UNLOCKED;
}

static inline int ttas_cas_acquire(spinlock_ttas_t *lock)
{
    int desired_word = IS_SPINLOCK_LOCKED;
    /*
     * 'expected_word' is UNLOCKED on entry to every attempt. A failed CAS
     * overwrites it with the lock's current value (x86 'cmpxchg' leaves it
     * in EAX, aarch64 'ldaxr' loads it into the output register, and aarch64
     * LSE 'casa' always writes the prior value back into it), so the
     * comparison baseline must be reloaded for the next try.
     */
    int expected_word = IS_SPINLOCK_UNLOCKED;

#if defined(__x86_64__)
    /*
     * Test-and-Set (Atomic CAS)
     * Operates on three values: memory (%1), EAX (%0), and desired (%2).
     * - SUCCESS: memory == EAX(0). Memory becomes 1. EAX stays 0.
     * - FAILURE: memory != EAX(0). EAX becomes 1 (loads memory).
     */
    asm volatile("lock cmpxchgl %2, %1"
                 : "+a"(expected_word), "+m"(lock->is_locked)
                 : "r"(desired_word)
                 : "memory");
#elif defined(__aarch64__)
#if defined(__ARM_FEATURE_ATOMICS)
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
    asm volatile("casa %w[expected], %w[desired], %[lock_word]"
                 : [expected] "+r"(expected_word), [lock_word] "+Q"(lock->is_locked)
                 : [desired] "r"(desired_word)
                 : "memory");
#else
    /*
     * Test-and-Set (Atomic CAS) on weakly-ordered aarch64 without LSE.
     * 'ldaxr' is a load-ACQUIRE exclusive, so a successful acquire also
     * establishes acquire ordering for the critical section. The LL/SC
     * pair retries only when the exclusive reservation is lost; a value
     * mismatch leaves the observed value in 'expected_word' (mirroring x86
     * cmpxchg) and clears the monitor via 'clrex'.
     */
    {
        int store_failed;
        asm volatile("1: ldaxr   %w[observed], %[lock_word]\n\t"
                     "   cmp     %w[observed], %w[expected]\n\t"
                     "   b.ne    2f\n\t"
                     "   stlxr   %w[store_failed], %w[desired], %[lock_word]\n\t"
                     "   cbnz    %w[store_failed], 1b\n\t"
                     "   b       3f\n\t"
                     "2: clrex\n\t"
                     "3:"
                     : [observed] "=&r"(expected_word), [store_failed] "=&r"(store_failed), [lock_word] "+Q"(lock->is_locked)
                     : [expected] "r"(IS_SPINLOCK_UNLOCKED), [desired] "r"(desired_word)
                     : "memory", "cc");
    }
#endif
#endif

    /*
     * If expected_word is still 0, we won the race and successfully
     * flipped the bit from 0 to 1.
     */
    return expected_word == IS_SPINLOCK_UNLOCKED;
}

static inline void ttas_acquire(spinlock_ttas_t *lock, int park_when_capped)
{
    const struct timespec park_duration = {.tv_sec = 0, .tv_nsec = 1000};
    const int spin_max_pause_count = g_conf_spin_max;
    int backoff_pause_count = g_conf_spin_min;
    int pause_index;

    while (1) {
        /*
         * Test (Read-only observation)
         * Spinning on a read prevents generating "Invalidate" traffic
         * on the bus. We only proceed to the atomic "Set" phase when
         * we observe the lock is likely free (is_locked == 0).
         */
        while (__builtin_expect(lock->is_locked, IS_SPINLOCK_LOCKED) == IS_SPINLOCK_LOCKED) {
            cpu_relax();
        }

        if (ttas_cas_acquire(lock)) {
            return;
        }

        for (pause_index = 0; pause_index < backoff_pause_count; ++pause_index) {
            cpu_relax();
        }

        /*
         * Double the backoff until it would pass the cap, then hold it at the
         * cap. Comparing against half the cap instead of doubling first keeps
         * the arithmetic inside int for every cap value a caller can set.
         */
        if (backoff_pause_count <= spin_max_pause_count / 2) {
            backoff_pause_count *= 2;
        } else {
            backoff_pause_count = spin_max_pause_count;
            if (park_when_capped) {
                nanosleep(&park_duration, NULL);
            }
        }
    }
}

static inline void spin_lock_ttas(spinlock_ttas_t *lock)
{
    ttas_acquire(lock, 0);
}

static inline void spin_lock_ttas_park(spinlock_ttas_t *lock)
{
    ttas_acquire(lock, 1);
}

static inline void spin_unlock_ttas(spinlock_ttas_t *lock)
{
    SPINLOCK_DEBUG_ASSERT(lock->is_locked == IS_SPINLOCK_LOCKED,
                          "spin_unlock_ttas on a lock that is not held");
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
     * aarch64 is weakly ordered: a plain store is NOT a release. A store-release
     * (STLR) guarantees every critical-section write is globally observable
     * before the lock flag is seen as free, providing the release that pairs
     * with the 'ldaxr' acquire in spin_lock_ttas.
     */
    asm volatile("stlr %w[unlocked], %[lock_word]"
                 : [lock_word] "=Q"(lock->is_locked)
                 : [unlocked] "r"(IS_SPINLOCK_UNLOCKED)
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
 *   - mcs_swap_queue_tail      publishes a node as the new tail and returns the
 *                              previous one (ACQUIRE+RELEASE: xchg / swpal /
 *                              ldaxr+stlxr).
 *   - mcs_cas_queue_tail_null  CASes the tail from 'expected_tail' to NULL,
 *                              returning 1 on success (RELEASE: lock cmpxchg /
 *                              casl / ldxr+stlxr).
 *   - mcs_load_acquire_*       ACQUIRE-load a node pointer or waiter flag
 *                              (plain / ldar).
 *   - mcs_store_release_*      RELEASE-store a node pointer or waiter flag
 *                              (plain / stlr).
 */
static inline mcs_node_t *mcs_swap_queue_tail(mcs_node_t *volatile *queue_tail, mcs_node_t *new_tail)
{
    mcs_node_t *previous_tail = new_tail;
#if defined(__x86_64__)
    /* xchg with a memory operand is implicitly LOCKed and is a full barrier. */
    asm volatile("xchg %[value], %[tail_slot]"
                 : [value] "+r"(previous_tail), [tail_slot] "+m"(*queue_tail)
                 :
                 : "memory");
#elif defined(__aarch64__)
#if defined(__ARM_FEATURE_ATOMICS)
    asm volatile("swpal %[value], %[previous], %[tail_slot]"
                 : [previous] "=&r"(previous_tail), [tail_slot] "+Q"(*queue_tail)
                 : [value] "r"(new_tail)
                 : "memory");
#else
    {
        int store_failed;
        asm volatile("1: ldaxr   %[previous], %[tail_slot]\n\t"
                     "   stlxr   %w[store_failed], %[value], %[tail_slot]\n\t"
                     "   cbnz    %w[store_failed], 1b"
                     : [previous] "=&r"(previous_tail), [store_failed] "=&r"(store_failed), [tail_slot] "+Q"(*queue_tail)
                     : [value] "r"(new_tail)
                     : "memory");
    }
#endif
#endif
    return previous_tail;
}

static inline int mcs_cas_queue_tail_null(mcs_node_t *volatile *queue_tail, mcs_node_t *expected_tail)
{
#if defined(__x86_64__)
    mcs_node_t *compare_value = expected_tail;
    char swap_succeeded;
    asm volatile("lock cmpxchg %[desired], %[tail_slot]\n\t"
                 "sete %[succeeded]"
                 : [succeeded] "=q"(swap_succeeded), "+a"(compare_value), [tail_slot] "+m"(*queue_tail)
                 : [desired] "r"((mcs_node_t *)0)
                 : "memory");
    return swap_succeeded;
#elif defined(__aarch64__)
#if defined(__ARM_FEATURE_ATOMICS)
    mcs_node_t *compare_value = expected_tail;
    asm volatile("casl %[compare], %[desired], %[tail_slot]"
                 : [compare] "+r"(compare_value), [tail_slot] "+Q"(*queue_tail)
                 : [desired] "r"((mcs_node_t *)0)
                 : "memory");
    return compare_value == expected_tail;
#else
    mcs_node_t *observed_tail;
    int store_failed;
    asm volatile("1: ldxr    %[observed], %[tail_slot]\n\t"
                 "   cmp     %[observed], %[expected]\n\t"
                 "   b.ne    2f\n\t"
                 "   stlxr   %w[store_failed], %[desired], %[tail_slot]\n\t"
                 "   cbnz    %w[store_failed], 1b\n\t"
                 "   b       3f\n\t"
                 "2: clrex\n\t"
                 "3:"
                 : [observed] "=&r"(observed_tail), [store_failed] "=&r"(store_failed), [tail_slot] "+Q"(*queue_tail)
                 : [expected] "r"(expected_tail), [desired] "r"((mcs_node_t *)0)
                 : "memory", "cc");
    return observed_tail == expected_tail;
#endif
#endif
}

static inline mcs_node_t *mcs_load_acquire_node(mcs_node_t *const volatile *node_slot)
{
    mcs_node_t *loaded_node;
#if defined(__x86_64__)
    loaded_node = *node_slot;
    asm volatile("" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("ldar %[loaded], %[slot]"
                 : [loaded] "=r"(loaded_node)
                 : [slot] "Q"(*node_slot)
                 : "memory");
#endif
    return loaded_node;
}

static inline void mcs_store_release_node(mcs_node_t *volatile *node_slot, mcs_node_t *node)
{
#if defined(__x86_64__)
    asm volatile("" ::: "memory");
    *node_slot = node;
#elif defined(__aarch64__)
    asm volatile("stlr %[value], %[slot]" : [slot] "=Q"(*node_slot) : [value] "r"(node) : "memory");
#endif
}

static inline spinlock_word_t mcs_load_acquire_flag(const volatile spinlock_word_t *flag_slot)
{
    spinlock_word_t loaded_flag;
#if defined(__x86_64__)
    loaded_flag = *flag_slot;
    asm volatile("" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("ldar %w[loaded], %[slot]"
                 : [loaded] "=r"(loaded_flag)
                 : [slot] "Q"(*flag_slot)
                 : "memory");
#endif
    return loaded_flag;
}

static inline void mcs_store_release_flag(volatile spinlock_word_t *flag_slot, spinlock_word_t flag_value)
{
#if defined(__x86_64__)
    asm volatile("" ::: "memory");
    *flag_slot = flag_value;
#elif defined(__aarch64__)
    asm volatile("stlr %w[value], %[slot]"
                 : [slot] "=Q"(*flag_slot)
                 : [value] "r"(flag_value)
                 : "memory");
#endif
}

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
extern __thread mcs_node_t spin_mcs_thread_local_node;
extern __thread int spin_mcs_thread_local_node_in_use;

static inline void spin_init_mcs(spinlock_mcs_t *lock)
{
    lock->queue_tail = (mcs_node_t *)0;
}

static inline void spin_lock_mcs_node(spinlock_mcs_t *lock, mcs_node_t *self_node)
{
    self_node->next_waiter = (mcs_node_t *)0;

    /*
     * Atomically install ourselves as the tail. 'predecessor_node' is the node
     * that was there before: NULL means the lock was free and we own it now
     * with no hand-off. The swap's ACQUIRE pairs with the predecessor's release
     * below.
     */
    mcs_node_t *predecessor_node = mcs_swap_queue_tail(&lock->queue_tail, self_node);
    if (predecessor_node) {
        /*
         * Set our private flag, link ourselves behind the predecessor, then
         * spin only on our OWN cache line. The predecessor clears the flag when
         * it hands the lock over. Publishing 'next_waiter' with RELEASE makes
         * our initialised node visible before the predecessor can dereference
         * it.
         */
        self_node->waiting_for_handoff = IS_SPINLOCK_LOCKED;
        mcs_store_release_node(&predecessor_node->next_waiter, self_node);
        while (mcs_load_acquire_flag(&self_node->waiting_for_handoff) == IS_SPINLOCK_LOCKED) {
            cpu_relax();
        }
    }
}

static inline void spin_unlock_mcs_node(spinlock_mcs_t *lock, mcs_node_t *self_node)
{
    SPINLOCK_DEBUG_ASSERT(lock->queue_tail != (mcs_node_t *)0,
                          "spin_unlock_mcs_node on a lock with an empty queue");

    mcs_node_t *successor_node = mcs_load_acquire_node(&self_node->next_waiter);

    if (!successor_node) {
        /*
         * We see no successor. If the tail is still us, reset it to NULL and we
         * are done. The CAS carries RELEASE so the critical section is published
         * before the lock becomes free.
         */
        if (mcs_cas_queue_tail_null(&lock->queue_tail, self_node)) {
            return;
        }
        /*
         * The CAS failed: a successor has already swapped into the tail but has
         * not finished linking its node into ours. Wait for that link to appear.
         */
        while (!(successor_node = mcs_load_acquire_node(&self_node->next_waiter))) {
            cpu_relax();
        }
    }

    /* Hand the lock over by clearing the successor's private flag (RELEASE). */
    mcs_store_release_flag(&successor_node->waiting_for_handoff, IS_SPINLOCK_UNLOCKED);
}

static inline void spin_lock_mcs(spinlock_mcs_t *lock)
{
    SPINLOCK_DEBUG_ASSERT(!spin_mcs_thread_local_node_in_use,
                          "spin_lock_mcs nested: the thread-local node is already queued, "
                          "use spin_lock_mcs_node with a second node");
#if defined(SPINLOCK_DEBUG)
    spin_mcs_thread_local_node_in_use = 1;
#endif
    spin_lock_mcs_node(lock, &spin_mcs_thread_local_node);
}

static inline void spin_unlock_mcs(spinlock_mcs_t *lock)
{
    SPINLOCK_DEBUG_ASSERT(spin_mcs_thread_local_node_in_use,
                          "spin_unlock_mcs without a matching spin_lock_mcs on this thread");
    spin_unlock_mcs_node(lock, &spin_mcs_thread_local_node);
#if defined(SPINLOCK_DEBUG)
    spin_mcs_thread_local_node_in_use = 0;
#endif
}

#endif
