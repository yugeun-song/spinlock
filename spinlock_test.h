#ifndef SPINLOCK_TEST_H
#define SPINLOCK_TEST_H

#include "./spinlock.h"

/* Default Configuration Values */
#define DEFAULT_ITERATIONS  1000000
#define DEFAULT_LOAD_LOOPS  500
#define DEFAULT_NTHREADS    4

/* Global Configuration (Highest Priority: g_ prefix) */
extern int g_conf_iterations;
extern int g_conf_load_loops;
extern int g_conf_nthreads;

/* Context passed to each worker thread */
struct thread_ctx {
	int *shared_counter;
	spinlock_t *spinlock;
	pthread_mutex_t *mutex;
};

/* Utilities */
double calc_time_diff_ms(struct timespec *start, struct timespec *end);

/* Worker Tasks (Unified) */
void *task_spinlock(void *arg);
void *task_mutex(void *arg);

#endif /* SPINLOCK_TEST_H */
