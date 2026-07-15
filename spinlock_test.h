#ifndef SPINLOCK_TEST_H
#define SPINLOCK_TEST_H

#include <pthread.h>
#include <time.h>

#include "./spinlock.h"

#define DEFAULT_ITERATIONS 1000000
#define DEFAULT_LOAD_LOOPS 500
#define DEFAULT_NTHREADS 4
#define DEFAULT_SPIN_MIN 4
#define DEFAULT_SPIN_MAX 16000

extern int g_conf_iterations;
extern int g_conf_load_loops;
extern int g_conf_nthreads;

struct thread_ctx {
    long long *shared_counter;
    spinlock_ttas_t *spinlock_ttas;
    spinlock_mcs_t *spinlock_mcs;
    pthread_spinlock_t *pthread_spin;
    pthread_barrier_t *barrier;
};

double calc_time_diff_ms(const struct timespec *start, const struct timespec *end);
void *task_spinlock_ttas(void *arg);
void *task_spinlock_mcs(void *arg);
void *task_pthread_spin(void *arg);

#endif
