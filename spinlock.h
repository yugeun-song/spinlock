#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define COMPILE_TIME_CACHE_LINE_SIZE 64
#define IS_SPINLOCK_UNLOCKED 0
#define IS_SPINLOCK_LOCKED 1

extern int g_conf_spin_min;
extern int g_conf_spin_max;

typedef int spinlock_val_t;

typedef struct {
	volatile spinlock_val_t is_locked;
	char x64_aligned_padding[COMPILE_TIME_CACHE_LINE_SIZE - sizeof(spinlock_val_t)];
} __attribute__((aligned(COMPILE_TIME_CACHE_LINE_SIZE))) spinlock_t;

static inline void spin_init(spinlock_t *lock)
{
	if (!lock) return;
	lock->is_locked = IS_SPINLOCK_UNLOCKED;
}

static inline void spin_lock(spinlock_t *lock)
{
	int expected;
	int desired = IS_SPINLOCK_LOCKED;
	int backoff = g_conf_spin_min;

	if (!lock) return;

	while (1) {
		while (__builtin_expect(lock->is_locked, IS_SPINLOCK_LOCKED) == desired)
			asm volatile("pause" ::: "memory");

		expected = IS_SPINLOCK_UNLOCKED;

		asm volatile("lock cmpxchgl %2, %1"
			     : "+a"(expected), "+m"(lock->is_locked)
			     : "r"(desired)
			     : "memory");

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
	if (!lock) return;
	asm volatile("" ::: "memory");
	lock->is_locked = IS_SPINLOCK_UNLOCKED;
}

#endif /* SPINLOCK_H */
