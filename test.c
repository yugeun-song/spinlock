#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>

#include "./spinlock_test.h"

#define MIN_THREADS 1
#define MAX_THREADS 1024
#define MIN_ITERS 1
#define MAX_ITERS INT_MAX
#define MIN_LOAD 0
#define MAX_LOAD INT_MAX
#define MIN_BACKOFF 1
#define MAX_BACKOFF (INT_MAX / 2)

/*
 * Quiescent gap inserted right before each measured benchmark so the system
 * settles (scheduler drains, CPU frequency relaxes, the previous lock's cache
 * footprint dissipates) and one lock type cannot bias the next.
 */
#define SETTLE_DELAY_MS 100

/*
 * Cache-line-isolated wrapper for the POSIX spinlock. pthread_spinlock_t is a
 * bare word, whereas our custom spinlock_t is padded to its own cache line. To
 * compare the two locks fairly, the POSIX lock must get the same isolation:
 * without this padding it could share a cache line with the contended counter
 * on the benchmark stack, so the lock holder's counter write would invalidate
 * the waiters' cached copy of the lock word on every critical section — extra
 * coherence traffic that has nothing to do with the lock itself and would
 * unfairly penalise the POSIX lock. Mirrors the spinlock_t layout exactly.
 */
typedef struct {
    pthread_spinlock_t lock;
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(pthread_spinlock_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) isolated_pspin_t;

int g_conf_iterations = DEFAULT_ITERATIONS;
int g_conf_load_loops = DEFAULT_LOAD_LOOPS;
int g_conf_nthreads = DEFAULT_NTHREADS;
int g_conf_spin_min = DEFAULT_SPIN_MIN;
int g_conf_spin_max = DEFAULT_SPIN_MAX;

long g_sys_cache_line_size = 0;

static void detect_system_topology(void)
{
    g_sys_cache_line_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (g_sys_cache_line_size <= 0) {
        g_sys_cache_line_size = 64;
    }

    if (g_sys_cache_line_size != CACHE_LINE_SIZE) {
        fprintf(stderr,
                "\n[WARNING] Cache Line Size Mismatch!\n"
                "  Detected: %ld bytes\n"
                "  Compiled: %d bytes\n\n",
                g_sys_cache_line_size, CACHE_LINE_SIZE);
    }
}

static inline void print_help(const char *prog_name)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "Options:\n"
            "  -t <threads>    Number of threads (Range: %d-%d, default: %d)\n"
            "  -i <iters>      Iterations per thread (Range: %d-%d, default: %d)\n"
            "  -l <loops>      Dummy Task Count (Mock NOP) (Range: %d-%d, default: %d)\n"
            "  -m <min_spin>   Min spin backoff (Range: %d-%d, default: %d)\n"
            "  -M <max_spin>   Max spin backoff (Range: %d-%d, default: %d)\n"
            "  -h              Show this help and exit\n",
            prog_name, MIN_THREADS, MAX_THREADS, DEFAULT_NTHREADS, MIN_ITERS, MAX_ITERS,
            DEFAULT_ITERATIONS, MIN_LOAD, MAX_LOAD, DEFAULT_LOAD_LOOPS, MIN_BACKOFF, MAX_BACKOFF,
            DEFAULT_SPIN_MIN, MIN_BACKOFF, MAX_BACKOFF, DEFAULT_SPIN_MAX);
}

static int safe_strtoi(const char *str, int min, int max, const char *name)
{
    char *endptr;
    errno = 0;
    const long val = strtol(str, &endptr, 10);

    const int range_err = (errno == ERANGE) && (val == LONG_MAX || val == LONG_MIN);
    const int other_err = (errno != 0) && (val == 0);
    if (range_err || other_err) {
        perror("strtol");
        exit(EXIT_FAILURE);
    }

    if (endptr == str || *endptr != '\0') {
        fprintf(stderr, "Error: Invalid integer for %s: '%s'\n", name, str);
        exit(EXIT_FAILURE);
    }

    if (val < min || val > max) {
        fprintf(stderr, "Error: %s must be between %d and %d. Got: %ld\n", name, min, max, val);
        exit(EXIT_FAILURE);
    }

    return (int)val;
}

static void parse_args(int argc, char *argv[])
{
    opterr = 0;

    for (int i = 1; i < argc; ++i) {
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

    int opt;
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
            if (optopt == 't' || optopt == 'i' || optopt == 'l' || optopt == 'm' || optopt == 'M') {
                fprintf(stderr, "Error: Option '-%c' requires an argument.\n", optopt);
            } else {
                fprintf(stderr, "Error: Unknown option '-%c'.\n", optopt);
            }
            print_help(argv[0]);
            exit(EXIT_FAILURE);
        default:
            exit(EXIT_FAILURE);
        }
    }

    if (g_conf_spin_max < g_conf_spin_min) {
        fprintf(stderr, "Error: Max spin backoff (%d) < Min spin backoff (%d)\n", g_conf_spin_max, g_conf_spin_min);
        exit(EXIT_FAILURE);
    }

    if (optind < argc) {
        fprintf(stderr, "Error: Unexpected positional argument '%s'\n", argv[optind]);
        print_help(argv[0]);
        exit(EXIT_FAILURE);
    }
}

static void settle_between_tests(void)
{
    const struct timespec settle_ts = {
        .tv_sec = SETTLE_DELAY_MS / 1000,
        .tv_nsec = (long)(SETTLE_DELAY_MS % 1000) * 1000000L
    };
    nanosleep(&settle_ts, NULL);
}

static double run_benchmark(const char *name, void *(*task_routine)(void *))
{
    isolated_pspin_t local_pspin;
    spinlock_t local_spinlock;
    pthread_barrier_t barrier;
    struct thread_ctx ctx;
    struct timespec start, end;
    long long local_counter = 0;

    settle_between_tests();

    spin_init(&local_spinlock);
    if (pthread_spin_init(&local_pspin.lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        perror("pthread_spin_init");
        exit(EXIT_FAILURE);
    }
    if (pthread_barrier_init(&barrier, NULL, g_conf_nthreads + 1) != 0) {
        perror("pthread_barrier_init");
        pthread_spin_destroy(&local_pspin.lock);
        exit(EXIT_FAILURE);
    }

    ctx.shared_counter = &local_counter;
    ctx.spinlock = &local_spinlock;
    ctx.pthread_spin = &local_pspin.lock;
    ctx.barrier = &barrier;

    pthread_t *threads = calloc(g_conf_nthreads, sizeof(*threads));
    if (!threads) {
        perror("calloc");
        pthread_barrier_destroy(&barrier);
        pthread_spin_destroy(&local_pspin.lock);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < g_conf_nthreads; ++i) {
        const int ret = pthread_create(&threads[i], NULL, task_routine, &ctx);
        if (ret != 0) {
            fprintf(stderr, "Error: pthread_create failed at index %d: %s\n", i, strerror(ret));
            free(threads);
            exit(EXIT_FAILURE);
        }
    }

    pthread_barrier_wait(&barrier);
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < g_conf_nthreads; ++i) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    const double elapsed_ms = calc_time_diff_ms(&start, &end);
    const long long expected = (long long)g_conf_iterations * g_conf_nthreads;

    const char *status = (local_counter == expected) ? "OK" : "FAIL";
    printf("[ %-22s ]\n"
           "  - Elapsed Time : %10.3f ms\n"
           "  - Atomic Count : %10lld / %lld (%s)\n",
           name, elapsed_ms, local_counter, expected, status);

    pthread_barrier_destroy(&barrier);
    pthread_spin_destroy(&local_pspin.lock);
    free(threads);

    return elapsed_ms;
}

static void run_warmup(void)
{
    spinlock_t local_spinlock;
    pthread_barrier_t barrier;
    struct thread_ctx ctx;
    long long local_counter = 0;
    const int saved_iters = g_conf_iterations;

    const int warmup_iters = saved_iters / 10;
    g_conf_iterations = (warmup_iters > 0) ? warmup_iters : 1;

    spin_init(&local_spinlock);
    pthread_barrier_init(&barrier, NULL, g_conf_nthreads + 1);

    ctx.shared_counter = &local_counter;
    ctx.spinlock = &local_spinlock;
    ctx.pthread_spin = NULL;
    ctx.barrier = &barrier;

    pthread_t *threads = calloc(g_conf_nthreads, sizeof(*threads));
    if (!threads) {
        g_conf_iterations = saved_iters;
        pthread_barrier_destroy(&barrier);
        return;
    }

    for (int i = 0; i < g_conf_nthreads; ++i) {
        if (pthread_create(&threads[i], NULL, task_spinlock, &ctx) != 0) {
            free(threads);
            g_conf_iterations = saved_iters;
            pthread_barrier_destroy(&barrier);
            return;
        }
    }

    pthread_barrier_wait(&barrier);
    for (int i = 0; i < g_conf_nthreads; ++i) {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&barrier);
    free(threads);
    g_conf_iterations = saved_iters;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    detect_system_topology();
    parse_args(argc, argv);

    /*
     * Best-effort: pin the resident working set into RAM so a page fault or
     * swap-out cannot perturb a measurement. MCL_CURRENT only (not MCL_FUTURE):
     * locking future allocations would also lock every worker thread stack and
     * exceed RLIMIT_MEMLOCK on typical hosts, failing pthread_create. A failure
     * here is non-fatal and only means results may carry slightly more noise.
     */
    const char *mlock_status = "locked (MCL_CURRENT)";
    if (mlockall(MCL_CURRENT) != 0) {
        mlock_status = "unavailable";
        fprintf(stderr,
                "[WARNING] mlockall(MCL_CURRENT) failed: %s\n"
                "  Measurements may carry more variance from page faults.\n\n",
                strerror(errno));
    }

    printf("\n--- SPINLOCK BENCHMARK SUITE START ---\n"
           "System Info:\n"
           "  L1 Cache Line  : %ld bytes\n"
           "Configuration:\n"
           "  Threads        : %d\n"
           "  Iterations     : %d\n"
           "  Dummy Tasks    : %d\n"
           "  Backoff Range  : %d ~ %d\n"
           "  Settle Delay   : %d ms (between tests)\n"
           "  Memory Lock    : %s\n"
           "--------------------------------------\n\n",
           g_sys_cache_line_size, g_conf_nthreads, g_conf_iterations, g_conf_load_loops,
           g_conf_spin_min, g_conf_spin_max, SETTLE_DELAY_MS, mlock_status);

    run_warmup();

    const double t_spin = run_benchmark("Custom Hybrid Spinlock", task_spinlock);
    printf("\n");
    const double t_pthread = run_benchmark("POSIX Spinlock", task_pthread_spin);

    const double speedup = t_pthread / t_spin;
    const char *winner = (t_spin < t_pthread) ? "Custom Spinlock" : "POSIX Spinlock";
    printf("\n--------------------------------------\n"
           "FINAL RESULT:\n"
           "  Speedup Factor : %.2fx\n"
           "  Winner         : %s\n"
           "--- BENCHMARK SUITE END ---\n\n",
           speedup, winner);

    return EXIT_SUCCESS;
}
