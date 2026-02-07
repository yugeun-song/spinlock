#include "./spinlock_test.h"

double calc_time_diff_ms(struct timespec *start, struct timespec *end)
{
	long long elapsed_ns;

	if (!start || !end) return 0.0;

	elapsed_ns = (end->tv_sec - start->tv_sec) * 1000000000LL +
		     (end->tv_nsec - start->tv_nsec);
	return (double)elapsed_ns / 1000000.0;
}

void *task_spinlock(void *arg)
{
	struct thread_ctx *ctx = (struct thread_ctx *)arg;
	if (!ctx) return NULL;

	for (int i = 0; i < g_conf_iterations; ++i) {
		spin_lock(ctx->spinlock);
		(*ctx->shared_counter)++;
		
		/* Ensure dummy workload is not optimized away */
		for (int j = 0; j < g_conf_load_loops; ++j)
			asm volatile("nop" : : : "memory");
			
		spin_unlock(ctx->spinlock);
	}
	return NULL;
}

void *task_mutex(void *arg)
{
	struct thread_ctx *ctx = (struct thread_ctx *)arg;
	if (!ctx) return NULL;

	for (int i = 0; i < g_conf_iterations; ++i) {
		pthread_mutex_lock(ctx->mutex);
		(*ctx->shared_counter)++;
		
		for (int j = 0; j < g_conf_load_loops; ++j)
			asm volatile("nop" : : : "memory");
			
		pthread_mutex_unlock(ctx->mutex);
	}
	return NULL;
}
