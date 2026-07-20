#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>

#include "./spinlock_test.h"

/*
 * Cache-line-isolated wrapper for the POSIX spinlock. pthread_spinlock_t is a
 * bare word, whereas our custom spinlock_ttas_t is padded to its own cache line. To
 * compare the two locks fairly, the POSIX lock must get the same isolation:
 * without this padding it could share a cache line with the contended counter
 * on the benchmark stack, so the lock holder's counter write would invalidate
 * the waiters' cached copy of the lock word on every critical section — extra
 * coherence traffic that has nothing to do with the lock itself and would
 * unfairly penalise the POSIX lock. Mirrors the spinlock_ttas_t layout exactly.
 */
typedef struct {
    pthread_spinlock_t lock;
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(pthread_spinlock_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) isolated_pspin_t;

struct thread_ctx {
    long long *shared_counter;
    spinlock_ttas_t *spinlock_ttas;
    spinlock_mcs_t *spinlock_mcs;
    pthread_spinlock_t *pthread_spin;
    pthread_barrier_t *barrier;
};

int g_conf_spin_min = DEFAULT_SPIN_MIN;
int g_conf_spin_max = DEFAULT_SPIN_MAX;

static int g_conf_iterations = DEFAULT_ITERATIONS;
static int g_conf_load_loops = DEFAULT_LOAD_LOOPS;
static int g_conf_nthreads = DEFAULT_NTHREADS;

static long g_sys_cache_line_size = 0;
static const char *g_mlock_status = "unavailable";

/*
 * Optional CPU pin set parsed from -C (e.g. "0-3" or "0,2,4"). When set, worker
 * i is bound to g_conf_cpus[i % g_conf_ncpus], giving deterministic
 * one-thread-per-core placement. On heterogeneous machines (P/E cores) this is
 * the single largest lever on measurement variance: without it the scheduler
 * scatters workers across fast and slow cores run to run, and the numbers swing.
 */
static int *g_conf_cpus = NULL;
static int g_conf_ncpus = 0;

/*
 * Which contenders to run (-K). Defaults to all three. The knob exists mainly
 * so a sweep can drop the MCS lock at thread counts that oversubscribe the
 * cores: a strict FIFO queue lock convoys there (a preempted successor stalls
 * the whole queue) and would otherwise dominate wall-clock with timeouts.
 */
static int g_run_ttas = 1;
static int g_run_mcs = 1;
static int g_run_pspin = 1;

static double calc_time_diff_ms(const struct timespec *start, const struct timespec *end)
{
    if (!start || !end) {
        return 0.0;
    }

    const long long sec_diff = end->tv_sec - start->tv_sec;
    const long long nsec_diff = end->tv_nsec - start->tv_nsec;
    const long long elapsed_ns = sec_diff * 1000000000LL + nsec_diff;

    return (double)elapsed_ns / 1000000.0;
}

static void *task_spinlock_ttas(void *arg)
{
    struct thread_ctx *ctx = (struct thread_ctx *)arg;

    if (!ctx) {
        return NULL;
    }

    const int iters = g_conf_iterations;
    const int loops = g_conf_load_loops;
    long long *const counter = ctx->shared_counter;
    spinlock_ttas_t *const lock = ctx->spinlock_ttas;

    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < iters; ++i) {
        spin_lock_ttas(lock);
        *counter += 1;

        for (int j = 0; j < loops; ++j) {
            asm volatile("nop" : : : "memory");
        }

        spin_unlock_ttas(lock);
    }

    return NULL;
}

static void *task_spinlock_mcs(void *arg)
{
    struct thread_ctx *ctx = (struct thread_ctx *)arg;

    if (!ctx) {
        return NULL;
    }

    const int iters = g_conf_iterations;
    const int loops = g_conf_load_loops;
    long long *const counter = ctx->shared_counter;
    spinlock_mcs_t *const lock = ctx->spinlock_mcs;

    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < iters; ++i) {
        spin_lock_mcs(lock);
        *counter += 1;

        for (int j = 0; j < loops; ++j) {
            asm volatile("nop" : : : "memory");
        }

        spin_unlock_mcs(lock);
    }

    return NULL;
}

static void *task_pthread_spin(void *arg)
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

static void parse_klist(const char *str)
{
    g_run_ttas = g_run_mcs = g_run_pspin = 0;

    const char *p = str;
    while (*p) {
        const char *comma = strchr(p, ',');
        const size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == 4 && strncmp(p, "ttas", 4) == 0) {
            g_run_ttas = 1;
        } else if (len == 3 && strncmp(p, "mcs", 3) == 0) {
            g_run_mcs = 1;
        } else if (len == 5 && strncmp(p, "pspin", 5) == 0) {
            g_run_pspin = 1;
        } else {
            fprintf(stderr, "Error: Unknown lock in -K: '%.*s' (use ttas,mcs,pspin)\n",
                    (int)len, p);
            exit(EXIT_FAILURE);
        }
        if (!comma) {
            break;
        }
        p = comma + 1;
    }

    if (!g_run_ttas && !g_run_mcs && !g_run_pspin) {
        fprintf(stderr, "Error: -K selected no locks\n");
        exit(EXIT_FAILURE);
    }
}

/*
 * Parse a CPU list ("0-3", "0,2,4", "0-1,4-5") into g_conf_cpus/g_conf_ncpus.
 * Rejects malformed input the same way the numeric options do, so a typo fails
 * loudly instead of silently pinning to the wrong set.
 */
static void parse_cpulist(const char *str)
{
    long max_cpu = sysconf(_SC_NPROCESSORS_CONF);
    if (max_cpu <= 0 || max_cpu > CPU_SETSIZE) {
        /* Clamp to the fixed cpu_set_t width so CPU_SET can never index past it. */
        max_cpu = CPU_SETSIZE;
    }

    int cap = 8;
    g_conf_cpus = malloc(cap * sizeof(*g_conf_cpus));
    if (!g_conf_cpus) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    g_conf_ncpus = 0;

    const char *p = str;
    while (*p) {
        char *endptr;
        errno = 0;
        long lo = strtol(p, &endptr, 10);
        if (endptr == p || errno != 0) {
            fprintf(stderr, "Error: Invalid CPU list '%s'\n", str);
            exit(EXIT_FAILURE);
        }
        long hi = lo;
        p = endptr;
        if (*p == '-') {
            errno = 0;
            hi = strtol(p + 1, &endptr, 10);
            if (endptr == p + 1 || errno != 0) {
                fprintf(stderr, "Error: Invalid CPU list '%s'\n", str);
                exit(EXIT_FAILURE);
            }
            p = endptr;
        }
        if (lo < 0 || hi < lo || hi >= max_cpu) {
            fprintf(stderr, "Error: CPU list '%s' out of range 0-%ld\n", str, max_cpu - 1);
            exit(EXIT_FAILURE);
        }
        for (long c = lo; c <= hi; ++c) {
            if (g_conf_ncpus == cap) {
                cap *= 2;
                int *grown = realloc(g_conf_cpus, cap * sizeof(*g_conf_cpus));
                if (!grown) {
                    perror("realloc");
                    exit(EXIT_FAILURE);
                }
                g_conf_cpus = grown;
            }
            g_conf_cpus[g_conf_ncpus++] = (int)c;
        }
        if (*p == ',') {
            ++p;
        } else if (*p != '\0') {
            fprintf(stderr, "Error: Invalid CPU list '%s'\n", str);
            exit(EXIT_FAILURE);
        }
    }

    if (g_conf_ncpus == 0) {
        fprintf(stderr, "Error: Empty CPU list\n");
        exit(EXIT_FAILURE);
    }
}

/*
 * Pin the worker about to be created (index i) to a single core from the parsed
 * set via a thread attribute. A no-op when -C was not given. Failure is
 * non-fatal: the thread still runs, just unpinned.
 */
static void pin_worker_attr(pthread_attr_t *attr, int i)
{
    if (g_conf_ncpus == 0) {
        return;
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(g_conf_cpus[i % g_conf_ncpus], &set);
    pthread_attr_setaffinity_np(attr, sizeof(set), &set);
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
            "  -C <cpulist>    Pin workers to these cores round-robin, e.g. 0-3 or 0,2,4\n"
            "                  (deterministic placement; pass >= as many cores as threads\n"
            "                  from one homogeneous class to control P/E-core variance)\n"
            "  -K <locks>      Which contenders to run, comma-separated subset of\n"
            "                  ttas,mcs,pspin (default: all three)\n"
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
    spinlock_ttas_t local_spinlock;
    spinlock_mcs_t local_mcs;
    pthread_barrier_t barrier;
    struct thread_ctx ctx;
    struct timespec start, end;
    long long local_counter = 0;

    settle_between_tests();

    spin_init_ttas(&local_spinlock);
    spin_init_mcs(&local_mcs);
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
    ctx.spinlock_ttas = &local_spinlock;
    ctx.spinlock_mcs = &local_mcs;
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
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pin_worker_attr(&attr, i);
        const int ret = pthread_create(&threads[i], &attr, task_routine, &ctx);
        pthread_attr_destroy(&attr);
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

/*
 * Warm one contender: ramp CPU frequency, the thread pool, and that lock's
 * i-cache / branch predictors before it is measured. Every contender is warmed
 * with the same pinning as the real run so none is charged a cold first pass
 * (the previous version warmed only the custom lock, biasing whichever lock the
 * warmup touched — and the custom lock was measured first).
 */
static void run_warmup(void *(*task_routine)(void *))
{
    isolated_pspin_t local_pspin;
    spinlock_ttas_t local_spinlock;
    spinlock_mcs_t local_mcs;
    pthread_barrier_t barrier;
    struct thread_ctx ctx;
    long long local_counter = 0;
    const int saved_iters = g_conf_iterations;

    const int warmup_iters = saved_iters / 10;
    g_conf_iterations = (warmup_iters > 0) ? warmup_iters : 1;

    spin_init_ttas(&local_spinlock);
    spin_init_mcs(&local_mcs);
    if (pthread_spin_init(&local_pspin.lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        g_conf_iterations = saved_iters;
        return;
    }
    if (pthread_barrier_init(&barrier, NULL, g_conf_nthreads + 1) != 0) {
        g_conf_iterations = saved_iters;
        pthread_spin_destroy(&local_pspin.lock);
        return;
    }

    ctx.shared_counter = &local_counter;
    ctx.spinlock_ttas = &local_spinlock;
    ctx.spinlock_mcs = &local_mcs;
    ctx.pthread_spin = &local_pspin.lock;
    ctx.barrier = &barrier;

    pthread_t *threads = calloc(g_conf_nthreads, sizeof(*threads));
    if (!threads) {
        g_conf_iterations = saved_iters;
        pthread_barrier_destroy(&barrier);
        pthread_spin_destroy(&local_pspin.lock);
        return;
    }

    for (int i = 0; i < g_conf_nthreads; ++i) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pin_worker_attr(&attr, i);
        const int rc = pthread_create(&threads[i], &attr, task_routine, &ctx);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            /*
             * Workers 0..i-1 are already parked on the barrier, which needs
             * nthreads+1 participants. Destroying it here would be UB and would
             * orphan those threads on this soon-to-be-dead stack frame, so bail
             * the whole process as run_benchmark does on the same failure.
             */
            fprintf(stderr, "Error: warmup pthread_create failed at index %d: %s\n",
                    i, strerror(rc));
            free(threads);
            exit(EXIT_FAILURE);
        }
    }

    pthread_barrier_wait(&barrier);
    for (int i = 0; i < g_conf_nthreads; ++i) {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&barrier);
    pthread_spin_destroy(&local_pspin.lock);
    free(threads);
    g_conf_iterations = saved_iters;
}

void bench_detect_topology(void)
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

void bench_parse_args(int argc, char *argv[])
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
    while ((opt = getopt(argc, argv, "+t:i:l:m:M:C:K:")) != -1) {
        switch (opt) {
        case 't':
            g_conf_nthreads = safe_strtoi(optarg, MIN_THREADS, MAX_THREADS, "threads");
            break;
        case 'C':
            parse_cpulist(optarg);
            break;
        case 'K':
            parse_klist(optarg);
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
            if (optopt == 't' || optopt == 'i' || optopt == 'l' || optopt == 'm' || optopt == 'M' ||
                optopt == 'C' || optopt == 'K') {
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

/*
 * Best-effort: pin the resident working set into RAM so a page fault or
 * swap-out cannot perturb a measurement. MCL_CURRENT only (not MCL_FUTURE):
 * locking future allocations would also lock every worker thread stack and
 * exceed RLIMIT_MEMLOCK on typical hosts, failing pthread_create. A failure
 * here is non-fatal and only means results may carry slightly more noise.
 */
void bench_lock_memory(void)
{
    g_mlock_status = "locked (MCL_CURRENT)";
    if (mlockall(MCL_CURRENT) != 0) {
        g_mlock_status = "unavailable";
        fprintf(stderr,
                "[WARNING] mlockall(MCL_CURRENT) failed: %s\n"
                "  Measurements may carry more variance from page faults.\n\n",
                strerror(errno));
    }
}

void bench_print_config(void)
{
    char pin_desc[128];
    if (g_conf_ncpus > 0) {
        int off = snprintf(pin_desc, sizeof(pin_desc), "cpus");
        for (int i = 0; i < g_conf_ncpus && off < (int)sizeof(pin_desc) - 8; ++i) {
            off += snprintf(pin_desc + off, sizeof(pin_desc) - off, " %d", g_conf_cpus[i]);
        }
    } else {
        snprintf(pin_desc, sizeof(pin_desc),
                 "none (WARNING: P/E-core scheduling variance uncontrolled)");
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
           "  Pinning        : %s\n"
           "--------------------------------------\n\n",
           g_sys_cache_line_size, g_conf_nthreads, g_conf_iterations, g_conf_load_loops,
           g_conf_spin_min, g_conf_spin_max, SETTLE_DELAY_MS, g_mlock_status, pin_desc);
}

/* Warm every selected contender (MCS last: it can convoy at oversubscription). */
void bench_warmup_all(void)
{
    if (g_run_ttas) {
        run_warmup(task_spinlock_ttas);
    }
    if (g_run_pspin) {
        run_warmup(task_pthread_spin);
    }
    if (g_run_mcs) {
        run_warmup(task_spinlock_mcs);
    }
}

/*
 * Measure in the order TTAS, POSIX, MCS so that if MCS convoys under
 * oversubscription its slow (or timed-out) run cannot delay the others'
 * already-flushed output. Only the -K-selected contenders run.
 */
void bench_run_all(struct bench_results *results)
{
    if (!results) {
        return;
    }

    results->ttas_ms = -1.0;
    results->pspin_ms = -1.0;
    results->mcs_ms = -1.0;

    int printed = 0;
    if (g_run_ttas) {
        results->ttas_ms = run_benchmark("Custom TTAS Spinlock", task_spinlock_ttas);
        printed = 1;
    }
    if (g_run_pspin) {
        if (printed) {
            printf("\n");
        }
        results->pspin_ms = run_benchmark("POSIX Spinlock", task_pthread_spin);
        printed = 1;
    }
    if (g_run_mcs) {
        if (printed) {
            printf("\n");
        }
        results->mcs_ms = run_benchmark("Custom MCS Spinlock", task_spinlock_mcs);
    }
}

void bench_print_summary(const struct bench_results *results)
{
    if (!results) {
        return;
    }

    const char *winner = NULL;
    double best = 1e300;
    if (g_run_ttas && results->ttas_ms < best) {
        best = results->ttas_ms;
        winner = "Custom TTAS Spinlock";
    }
    if (g_run_mcs && results->mcs_ms < best) {
        best = results->mcs_ms;
        winner = "Custom MCS Spinlock";
    }
    if (g_run_pspin && results->pspin_ms < best) {
        best = results->pspin_ms;
        winner = "POSIX Spinlock";
    }

    printf("\n--------------------------------------\n"
           "FINAL RESULT:\n");
    if (g_run_ttas) {
        printf("  Custom TTAS    : %10.3f ms\n", results->ttas_ms);
    }
    if (g_run_mcs) {
        printf("  Custom MCS     : %10.3f ms\n", results->mcs_ms);
    }
    if (g_run_pspin) {
        printf("  POSIX Spinlock : %10.3f ms\n", results->pspin_ms);
    }
    if (g_run_ttas && g_run_pspin) {
        printf("  TTAS / POSIX   : %.2fx\n", results->pspin_ms / results->ttas_ms);
    }
    printf("  Winner         : %s\n"
           "--- BENCHMARK SUITE END ---\n\n", winner);
}

void bench_cleanup(void)
{
    free(g_conf_cpus);
    g_conf_cpus = NULL;
    g_conf_ncpus = 0;
}
