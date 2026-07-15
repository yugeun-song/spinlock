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

/*
 * Two lock disciplines are exposed, each named for its algorithm, both with the
 * same single-argument shape and both running on x86-64 and aarch64:
 *
 *   - spin_lock_ttas : test-and-test-and-set with exponential backoff. For the
 *     short, lightly held critical sections a spinlock is meant for (a flag
 *     flip, a small bounded loop) at thread counts up to the core count it is
 *     the fastest option: an uncontended acquire is a single CAS, and a
 *     just-freed lock can be re-taken with no cross-core cache-line transfer.
 *
 *   - spin_lock_mcs : an MCS queue lock. Every waiter spins on its own local
 *     cache line, so it emits no coherence storm and stays FIFO-fair even at
 *     high core counts. That scalability costs a mandatory cache-line transfer
 *     on every hand-off, so for short critical sections at low-to-modest
 *     contention it measures several times slower than the TTAS lock. Reach for
 *     it only when a great many cores hammer the same lock and fairness matters.
 */

typedef struct {
    volatile spinlock_val_t is_locked;
    /*
     * Cache line for modern x86-64 and arm64 processors to prevent "False Sharing".
     * Without padding, multiple locks might reside on the same 64-byte line,
     * causing CPU cores to fight for ownership (MESI protocol) even if they
     * access different locks.
     */
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(spinlock_val_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) spinlock_ttas_t;

/*
 * One MCS waiter record. A thread enqueues its own node on the lock's tail and
 * then spins on its private 'locked' flag; the predecessor flips that flag to
 * pass ownership. Because the flag is local to the waiter, the queue never
 * bounces a single shared line between all the spinners.
 */
typedef struct mcs_node {
    struct mcs_node *volatile next;
    volatile spinlock_val_t locked;
} mcs_node_t;

typedef struct {
    /* Tail of the waiter queue; NULL when the lock is free and unqueued. */
    mcs_node_t *volatile tail;
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(mcs_node_t *)];
} __attribute__((aligned(CACHE_LINE_SIZE))) spinlock_mcs_t;

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

static inline void spin_init_ttas(spinlock_ttas_t *lock)
{
    if (!lock) {
        return;
    }

    lock->is_locked = IS_SPINLOCK_UNLOCKED;
}

static inline void spin_lock_ttas(spinlock_ttas_t *lock)
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
         * overwrites it with the lock's current value (x86 'cmpxchg' leaves it
         * in EAX, arm64 'ldaxr' loads it into the output register, and arm64
         * LSE 'casa' always writes the prior value back into it), so the
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
#if defined(__ARM_FEATURE_ATOMICS)
        /*
         * Test-and-Set via the ARMv8.1-A LSE atomics extension.
         * 'casa' is a single-instruction load-ACQUIRE compare-and-swap:
         *   - it compares memory (%[mem]) against 'expected' (preloaded to
         *     UNLOCKED above),
         *   - if they match it stores 'desired' (LOCKED),
         *   - and it ALWAYS writes the prior memory value back into 'expected'.
         * The acquire variant establishes the critical-section ordering, so
         * unlike the LL/SC fallback there is no exclusive monitor to lose and
         * no retry loop. This scales far better under heavy contention on
         * high-core-count machines. Selected only when the toolchain targets an
         * LSE-capable CPU (build with e.g. -march=armv8.1-a or
         * -march=armv8-a+lse); otherwise __ARM_FEATURE_ATOMICS is undefined and
         * the ldaxr/stlxr path below is emitted instead.
         */
        asm volatile("casa %w[exp], %w[des], %[mem]"
                     : [exp] "+r"(expected), [mem] "+Q"(lock->is_locked)
                     : [des] "r"(desired)
                     : "memory");
#else
        /*
         * Test-and-Set (Atomic CAS) on weakly-ordered arm64 without LSE.
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

static inline void spin_unlock_ttas(spinlock_ttas_t *lock)
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
     * with the 'ldaxr' acquire in spin_lock_ttas.
     */
    asm volatile("stlr %w[v], %[mem]"
                 : [mem] "=Q"(lock->is_locked)
                 : [v] "r"(IS_SPINLOCK_UNLOCKED)
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
 *   - mcs_swap_tail       publishes a node as the new tail and returns the
 *                         previous one (ACQUIRE+RELEASE: xchg / swpal / ldaxr+stlxr).
 *   - mcs_cas_tail_null   CASes the tail from 'expected' to NULL, returning 1 on
 *                         success (RELEASE: lock cmpxchg / casl / ldxr+stlxr).
 *   - mcs_load_acquire_*  ACQUIRE-load a node pointer or waiter flag (plain / ldar).
 *   - mcs_store_release_* RELEASE-store a node pointer or waiter flag (plain / stlr).
 */
static inline mcs_node_t *mcs_swap_tail(mcs_node_t *volatile *tail, mcs_node_t *node)
{
    mcs_node_t *prev = node;
#if defined(__x86_64__)
    /* xchg with a memory operand is implicitly LOCKed and is a full barrier. */
    asm volatile("xchg %[val], %[mem]"
                 : [val] "+r"(prev), [mem] "+m"(*tail)
                 : : "memory");
#elif defined(__aarch64__)
#if defined(__ARM_FEATURE_ATOMICS)
    asm volatile("swpal %[val], %[old], %[mem]"
                 : [old] "=&r"(prev), [mem] "+Q"(*tail)
                 : [val] "r"(node)
                 : "memory");
#else
    {
        int st;
        asm volatile("1: ldaxr   %[old], %[mem]\n\t"
                     "   stlxr   %w[st], %[val], %[mem]\n\t"
                     "   cbnz    %w[st], 1b"
                     : [old] "=&r"(prev), [st] "=&r"(st), [mem] "+Q"(*tail)
                     : [val] "r"(node)
                     : "memory");
    }
#endif
#endif
    return prev;
}

static inline int mcs_cas_tail_null(mcs_node_t *volatile *tail, mcs_node_t *expected)
{
#if defined(__x86_64__)
    mcs_node_t *cmp = expected;
    char ok;
    asm volatile("lock cmpxchg %[des], %[mem]\n\t"
                 "sete %[ok]"
                 : [ok] "=q"(ok), "+a"(cmp), [mem] "+m"(*tail)
                 : [des] "r"((mcs_node_t *)0)
                 : "memory");
    return ok;
#elif defined(__aarch64__)
#if defined(__ARM_FEATURE_ATOMICS)
    mcs_node_t *cmp = expected;
    asm volatile("casl %[cmp], %[des], %[mem]"
                 : [cmp] "+r"(cmp), [mem] "+Q"(*tail)
                 : [des] "r"((mcs_node_t *)0)
                 : "memory");
    return cmp == expected;
#else
    mcs_node_t *old;
    int st;
    asm volatile("1: ldxr    %[old], %[mem]\n\t"
                 "   cmp     %[old], %[exp]\n\t"
                 "   b.ne    2f\n\t"
                 "   stlxr   %w[st], %[des], %[mem]\n\t"
                 "   cbnz    %w[st], 1b\n\t"
                 "   b       3f\n\t"
                 "2: clrex\n\t"
                 "3:"
                 : [old] "=&r"(old), [st] "=&r"(st), [mem] "+Q"(*tail)
                 : [exp] "r"(expected), [des] "r"((mcs_node_t *)0)
                 : "memory", "cc");
    return old == expected;
#endif
#endif
}

static inline mcs_node_t *mcs_load_acquire_node(mcs_node_t *volatile *p)
{
    mcs_node_t *v;
#if defined(__x86_64__)
    v = *p;
    asm volatile("" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("ldar %[v], %[mem]" : [v] "=r"(v) : [mem] "Q"(*p) : "memory");
#endif
    return v;
}

static inline void mcs_store_release_node(mcs_node_t *volatile *p, mcs_node_t *v)
{
#if defined(__x86_64__)
    asm volatile("" ::: "memory");
    *p = v;
#elif defined(__aarch64__)
    asm volatile("stlr %[v], %[mem]" : [mem] "=Q"(*p) : [v] "r"(v) : "memory");
#endif
}

static inline spinlock_val_t mcs_load_acquire_flag(volatile spinlock_val_t *p)
{
    spinlock_val_t v;
#if defined(__x86_64__)
    v = *p;
    asm volatile("" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("ldar %w[v], %[mem]" : [v] "=r"(v) : [mem] "Q"(*p) : "memory");
#endif
    return v;
}

static inline void mcs_store_release_flag(volatile spinlock_val_t *p, spinlock_val_t v)
{
#if defined(__x86_64__)
    asm volatile("" ::: "memory");
    *p = v;
#elif defined(__aarch64__)
    asm volatile("stlr %w[v], %[mem]" : [mem] "=Q"(*p) : [v] "r"(v) : "memory");
#endif
}

/*
 * The waiter record lives in thread-local storage so spin_lock_mcs keeps the
 * same single-argument shape as the TTAS lock. Two consequences, both fine for
 * the short critical sections a spinlock targets: a thread holds at most one MCS
 * lock at a time and never nests acquisitions (a second concurrent acquire would
 * reuse the node the first is still parked on); and because this node and its
 * accessors are static-inline in the header, each translation unit gets its own
 * copy, so a given lock's spin_lock_mcs and spin_unlock_mcs must be compiled in
 * the same TU.
 *
 * Aligned to its own cache line so that one waiter's 'locked' flag, which it
 * polls in a tight loop, never shares a line with another thread's node and
 * turns the local spin back into cross-core coherence traffic.
 */
static __thread mcs_node_t spin_mcs_self __attribute__((aligned(CACHE_LINE_SIZE)));

static inline void spin_init_mcs(spinlock_mcs_t *lock)
{
    if (!lock) {
        return;
    }

    lock->tail = (mcs_node_t *)0;
}

static inline void spin_lock_mcs(spinlock_mcs_t *lock)
{
    if (!lock) {
        return;
    }

    mcs_node_t *me = &spin_mcs_self;
    me->next = (mcs_node_t *)0;

    /*
     * Atomically install ourselves as the tail. 'prev' is the node that was
     * there before: NULL means the lock was free and we own it now with no
     * hand-off. The swap's ACQUIRE pairs with the predecessor's release below.
     */
    mcs_node_t *prev = mcs_swap_tail(&lock->tail, me);
    if (prev) {
        /*
         * Arm our private flag, link ourselves behind the predecessor, then
         * spin only on our OWN cache line. The predecessor clears the flag when
         * it hands the lock over. Publishing 'next' with RELEASE makes our
         * initialised node visible before the predecessor can dereference it.
         */
        me->locked = IS_SPINLOCK_LOCKED;
        mcs_store_release_node(&prev->next, me);
        while (mcs_load_acquire_flag(&me->locked) == IS_SPINLOCK_LOCKED) {
            cpu_relax();
        }
    }
}

static inline void spin_unlock_mcs(spinlock_mcs_t *lock)
{
    if (!lock) {
        return;
    }

    mcs_node_t *me = &spin_mcs_self;
    mcs_node_t *next = mcs_load_acquire_node(&me->next);

    if (!next) {
        /*
         * We see no successor. If the tail is still us, reset it to NULL and we
         * are done. The CAS carries RELEASE so the critical section is published
         * before the lock becomes free.
         */
        if (mcs_cas_tail_null(&lock->tail, me)) {
            return;
        }
        /*
         * The CAS failed: a successor has already swapped into the tail but has
         * not finished linking its node into ours. Wait for that link to appear.
         */
        while (!(next = mcs_load_acquire_node(&me->next))) {
            cpu_relax();
        }
    }

    /* Hand the lock over by clearing the successor's private flag (RELEASE). */
    mcs_store_release_flag(&next->locked, IS_SPINLOCK_UNLOCKED);
}

#endif
