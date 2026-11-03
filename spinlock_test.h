#ifndef SPINLOCK_TEST_H
#define SPINLOCK_TEST_H

#include "spinlock.h"

#include <limits.h>

#define DEFAULT_ITERATIONS 1000000
#define DEFAULT_CRITICAL_SECTION_NS 50
#define DEFAULT_OUTSIDE_WORK_NS 150
#define DEFAULT_THREAD_COUNT 4
#define DEFAULT_BACKOFF_MIN_NS SPINLOCK_DEFAULT_BACKOFF_MIN_NS
#define DEFAULT_BACKOFF_MAX_NS SPINLOCK_DEFAULT_BACKOFF_MAX_NS
#define FAST_CORE_CAPACITY_PERCENT 80

#define MIN_THREAD_COUNT 1
#define MAX_THREAD_COUNT 1024
#define MIN_ITERATIONS 1
#define MAX_ITERATIONS INT_MAX
#define MIN_WORKLOAD_NOP_LOOPS 0
#define MAX_WORKLOAD_NOP_LOOPS INT_MAX
#define MIN_BACKOFF_NS 1
#define MAX_BACKOFF_NS 1000000000
#define NOP_CALIBRATION_LOOPS 1000000
#define NOP_CALIBRATION_PASSES 8
#define NOP_CALIBRATION_WARMUP_MS 50
#define MAX_PLACEMENT_PAGES 64

/*
 * Quiescent gap inserted right before each measured benchmark so the system
 * settles (scheduler drains, CPU frequency relaxes, the previous lock's cache
 * footprint dissipates) and one lock type cannot bias the next.
 */
#define SETTLE_DELAY_MS 100

enum bench_lock {
    BENCH_LOCK_TTAS,
    BENCH_LOCK_TTAS_PARK,
    BENCH_LOCK_POSIX_SPINLOCK,
    BENCH_LOCK_POSIX_MUTEX,
    BENCH_LOCK_MCS,
    BENCH_LOCK_COUNT,
};

struct bench_results {
    double elapsed_ms[BENCH_LOCK_COUNT];
    int failed_atomic_count_checks;
};

void bench_detect_topology(void);

void bench_parse_args(int argc, char *argv[]);

void bench_lock_memory(void);

void bench_calibrate(void);

void bench_print_config(void);

void bench_warmup_all(void);

void bench_run_all(struct bench_results *results);

void bench_print_summary(const struct bench_results *results);

void bench_cleanup(void);

#endif
