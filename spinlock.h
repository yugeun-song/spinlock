#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/*
 * Cache line size for modern x86_64 processors to prevent "False Sharing".
 * Without padding, multiple locks might reside on the same 64-byte line,
 * causing CPU cores to fight for ownership (MESI protocol) even if they 
 * access different locks.
 */
#define COMPILE_TIME_CACHE_LINE_SIZE 64
#define IS_SPINLOCK_UNLOCKED 0
#define IS_SPINLOCK_LOCKED 1

extern int g_conf_spin_min;
extern int g_conf_spin_max;

typedef int spinlock_val_t;

typedef struct {
	volatile spinlock_val_t is_locked;
	/*
	 * Cache line for modern x86_64 processors to prevent "False Sharing".
	 * Without padding, multiple locks might reside on the same 64-byte line,
	 * causing CPU cores to fight for ownership (MESI protocol) even if they
	 * access different locks.
	 */
	char x64_aligned_padding[COMPILE_TIME_CACHE_LINE_SIZE - sizeof(spinlock_val_t)];
} __attribute__((aligned(COMPILE_TIME_CACHE_LINE_SIZE))) spinlock_t;

static inline void spin_init(spinlock_t *lock)
{
	if (!lock)
		return;
	lock->is_locked = IS_SPINLOCK_UNLOCKED;
}

static inline void spin_lock(spinlock_t *lock)
{
	int expected;
	int desired = IS_SPINLOCK_LOCKED;
	int backoff = g_conf_spin_min;

	if (!lock)
		return;

	while (1) {
		/*
		 * Test (Read-only observation)
		 * Spinning on a read prevents generating "Invalidate" traffic
		 * on the bus. We only proceed to the atomic "Set" phase when
		 * we observe the lock is likely free (is_locked == 0).
		 */
		while (__builtin_expect(lock->is_locked, IS_SPINLOCK_LOCKED) == desired)
			asm volatile("pause" ::: "memory");

		/*
		 * CRITICAL: Reset 'expected' to 0 for every attempt.
		 * In the previous failed CAS, the CPU would have overwritten
		 * the EAX register (expected) with the current lock value (1).
		 * We must reload it with 0 to try for the lock again.
		 */
		expected = IS_SPINLOCK_UNLOCKED;

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

		/*
		 * If expected is still 0, we won the race and successfully
		 * flipped the bit from 0 to 1.
		 */
		if (expected == IS_SPINLOCK_UNLOCKED)
			return;

		for (int i = 0; i < backoff; ++i)
			asm volatile("pause" ::: "memory");

		backoff *= 2;
		if (backoff > g_conf_spin_max) {
			backoff = g_conf_spin_max;
			sched_yield();
		}
	}
}

static inline void spin_unlock(spinlock_t *lock)
{
	if (!lock)
		return;

	/*
	 * It prevents the compiler from moving any memory operations from
	 * the critical section below this point. On x86, Store-Store
	 * reordering is prohibited by hardware, so this barrier is
	 * sufficient to ensure data visibility before the lock is set to 0.
	 */
	asm volatile("" ::: "memory");
	lock->is_locked = IS_SPINLOCK_UNLOCKED;
}

#endif /* SPINLOCK_H */
