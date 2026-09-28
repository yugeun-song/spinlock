#include "spinlock_test.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    struct bench_results benchmark_results;

    setvbuf(stdout, NULL, _IOLBF, 0);

    bench_detect_topology();
    bench_parse_args(argc, argv);
    bench_lock_memory();
    bench_calibrate();
    bench_print_config();

    bench_warmup_all();
    bench_run_all(&benchmark_results);
    bench_print_summary(&benchmark_results);

    bench_cleanup();

    return EXIT_SUCCESS;
}
