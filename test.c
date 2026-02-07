#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "./spinlock_test.h"

#define MIN_THREADS     1
#define MAX_THREADS     1024
#define MIN_ITERS       1
#define MAX_ITERS       INT_MAX
#define MIN_LOAD        0
#define MAX_LOAD        INT_MAX
#define MIN_BACKOFF     0
#define MAX_BACKOFF     INT_MAX

int g_conf_iterations = DEFAULT_ITERATIONS;
int g_conf_load_loops = DEFAULT_LOAD_LOOPS;
int g_conf_nthreads   = DEFAULT_NTHREADS;
int g_conf_spin_min   = DEFAULT_SPIN_MIN;
int g_conf_spin_max   = DEFAULT_SPIN_MAX;

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
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  -t <threads>   Number of threads (Range: %d-%d, default: %d)\n"
        "  -i <iters>     Iterations per thread (Range: %d-%d, default: %d)\n"
        "  -l <loops>     Dummy Task Count (Mock NOP) (Range: %d-%d, default: %d)\n"
        "  -m <min_spin>  Min spin backoff (Range: %d-%d, default: %d)\n"
        "  -M <max_spin>  Max spin backoff (Range: %d-%d, default: %d)\n"
        "  -h             Show this help and exit\n",
        prog_name,
        MIN_THREADS, MAX_THREADS, DEFAULT_NTHREADS,
        MIN_ITERS, MAX_ITERS, DEFAULT_ITERATIONS,
        MIN_LOAD, MAX_LOAD, DEFAULT_LOAD_LOOPS,
        MIN_BACKOFF, MAX_BACKOFF, DEFAULT_SPIN_MIN,
        MIN_BACKOFF, MAX_BACKOFF, DEFAULT_SPIN_MAX);
}

static int safe_strtoi(const char *str, int min, int max, const char *name)
{
    char *endptr;
    errno = 0;
    long val = strtol(str, &endptr, 10);

    if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN)) || (errno != 0 && val == 0)) {
        perror("strtol");
        exit(EXIT_FAILURE);
    }

    if (endptr == str || *endptr != '\0') {
        fprintf(stderr, "Error: Invalid integer for %s: '%s'\n", name, str);
        exit(EXIT_FAILURE);
    }

    if (val < min || val > max) {
        fprintf(stderr, "Error: %s must be between %d and %d. Got: %ld\n",
            name, min, max, val);
        exit(EXIT_FAILURE);
    }

    return (int)val;
}

static void parse_args(int argc, char *argv[])
{
    int opt;
    opterr = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            if (argc > 2) {
                fprintf(stderr, "Error: -h cannot be combined with other options.\n");
                print_help(argv[0]);
                exit(EXIT_FAILURE);
            }
            print_help(argv[0]);
            exit(EXIT_SUCCESS);
        }
    }

    while ((opt = getopt(argc, argv, "+t:i:l:m:M:")) != -1) {
        switch (opt) {
        case 't':
            g_conf_nthreads = safe_strtoi(optarg, MIN_THREADS, MAX_THREADS, "threads");
            break;
        case 'i':
            g_conf_iterations = safe_strtoi(optarg, MIN_ITERS, MAX_ITERS, "iterations");
            break;
        case 'l':
            g_conf_load_loops = safe_strtoi(optarg, MIN_LOAD, MAX_LOAD, "load_loops");
            break;
        case 'm':
            g_conf_spin_min = safe_strtoi(optarg, MIN_BACKOFF, MAX_BACKOFF, "spin_min");
            break;
        case 'M':
            g_conf_spin_max = safe_strtoi(optarg, MIN_BACKOFF, MAX_BACKOFF, "spin_max");
            break;
        case '?':
            if (optopt == 't' || optopt == 'i' || optopt == 'l' || optopt == 'm' || optopt == 'M')
                fprintf(stderr, "Error: Option '-%c' requires an argument.\n", optopt);
            else
                fprintf(stderr, "Error: Unknown option '-%c'.\n", optopt);
            print_help(argv[0]);
            exit(EXIT_FAILURE);
        default:
            exit(EXIT_FAILURE);
        }
    }

    if (g_conf_spin_max < g_conf_spin_min) {
        fprintf(stderr, "Error: Max spin backoff (%d) < Min spin backoff (%d)\n",
            g_conf_spin_max, g_conf_spin_min);
        exit(EXIT_FAILURE);
    }

    if (optind < argc) {
        fprintf(stderr, "Error: Unexpected positional argument '%s'\n", argv[optind]);
        print_help(argv[0]);
        exit(EXIT_FAILURE);
    }
}

static double run_benchmark(const char *name, void *(*task_routine)(void *))
{
    pthread_t *threads = NULL;
    pthread_mutex_t local_mutex;
    spinlock_t local_spinlock;
    struct thread_ctx ctx;
    struct timespec start, end;
    int local_counter = 0;
    
    spin_init(&local_spinlock);
    if (pthread_mutex_init(&local_mutex, NULL) != 0) {
        perror("pthread_mutex_init");
        exit(EXIT_FAILURE);
    }

    ctx.shared_counter = &local_counter;
    ctx.spinlock = &local_spinlock;
    ctx.mutex = &local_mutex;

    threads = malloc(sizeof(pthread_t) * g_conf_nthreads);
    if (!threads) {
        perror("malloc");
        pthread_mutex_destroy(&local_mutex);
        exit(EXIT_FAILURE);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (long i = 0; i < g_conf_nthreads; ++i) {
        int ret = pthread_create(&threads[i], NULL, task_routine, &ctx);
        if (ret != 0) {
            fprintf(stderr, "Error: pthread_create failed at index %ld: %s\n", i, strerror(ret));
            for (long k = 0; k < i; ++k) pthread_join(threads[k], NULL);
            goto err_cleanup;
        }
    }

    for (int i = 0; i < g_conf_nthreads; ++i)
        pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = calc_time_diff_ms(&start, &end);
    long long expected = (long long)g_conf_iterations * g_conf_nthreads;

    printf("[ %-22s ]\n"
           "  - Elapsed Time : %10.3f ms\n"
           "  - Atomic Count : %10d / %lld (%s)\n",
           name, elapsed_ms, local_counter, expected, 
           (local_counter == expected) ? "OK" : "FAIL");

    pthread_mutex_destroy(&local_mutex);
    free(threads);
    return elapsed_ms;

err_cleanup:
    pthread_mutex_destroy(&local_mutex);
    free(threads);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    detect_system_topology();
    parse_args(argc, argv);

    printf("\n--- SPINLOCK BENCHMARK SUITE START ---\n"
           "System Info:\n"
           "  L1 Cache Line  : %ld bytes\n"
           "Configuration:\n"
           "  Threads        : %d\n"
           "  Iterations     : %d\n"
           "  Dummy Tasks    : %d\n"
           "  Backoff Range  : %d ~ %d\n"
           "--------------------------------------\n\n",
           g_sys_cache_line_size, g_conf_nthreads, g_conf_iterations,
           g_conf_load_loops, g_conf_spin_min, g_conf_spin_max);

    double t_spin = run_benchmark("Custom Hybrid Spinlock", task_spinlock);
    printf("\n");
    double t_mutex = run_benchmark("POSIX Mutex", task_mutex);

    printf("\n--------------------------------------\n"
           "FINAL RESULT:\n"
           "  Speedup Factor : %.2fx\n"
           "  Winner         : %s\n"
           "--- BENCHMARK SUITE END ---\n\n",
           t_mutex / t_spin, (t_spin < t_mutex) ? "Custom Spinlock" : "POSIX Mutex");

    return EXIT_SUCCESS;
}
