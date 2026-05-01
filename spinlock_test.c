#include "./spinlock_test.h"

double calc_time_diff_ms(const struct timespec *start, const struct timespec *end)
{
    if (!start || !end) {
        return 0.0;
    }

    const long long elapsed_ns =
        (end->tv_sec - start->tv_sec) * 1000000000LL + (end->tv_nsec - start->tv_nsec);
    return (double)elapsed_ns / 1000000.0;
}

void *task_spinlock(void *arg)
{
    struct thread_ctx *ctx = (struct thread_ctx *)arg;

    if (!ctx) {
        return NULL;
    }

    const int iters = g_conf_iterations;
    const int loops = g_conf_load_loops;
    long long *const counter = ctx->shared_counter;
    spinlock_t *const lock = ctx->spinlock;

    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < iters; ++i) {
        spin_lock(lock);
        ++(*counter);

        for (int j = 0; j < loops; ++j) {
            asm volatile("nop" : : : "memory");
        }

        spin_unlock(lock);
    }
    return NULL;
}

void *task_mutex(void *arg)
{
    struct thread_ctx *ctx = (struct thread_ctx *)arg;

    if (!ctx) {
        return NULL;
    }

    const int iters = g_conf_iterations;
    const int loops = g_conf_load_loops;
    long long *const counter = ctx->shared_counter;
    pthread_mutex_t *const mutex = ctx->mutex;

    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < iters; ++i) {
        pthread_mutex_lock(mutex);
        ++(*counter);

        for (int j = 0; j < loops; ++j) {
            asm volatile("nop" : : : "memory");
        }

        pthread_mutex_unlock(mutex);
    }
    return NULL;
}
