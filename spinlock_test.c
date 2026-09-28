#define _GNU_SOURCE

#include "spinlock_test.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/prctl.h>

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
} __attribute__((aligned(CACHE_LINE_SIZE))) isolated_posix_spinlock_t;

typedef struct {
    pthread_mutex_t lock;
    char cache_line_padding[CACHE_LINE_SIZE - sizeof(pthread_mutex_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) isolated_posix_mutex_t;

struct worker_context {
    long long *shared_counter;
    spinlock_ttas_t *ttas_lock;
    spinlock_mcs_t *mcs_lock;
    pthread_spinlock_t *posix_spinlock;
    pthread_mutex_t *posix_mutex;
    pthread_barrier_t *start_barrier;
    struct timespec loop_begin_time;
    struct timespec loop_done_time;
};

struct contender {
    const char *cli_key;
    const char *display_name;
    const char *summary_label;
    void *(*worker_routine)(void *);
    int is_selected;
};

struct nop_calibration {
    int loop_count;
    double nanoseconds_per_loop;
};

static int g_conf_iterations = DEFAULT_ITERATIONS;
static int g_conf_workload_nop_loops = DEFAULT_WORKLOAD_NOP_LOOPS;
static int g_conf_thread_count = DEFAULT_THREAD_COUNT;
static int g_conf_timerslack_ns = -1;

static long g_detected_cache_line_size = 0;
static double g_measured_nop_ns = 0.0;
static const char *g_memory_lock_status = "unavailable";
static int g_affinity_warning_printed = 0;

/*
 * Optional CPU pin set parsed from -C (e.g. "0-3" or "0,2,4"). When set, worker
 * i is bound to g_conf_pinned_cpu_ids[i % g_conf_pinned_cpu_count], giving
 * deterministic one-thread-per-core placement. On heterogeneous machines (P/E
 * cores) this is the single largest lever on measurement variance: without it
 * the scheduler scatters workers across fast and slow cores run to run, and the
 * numbers swing.
 */
static int *g_conf_pinned_cpu_ids = NULL;
static int g_conf_pinned_cpu_count = 0;

static double elapsed_milliseconds_between(const struct timespec *begin_time, const struct timespec *end_time)
{
    if (!begin_time || !end_time) {
        return 0.0;
    }

    const long long seconds_difference = end_time->tv_sec - begin_time->tv_sec;
    const long long nanoseconds_difference = end_time->tv_nsec - begin_time->tv_nsec;
    const long long elapsed_nanoseconds = (seconds_difference * 1000000000LL) + nanoseconds_difference;

    return (double)elapsed_nanoseconds / 1000000.0;
}

static inline void run_dummy_workload(int nop_loop_count)
{
    for (int nop_index = 0; nop_index < nop_loop_count; ++nop_index) {
        asm volatile("nop" : : : "memory");
    }
}

static void *worker_routine_ttas(void *raw_context)
{
    struct worker_context *context = (struct worker_context *)raw_context;

    if (!context) {
        return NULL;
    }

    const int iterations = g_conf_iterations;
    const int nop_loops = g_conf_workload_nop_loops;
    long long *const shared_counter = context->shared_counter;
    spinlock_ttas_t *const lock = context->ttas_lock;

    pthread_barrier_wait(context->start_barrier);
    clock_gettime(CLOCK_MONOTONIC, &context->loop_begin_time);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        spin_lock_ttas(lock);
        *shared_counter += 1;
        run_dummy_workload(nop_loops);
        spin_unlock_ttas(lock);
    }

    clock_gettime(CLOCK_MONOTONIC, &context->loop_done_time);
    return NULL;
}

static void *worker_routine_ttas_park(void *raw_context)
{
    struct worker_context *context = (struct worker_context *)raw_context;

    if (!context) {
        return NULL;
    }

    const int iterations = g_conf_iterations;
    const int nop_loops = g_conf_workload_nop_loops;
    long long *const shared_counter = context->shared_counter;
    spinlock_ttas_t *const lock = context->ttas_lock;

    pthread_barrier_wait(context->start_barrier);
    clock_gettime(CLOCK_MONOTONIC, &context->loop_begin_time);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        spin_lock_ttas_park(lock);
        *shared_counter += 1;
        run_dummy_workload(nop_loops);
        spin_unlock_ttas(lock);
    }

    clock_gettime(CLOCK_MONOTONIC, &context->loop_done_time);
    return NULL;
}

static void *worker_routine_mcs(void *raw_context)
{
    struct worker_context *context = (struct worker_context *)raw_context;

    if (!context) {
        return NULL;
    }

    const int iterations = g_conf_iterations;
    const int nop_loops = g_conf_workload_nop_loops;
    long long *const shared_counter = context->shared_counter;
    spinlock_mcs_t *const lock = context->mcs_lock;

    pthread_barrier_wait(context->start_barrier);
    clock_gettime(CLOCK_MONOTONIC, &context->loop_begin_time);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        spin_lock_mcs(lock);
        *shared_counter += 1;
        run_dummy_workload(nop_loops);
        spin_unlock_mcs(lock);
    }

    clock_gettime(CLOCK_MONOTONIC, &context->loop_done_time);
    return NULL;
}

static void *worker_routine_posix_spinlock(void *raw_context)
{
    struct worker_context *context = (struct worker_context *)raw_context;

    if (!context) {
        return NULL;
    }

    const int iterations = g_conf_iterations;
    const int nop_loops = g_conf_workload_nop_loops;
    long long *const shared_counter = context->shared_counter;
    pthread_spinlock_t *const lock = context->posix_spinlock;

    pthread_barrier_wait(context->start_barrier);
    clock_gettime(CLOCK_MONOTONIC, &context->loop_begin_time);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        pthread_spin_lock(lock);
        *shared_counter += 1;
        run_dummy_workload(nop_loops);
        pthread_spin_unlock(lock);
    }

    clock_gettime(CLOCK_MONOTONIC, &context->loop_done_time);
    return NULL;
}

static void *worker_routine_posix_mutex(void *raw_context)
{
    struct worker_context *context = (struct worker_context *)raw_context;

    if (!context) {
        return NULL;
    }

    const int iterations = g_conf_iterations;
    const int nop_loops = g_conf_workload_nop_loops;
    long long *const shared_counter = context->shared_counter;
    pthread_mutex_t *const lock = context->posix_mutex;

    pthread_barrier_wait(context->start_barrier);
    clock_gettime(CLOCK_MONOTONIC, &context->loop_begin_time);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        pthread_mutex_lock(lock);
        *shared_counter += 1;
        run_dummy_workload(nop_loops);
        pthread_mutex_unlock(lock);
    }

    clock_gettime(CLOCK_MONOTONIC, &context->loop_done_time);
    return NULL;
}

static void *calibration_routine_nop(void *raw_calibration)
{
    struct nop_calibration *calibration = (struct nop_calibration *)raw_calibration;
    struct timespec pass_begin_time;
    struct timespec pass_end_time;
    double best_pass_ms = 1e300;

    for (int pass_index = 0; pass_index < NOP_CALIBRATION_PASSES; ++pass_index) {
        clock_gettime(CLOCK_MONOTONIC, &pass_begin_time);
        run_dummy_workload(calibration->loop_count);
        clock_gettime(CLOCK_MONOTONIC, &pass_end_time);
        const double pass_ms = elapsed_milliseconds_between(&pass_begin_time, &pass_end_time);
        if (pass_ms < best_pass_ms) {
            best_pass_ms = pass_ms;
        }
    }

    calibration->nanoseconds_per_loop = best_pass_ms * 1e6 / calibration->loop_count;
    return NULL;
}

/*
 * Which contenders to run (-K). Defaults to all five, measured in table order
 * with MCS last: a strict FIFO queue lock convoys when threads oversubscribe
 * the cores (a preempted successor stalls the whole queue), so its slow run
 * cannot delay the others' already-flushed output.
 */
#define CONTENDER_KEY_LIST "ttas,ttas_park,pspin,pmutex,mcs"

static struct contender g_contenders[BENCH_LOCK_COUNT] = {
    [BENCH_LOCK_TTAS] = {"ttas", "Custom TTAS Spinlock", "Custom TTAS", worker_routine_ttas, 1},
    [BENCH_LOCK_TTAS_PARK] = {"ttas_park", "Custom TTAS Spin+Park", "TTAS Spin+Park", worker_routine_ttas_park, 1},
    [BENCH_LOCK_POSIX_SPINLOCK] = {"pspin", "POSIX Spinlock", "POSIX Spinlock", worker_routine_posix_spinlock, 1},
    [BENCH_LOCK_POSIX_MUTEX] = {"pmutex", "POSIX Mutex", "POSIX Mutex", worker_routine_posix_mutex, 1},
    [BENCH_LOCK_MCS] = {"mcs", "Custom MCS Spinlock", "Custom MCS", worker_routine_mcs, 1},
};

static void parse_contender_list(const char *argument_text)
{
    const char *cursor = argument_text;
    const char *next_comma;
    size_t token_length;
    int selected_any = 0;
    int contender_index;

    for (contender_index = 0; contender_index < BENCH_LOCK_COUNT; ++contender_index) {
        g_contenders[contender_index].is_selected = 0;
    }

    while (*cursor) {
        next_comma = strchr(cursor, ',');
        token_length = next_comma ? (size_t)(next_comma - cursor) : strlen(cursor);
        for (contender_index = 0; contender_index < BENCH_LOCK_COUNT; ++contender_index) {
            const char *cli_key = g_contenders[contender_index].cli_key;
            if (token_length == strlen(cli_key) && strncmp(cursor, cli_key, token_length) == 0) {
                g_contenders[contender_index].is_selected = 1;
                selected_any = 1;
                break;
            }
        }
        if (contender_index == BENCH_LOCK_COUNT) {
            fprintf(stderr, "Error: Unknown lock in -K: '%.*s' (use " CONTENDER_KEY_LIST ")\n",
                    (int)token_length, cursor);
            exit(EXIT_FAILURE);
        }
        if (!next_comma) {
            break;
        }
        cursor = next_comma + 1;
    }

    if (!selected_any) {
        fprintf(stderr, "Error: -K selected no locks\n");
        exit(EXIT_FAILURE);
    }
}

/*
 * Parse a CPU list ("0-3", "0,2,4", "0-1,4-5") into g_conf_pinned_cpu_ids and
 * g_conf_pinned_cpu_count. Rejects malformed input the same way the numeric
 * options do, so a typo fails loudly instead of silently pinning to the wrong
 * set. A repeated -C replaces the earlier list.
 */
static void parse_cpu_list(const char *argument_text)
{
    const char *cursor = argument_text;
    char *parse_end;
    int *grown_cpu_ids;
    long cpu_id_limit;
    long range_first_cpu;
    long range_last_cpu;
    long cpu_id;
    size_t cpu_ids_capacity = 8;

    cpu_id_limit = sysconf(_SC_NPROCESSORS_CONF);
    if (cpu_id_limit <= 0 || cpu_id_limit > CPU_SETSIZE) {
        /* Clamp to the fixed cpu_set_t width so CPU_SET can never index past it. */
        cpu_id_limit = CPU_SETSIZE;
    }

    free(g_conf_pinned_cpu_ids);
    g_conf_pinned_cpu_ids = malloc(cpu_ids_capacity * sizeof(*g_conf_pinned_cpu_ids));
    if (!g_conf_pinned_cpu_ids) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    g_conf_pinned_cpu_count = 0;

    while (*cursor) {
        errno = 0;
        range_first_cpu = strtol(cursor, &parse_end, 10);
        if (parse_end == cursor || errno != 0) {
            fprintf(stderr, "Error: Invalid CPU list '%s'\n", argument_text);
            exit(EXIT_FAILURE);
        }
        range_last_cpu = range_first_cpu;
        cursor = parse_end;
        if (*cursor == '-') {
            errno = 0;
            range_last_cpu = strtol(cursor + 1, &parse_end, 10);
            if (parse_end == cursor + 1 || errno != 0) {
                fprintf(stderr, "Error: Invalid CPU list '%s'\n", argument_text);
                exit(EXIT_FAILURE);
            }
            cursor = parse_end;
        }
        if (range_first_cpu < 0 || range_last_cpu < range_first_cpu || range_last_cpu >= cpu_id_limit) {
            fprintf(stderr, "Error: CPU list '%s' out of range 0-%ld\n", argument_text, cpu_id_limit - 1);
            exit(EXIT_FAILURE);
        }
        for (cpu_id = range_first_cpu; cpu_id <= range_last_cpu; ++cpu_id) {
            if ((size_t)g_conf_pinned_cpu_count == cpu_ids_capacity) {
                cpu_ids_capacity *= 2;
                grown_cpu_ids =
                    realloc(g_conf_pinned_cpu_ids, cpu_ids_capacity * sizeof(*g_conf_pinned_cpu_ids));
                if (!grown_cpu_ids) {
                    perror("realloc");
                    exit(EXIT_FAILURE);
                }
                g_conf_pinned_cpu_ids = grown_cpu_ids;
            }
            g_conf_pinned_cpu_ids[g_conf_pinned_cpu_count] = (int)cpu_id;
            ++g_conf_pinned_cpu_count;
        }
        if (*cursor == ',') {
            ++cursor;
        } else if (*cursor != '\0') {
            fprintf(stderr, "Error: Invalid CPU list '%s'\n", argument_text);
            exit(EXIT_FAILURE);
        }
    }

    if (g_conf_pinned_cpu_count == 0) {
        fprintf(stderr, "Error: Empty CPU list\n");
        exit(EXIT_FAILURE);
    }
}

/*
 * Pin the worker about to be created (index worker_index) to a single core from
 * the parsed set via a thread attribute. A no-op when -C was not given. Failure
 * is non-fatal: the thread still runs, just unpinned, and one warning says so.
 */
static void pin_worker_thread_attribute(pthread_attr_t *thread_attribute, int worker_index)
{
    cpu_set_t cpu_set;
    size_t pinned_cpu_id;
    int affinity_result;

    if (g_conf_pinned_cpu_count == 0) {
        return;
    }

    pinned_cpu_id = (size_t)g_conf_pinned_cpu_ids[worker_index % g_conf_pinned_cpu_count];
    CPU_ZERO(&cpu_set);
    CPU_SET(pinned_cpu_id, &cpu_set);
    affinity_result = pthread_attr_setaffinity_np(thread_attribute, sizeof(cpu_set), &cpu_set);
    if (affinity_result != 0 && !g_affinity_warning_printed) {
        g_affinity_warning_printed = 1;
        fprintf(stderr, "[WARNING] pthread_attr_setaffinity_np failed: %s; workers run unpinned\n",
                strerror(affinity_result));
    }
}

static inline void print_usage(const char *program_name)
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
            "                  " CONTENDER_KEY_LIST " (default: all five)\n"
            "  -S <ns>         Timer slack via prctl(PR_SET_TIMERSLACK); it stretches every\n"
            "                  nanosleep of ttas_park (0 restores the kernel default)\n"
            "  -h              Show this help and exit\n",
            program_name, MIN_THREAD_COUNT, MAX_THREAD_COUNT, DEFAULT_THREAD_COUNT, MIN_ITERATIONS,
            MAX_ITERATIONS, DEFAULT_ITERATIONS, MIN_WORKLOAD_NOP_LOOPS, MAX_WORKLOAD_NOP_LOOPS,
            DEFAULT_WORKLOAD_NOP_LOOPS, MIN_BACKOFF_PAUSES, MAX_BACKOFF_PAUSES, DEFAULT_SPIN_MIN,
            MIN_BACKOFF_PAUSES, MAX_BACKOFF_PAUSES, DEFAULT_SPIN_MAX);
}

static int parse_bounded_int(const char *argument_text, int minimum, int maximum, const char *option_name)
{
    char *parse_end;
    errno = 0;
    const long parsed_value = strtol(argument_text, &parse_end, 10);

    const int out_of_long_range = (errno == ERANGE) && (parsed_value == LONG_MAX || parsed_value == LONG_MIN);
    const int conversion_failed = (errno != 0) && (parsed_value == 0);
    if (out_of_long_range || conversion_failed) {
        perror("strtol");
        exit(EXIT_FAILURE);
    }

    if (parse_end == argument_text || *parse_end != '\0') {
        fprintf(stderr, "Error: Invalid integer for %s: '%s'\n", option_name, argument_text);
        exit(EXIT_FAILURE);
    }

    if (parsed_value < minimum || parsed_value > maximum) {
        fprintf(stderr, "Error: %s must be between %d and %d. Got: %ld\n", option_name, minimum,
                maximum, parsed_value);
        exit(EXIT_FAILURE);
    }

    return (int)parsed_value;
}

static void sleep_settle_gap(void)
{
    const struct timespec settle_duration = {.tv_sec = SETTLE_DELAY_MS / 1000,
                                             .tv_nsec = (long)(SETTLE_DELAY_MS % 1000) * 1000000L};
    nanosleep(&settle_duration, NULL);
}

static double run_contender(const char *display_name, void *(*worker_routine)(void *))
{
    isolated_posix_spinlock_t isolated_posix_spinlock;
    isolated_posix_mutex_t isolated_posix_mutex;
    spinlock_ttas_t ttas_lock;
    spinlock_mcs_t mcs_lock;
    pthread_barrier_t start_barrier;
    struct worker_context context_template;
    struct timespec suite_begin_time;
    struct timespec suite_end_time;
    long long shared_counter = 0;
    double earliest_done_ms = 1e300;
    double latest_done_ms = 0.0;

    if (display_name) {
        sleep_settle_gap();
    }

    spin_init_ttas(&ttas_lock);
    spin_init_mcs(&mcs_lock);
    if (pthread_spin_init(&isolated_posix_spinlock.lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        perror("pthread_spin_init");
        exit(EXIT_FAILURE);
    }
    if (pthread_mutex_init(&isolated_posix_mutex.lock, NULL) != 0) {
        perror("pthread_mutex_init");
        pthread_spin_destroy(&isolated_posix_spinlock.lock);
        exit(EXIT_FAILURE);
    }
    if (pthread_barrier_init(&start_barrier, NULL, (unsigned)g_conf_thread_count + 1) != 0) {
        perror("pthread_barrier_init");
        pthread_mutex_destroy(&isolated_posix_mutex.lock);
        pthread_spin_destroy(&isolated_posix_spinlock.lock);
        exit(EXIT_FAILURE);
    }

    context_template.shared_counter = &shared_counter;
    context_template.ttas_lock = &ttas_lock;
    context_template.mcs_lock = &mcs_lock;
    context_template.posix_spinlock = &isolated_posix_spinlock.lock;
    context_template.posix_mutex = &isolated_posix_mutex.lock;
    context_template.start_barrier = &start_barrier;

    pthread_t *worker_threads = calloc((size_t)g_conf_thread_count, sizeof(*worker_threads));
    struct worker_context *worker_contexts = calloc((size_t)g_conf_thread_count, sizeof(*worker_contexts));
    if (!worker_threads || !worker_contexts) {
        perror("calloc");
        pthread_barrier_destroy(&start_barrier);
        pthread_mutex_destroy(&isolated_posix_mutex.lock);
        pthread_spin_destroy(&isolated_posix_spinlock.lock);
        exit(EXIT_FAILURE);
    }

    for (int worker_index = 0; worker_index < g_conf_thread_count; ++worker_index) {
        pthread_attr_t thread_attribute;
        worker_contexts[worker_index] = context_template;
        pthread_attr_init(&thread_attribute);
        pin_worker_thread_attribute(&thread_attribute, worker_index);
        const int create_result = pthread_create(&worker_threads[worker_index], &thread_attribute,
                                                 worker_routine, &worker_contexts[worker_index]);
        pthread_attr_destroy(&thread_attribute);
        if (create_result != 0) {
            fprintf(stderr, "Error: pthread_create failed at index %d: %s\n", worker_index,
                    strerror(create_result));
            free(worker_threads);
            free(worker_contexts);
            exit(EXIT_FAILURE);
        }
    }

    pthread_barrier_wait(&start_barrier);
    clock_gettime(CLOCK_MONOTONIC, &suite_begin_time);

    for (int worker_index = 0; worker_index < g_conf_thread_count; ++worker_index) {
        pthread_join(worker_threads[worker_index], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &suite_end_time);
    const double elapsed_ms = elapsed_milliseconds_between(&suite_begin_time, &suite_end_time);
    const long long expected_count = (long long)g_conf_iterations * g_conf_thread_count;

    struct timespec earliest_begin_time = worker_contexts[0].loop_begin_time;
    for (int worker_index = 1; worker_index < g_conf_thread_count; ++worker_index) {
        if (elapsed_milliseconds_between(&earliest_begin_time,
                                         &worker_contexts[worker_index].loop_begin_time) < 0.0) {
            earliest_begin_time = worker_contexts[worker_index].loop_begin_time;
        }
    }
    for (int worker_index = 0; worker_index < g_conf_thread_count; ++worker_index) {
        const double done_ms =
            elapsed_milliseconds_between(&earliest_begin_time, &worker_contexts[worker_index].loop_done_time);
        earliest_done_ms = (done_ms < earliest_done_ms) ? done_ms : earliest_done_ms;
        latest_done_ms = (done_ms > latest_done_ms) ? done_ms : latest_done_ms;
    }

    if (display_name) {
        const char *count_status = (shared_counter == expected_count) ? "OK" : "FAIL";
        printf("[ %-22s ]\n"
               "  - Elapsed Time : %10.3f ms\n"
               "  - Atomic Count : %10lld / %lld (%s)\n"
               "  - Fairness     : min %10.3f ms, max %10.3f ms, min/max %.2f\n",
               display_name, elapsed_ms, shared_counter, expected_count, count_status, earliest_done_ms,
               latest_done_ms, (latest_done_ms > 0.0) ? earliest_done_ms / latest_done_ms : 1.0);
    }

    pthread_barrier_destroy(&start_barrier);
    pthread_spin_destroy(&isolated_posix_spinlock.lock);
    pthread_mutex_destroy(&isolated_posix_mutex.lock);
    free(worker_threads);
    free(worker_contexts);

    return elapsed_ms;
}

/*
 * Warm one contender: ramp CPU frequency, the thread pool, and that lock's
 * i-cache / branch predictors before it is measured. Every contender is warmed
 * with the same pinning as the real run so none is charged a cold first pass
 * (the previous version warmed only the custom lock, biasing whichever lock the
 * warmup touched — and the custom lock was measured first).
 */
static void run_warmup_pass(void *(*worker_routine)(void *))
{
    const int configured_iterations = g_conf_iterations;
    const int warmup_iterations = configured_iterations / 10;

    g_conf_iterations = (warmup_iterations > 0) ? warmup_iterations : 1;
    run_contender(NULL, worker_routine);
    g_conf_iterations = configured_iterations;
}

void bench_detect_topology(void)
{
    g_detected_cache_line_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (g_detected_cache_line_size <= 0) {
        g_detected_cache_line_size = 64;
    }

    if (g_detected_cache_line_size != CACHE_LINE_SIZE) {
        fprintf(stderr,
                "\n[WARNING] Cache Line Size Mismatch!\n"
                "  Detected: %ld bytes\n"
                "  Compiled: %d bytes\n\n",
                g_detected_cache_line_size, CACHE_LINE_SIZE);
    }
}

void bench_parse_args(int argc, char *argv[])
{
    opterr = 0;

    for (int argument_index = 1; argument_index < argc; ++argument_index) {
        if (strcmp(argv[argument_index], "-h") == 0) {
            if (argc > 2) {
                fprintf(stderr, "Error: -h cannot be combined with other options.\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
    }

    int option_char;
    while ((option_char = getopt(argc, argv, "+t:i:l:m:M:C:K:S:")) != -1) {
        switch (option_char) {
        case 't':
            g_conf_thread_count = parse_bounded_int(optarg, MIN_THREAD_COUNT, MAX_THREAD_COUNT, "threads");
            break;
        case 'C':
            parse_cpu_list(optarg);
            break;
        case 'K':
            parse_contender_list(optarg);
            break;
        case 'i':
            g_conf_iterations = parse_bounded_int(optarg, MIN_ITERATIONS, MAX_ITERATIONS, "iterations");
            break;
        case 'l':
            g_conf_workload_nop_loops =
                parse_bounded_int(optarg, MIN_WORKLOAD_NOP_LOOPS, MAX_WORKLOAD_NOP_LOOPS, "load_loops");
            break;
        case 'm':
            g_conf_spin_min = parse_bounded_int(optarg, MIN_BACKOFF_PAUSES, MAX_BACKOFF_PAUSES, "spin_min");
            break;
        case 'M':
            g_conf_spin_max = parse_bounded_int(optarg, MIN_BACKOFF_PAUSES, MAX_BACKOFF_PAUSES, "spin_max");
            break;
        case 'S':
            g_conf_timerslack_ns = parse_bounded_int(optarg, 0, INT_MAX, "timer_slack");
            break;
        case '?':
            if (optopt == 't' || optopt == 'i' || optopt == 'l' || optopt == 'm' || optopt == 'M' ||
                optopt == 'C' || optopt == 'K' || optopt == 'S') {
                fprintf(stderr, "Error: Option '-%c' requires an argument.\n", optopt);
            } else {
                fprintf(stderr, "Error: Unknown option '-%c'.\n", optopt);
            }
            print_usage(argv[0]);
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
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (g_conf_timerslack_ns >= 0 &&
        prctl(PR_SET_TIMERSLACK, (unsigned long)g_conf_timerslack_ns, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_TIMERSLACK)");
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
    g_memory_lock_status = "locked (MCL_CURRENT)";
    if (mlockall(MCL_CURRENT) != 0) {
        g_memory_lock_status = "unavailable";
        fprintf(stderr,
                "[WARNING] mlockall(MCL_CURRENT) failed: %s\n"
                "  Measurements may carry more variance from page faults.\n\n",
                strerror(errno));
    }
}

void bench_calibrate(void)
{
    struct nop_calibration nop_calibration = {.loop_count = NOP_CALIBRATION_LOOPS, .nanoseconds_per_loop = 0.0};
    pthread_attr_t thread_attribute;
    pthread_t calibration_thread;

    pthread_attr_init(&thread_attribute);
    pin_worker_thread_attribute(&thread_attribute, 0);
    const int create_result =
        pthread_create(&calibration_thread, &thread_attribute, calibration_routine_nop, &nop_calibration);
    pthread_attr_destroy(&thread_attribute);
    if (create_result != 0) {
        fprintf(stderr, "Error: calibration pthread_create failed: %s\n", strerror(create_result));
        exit(EXIT_FAILURE);
    }
    pthread_join(calibration_thread, NULL);
    g_measured_nop_ns = nop_calibration.nanoseconds_per_loop;
}

void bench_print_config(void)
{
    char pinning_description[128];
    if (g_conf_pinned_cpu_count > 0) {
        int written_length = snprintf(pinning_description, sizeof(pinning_description), "cpus");
        for (int cpu_index = 0;
             cpu_index < g_conf_pinned_cpu_count && written_length < (int)sizeof(pinning_description) - 8;
             ++cpu_index) {
            written_length += snprintf(pinning_description + written_length,
                                       sizeof(pinning_description) - (size_t)written_length, " %d",
                                       g_conf_pinned_cpu_ids[cpu_index]);
        }
    } else {
        snprintf(pinning_description, sizeof(pinning_description),
                 "none (WARNING: P/E-core scheduling variance uncontrolled)");
    }

    printf("\n--- SPINLOCK BENCHMARK SUITE START ---\n"
           "System Info:\n"
           "  L1 Cache Line  : %ld bytes\n"
           "  Timer Slack    : %d ns\n"
           "  NOP Cost       : %.4f ns\n"
           "Configuration:\n"
           "  Threads        : %d\n"
           "  Iterations     : %d\n"
           "  Dummy Tasks    : %d\n"
           "  Backoff Range  : %d ~ %d\n"
           "  Settle Delay   : %d ms (between tests)\n"
           "  Memory Lock    : %s\n"
           "  Pinning        : %s\n"
           "--------------------------------------\n\n",
           g_detected_cache_line_size, prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0), g_measured_nop_ns,
           g_conf_thread_count, g_conf_iterations, g_conf_workload_nop_loops, g_conf_spin_min,
           g_conf_spin_max, SETTLE_DELAY_MS, g_memory_lock_status, pinning_description);
}

void bench_warmup_all(void)
{
    for (int contender_index = 0; contender_index < BENCH_LOCK_COUNT; ++contender_index) {
        if (g_contenders[contender_index].is_selected) {
            run_warmup_pass(g_contenders[contender_index].worker_routine);
        }
    }
}

void bench_run_all(struct bench_results *results)
{
    int printed_any = 0;

    if (!results) {
        return;
    }

    for (int contender_index = 0; contender_index < BENCH_LOCK_COUNT; ++contender_index) {
        results->elapsed_ms[contender_index] = -1.0;
        if (!g_contenders[contender_index].is_selected) {
            continue;
        }
        if (printed_any) {
            printf("\n");
        }
        results->elapsed_ms[contender_index] = run_contender(
            g_contenders[contender_index].display_name, g_contenders[contender_index].worker_routine);
        printed_any = 1;
    }
}

void bench_print_summary(const struct bench_results *results)
{
    static const int summary_order[BENCH_LOCK_COUNT] = {BENCH_LOCK_TTAS, BENCH_LOCK_TTAS_PARK, BENCH_LOCK_MCS,
                                                        BENCH_LOCK_POSIX_SPINLOCK, BENCH_LOCK_POSIX_MUTEX};
    const char *winner_name = NULL;
    double best_ms = 1e300;

    if (!results) {
        return;
    }

    for (int contender_index = 0; contender_index < BENCH_LOCK_COUNT; ++contender_index) {
        if (g_contenders[contender_index].is_selected && results->elapsed_ms[contender_index] < best_ms) {
            best_ms = results->elapsed_ms[contender_index];
            winner_name = g_contenders[contender_index].display_name;
        }
    }

    printf("\n--------------------------------------\n"
           "FINAL RESULT:\n");
    for (int order_index = 0; order_index < BENCH_LOCK_COUNT; ++order_index) {
        const int contender_index = summary_order[order_index];
        if (g_contenders[contender_index].is_selected) {
            printf("  %-14s : %10.3f ms\n", g_contenders[contender_index].summary_label,
                   results->elapsed_ms[contender_index]);
        }
    }
    if (g_contenders[BENCH_LOCK_TTAS].is_selected && g_contenders[BENCH_LOCK_POSIX_SPINLOCK].is_selected) {
        printf("  TTAS / POSIX   : %.2fx\n",
               results->elapsed_ms[BENCH_LOCK_POSIX_SPINLOCK] / results->elapsed_ms[BENCH_LOCK_TTAS]);
    }
    printf("  Winner         : %s\n"
           "--- BENCHMARK SUITE END ---\n\n",
           winner_name);
}

void bench_cleanup(void)
{
    free(g_conf_pinned_cpu_ids);
    g_conf_pinned_cpu_ids = NULL;
    g_conf_pinned_cpu_count = 0;
}
