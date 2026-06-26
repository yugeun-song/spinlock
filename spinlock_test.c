#include "./spinlock_test.h"

double calc_time_diff_ms(const struct timespec *start, const struct timespec *end)
{
    if (!start || !end) {
        return 0.0;
    }

    const long long sec_diff = end->tv_sec - start->tv_sec;
    const long long nsec_diff = end->tv_nsec - start->tv_nsec;
    const long long elapsed_ns = sec_diff * 1000000000LL + nsec_diff;

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
        *counter += 1;

        for (int j = 0; j < loops; ++j) {
            asm volatile("nop" : : : "memory");
        }

        spin_unlock(lock);
    }

    return NULL;
}

void *task_pthread_spin(void *arg)
{
    struct thread_ctx *ctx = (struct thread_ctx *)arg;

    if (!ctx) {
        return NULL;
    }

    const int iters = g_conf_iterations;
    const int loops = g_conf_load_loops;
    long long *const counter = ctx->shared_counter;
    pthread_spinlock_t *const lock = ctx->pthread_spin;

    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < iters; ++i) {
        pthread_spin_lock(lock);
        *counter += 1;

        for (int j = 0; j < loops; ++j) {
            asm volatile("nop" : : : "memory");
        }

        pthread_spin_unlock(lock);
    }

    return NULL;
}
