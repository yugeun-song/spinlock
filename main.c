#include <stdio.h>
#include <stdlib.h>

#include "./spinlock_test.h"

int main(int argc, char *argv[])
{
    struct bench_results results;

    setvbuf(stdout, NULL, _IOLBF, 0);

    bench_detect_topology();
    bench_parse_args(argc, argv);
    bench_lock_memory();
    bench_print_config();

    bench_warmup_all();
    bench_run_all(&results);
    bench_print_summary(&results);

    bench_cleanup();

    return EXIT_SUCCESS;
}
