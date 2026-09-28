#ifndef SPINLOCK_TEST_H
#define SPINLOCK_TEST_H

#include "spinlock.h"

#include <limits.h>

#define DEFAULT_ITERATIONS 1000000
#define DEFAULT_WORKLOAD_NOP_LOOPS 500
#define DEFAULT_THREAD_COUNT 4
#define DEFAULT_SPIN_MIN SPINLOCK_DEFAULT_SPIN_MIN
#define DEFAULT_SPIN_MAX SPINLOCK_DEFAULT_SPIN_MAX

#define MIN_THREAD_COUNT 1
#define MAX_THREAD_COUNT 1024
#define MIN_ITERATIONS 1
#define MAX_ITERATIONS INT_MAX
#define MIN_WORKLOAD_NOP_LOOPS 0
#define MAX_WORKLOAD_NOP_LOOPS INT_MAX
#define MIN_BACKOFF_PAUSES 1
#define MAX_BACKOFF_PAUSES (INT_MAX / 2)
#define NOP_CALIBRATION_LOOPS 1000000
#define NOP_CALIBRATION_PASSES 8

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
    BENCH_LOCK_COUNT
};

struct bench_results {
    double elapsed_ms[BENCH_LOCK_COUNT];
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
