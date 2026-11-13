#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

#include "spinlock_test.h"

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    bench_detect_topology();
    bench_parse_args(argc, argv);
    bench_lock_memory();
    bench_calibrate();
    bench_print_config();

    bench_warm_up_all();

    struct bench_results benchmark_results;

    bench_run_all(&benchmark_results);
    bench_print_summary(&benchmark_results);

    bench_clean_up();

    if (benchmark_results.failed_atomic_count_checks != 0) {
        return EXIT_FAILURE;
    }

    return 0;
}
