#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include "./spinlock_test.h"

/* Global Configuration Variables */
int g_conf_iterations = DEFAULT_ITERATIONS;
int g_conf_load_loops = DEFAULT_LOAD_LOOPS;
int g_conf_nthreads   = DEFAULT_NTHREADS;
int g_conf_spin_min   = DEFAULT_SPIN_MIN;
int g_conf_spin_max   = DEFAULT_SPIN_MAX;

/* Runtime detected system information */
long g_sys_cache_line_size = 0;

static void detect_system_topology(void)
{
	g_sys_cache_line_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	if (g_sys_cache_line_size <= 0)
		g_sys_cache_line_size = 64;

	if (g_sys_cache_line_size != COMPILE_TIME_CACHE_LINE_SIZE) {
		fprintf(stderr,
			"\n[WARNING] Cache Line Size Mismatch!\n"
			"  Detected: %ld bytes\n"
			"  Compiled: %d bytes\n\n",
			g_sys_cache_line_size, COMPILE_TIME_CACHE_LINE_SIZE);
	}
}

static void print_help(const char *prog_name)
{
	printf("Usage: %s [options]\n"
	       "Options:\n"
	       "  -t <threads>   Number of threads (default: %d)\n"
	       "  -i <iters>     Iterations per thread (default: %d)\n"
	       "  -l <loops>     Busy work loops (nop) (default: %d)\n"
	       "  -m <min_spin>  Min spin backoff (default: %d)\n"
	       "  -M <max_spin>  Max spin backoff (default: %d)\n"
	       "  -h             Show this help and exit\n",
	       prog_name, DEFAULT_NTHREADS, DEFAULT_ITERATIONS,
	       DEFAULT_LOAD_LOOPS, DEFAULT_SPIN_MIN, DEFAULT_SPIN_MAX);
}

static void parse_args(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "t:i:l:m:M:h")) != -1) {
		switch (opt) {
		case 't': g_conf_nthreads = atoi(optarg); break;
		case 'i': g_conf_iterations = atoi(optarg); break;
		case 'l': g_conf_load_loops = atoi(optarg); break;
		case 'm': g_conf_spin_min = atoi(optarg); break;
		case 'M': g_conf_spin_max = atoi(optarg); break;
		case 'h': print_help(argv[0]); exit(0);
		default:  print_help(argv[0]); exit(1);
		}
	}
}

static double run_benchmark(const char *name, void *(*task_routine)(void *))
{
	pthread_t *threads;
	struct timespec start, end;
	struct thread_ctx ctx;
	int local_counter = 0;
	spinlock_t local_spinlock;
	pthread_mutex_t local_mutex;
	long i;
	double elapsed_ms;
	long long expected = (long long)g_conf_iterations * g_conf_nthreads;

	spin_init(&local_spinlock);
	pthread_mutex_init(&local_mutex, NULL);

	ctx.shared_counter = &local_counter;
	ctx.spinlock = &local_spinlock;
	ctx.mutex = &local_mutex;

	threads = malloc(sizeof(pthread_t) * g_conf_nthreads);
	if (!threads) {
		perror("malloc failed");
		exit(1);
	}

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (i = 0; i < g_conf_nthreads; i++)
		pthread_create(&threads[i], NULL, task_routine, &ctx);

	for (i = 0; i < g_conf_nthreads; i++)
		pthread_join(threads[i], NULL);

	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed_ms = calc_time_diff_ms(&start, &end);

	printf("[ %-22s ]\n", name);
	printf("  - Elapsed Time : %10.3f ms\n", elapsed_ms);
	printf("  - Atomic Count : %10d / %lld (%s)\n", 
	       local_counter, expected, (local_counter == expected) ? "OK" : "FAIL");

	pthread_mutex_destroy(&local_mutex);
	free(threads);

	return elapsed_ms;
}

int main(int argc, char *argv[])
{
	double t_spin, t_mutex;

	detect_system_topology();
	parse_args(argc, argv);

	printf("\n--- SPINLOCK BENCHMARK SUITE START ---\n");
	printf("System Info:\n");
	printf("  L1 Cache Line  : %ld bytes\n", g_sys_cache_line_size);
	printf("Configuration:\n");
	printf("  Threads        : %d\n", g_conf_nthreads);
	printf("  Iterations     : %d\n", g_conf_iterations);
	printf("  Workload (nop) : %d\n", g_conf_load_loops);
	printf("  Backoff Range  : %d ~ %d\n", g_conf_spin_min, g_conf_spin_max);
	printf("--------------------------------------\n\n");

	t_spin = run_benchmark("Custom Hybrid Spinlock", task_spinlock);
	printf("\n");
	t_mutex = run_benchmark("POSIX Mutex", task_mutex);

	printf("\n--------------------------------------\n");
	printf("FINAL RESULT:\n");
	printf("  Speedup Factor : %.2fx (Spinlock/Mutex)\n", t_mutex / t_spin);
	printf("  Winner         : %s\n", (t_spin < t_mutex) ? "Custom Spinlock" : "POSIX Mutex");
	printf("--- BENCHMARK SUITE END ---\n\n");

	return 0;
}
