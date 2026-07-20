#ifndef SPINLOCK_TEST_H
#define SPINLOCK_TEST_H

#include <limits.h>

#include "./spinlock.h"

#define DEFAULT_ITERATIONS 1000000
#define DEFAULT_LOAD_LOOPS 500
#define DEFAULT_NTHREADS 4
#define DEFAULT_SPIN_MIN 4
#define DEFAULT_SPIN_MAX 16000

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

struct bench_results {
    double ttas_ms;
    double mcs_ms;
    double pspin_ms;
};

void bench_detect_topology(void);

void bench_parse_args(int argc, char *argv[]);

void bench_lock_memory(void);

void bench_print_config(void);

void bench_warmup_all(void);

void bench_run_all(struct bench_results *results);

void bench_print_summary(const struct bench_results *results);

void bench_cleanup(void);

#endif
