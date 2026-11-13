#define _POSIX_C_SOURCE 200809L

#include "spinlock.h"

int g_conf_backoff_min_ns = SPINLOCK_DEFAULT_BACKOFF_MIN_NS;
int g_conf_backoff_max_ns = SPINLOCK_DEFAULT_BACKOFF_MAX_NS;
unsigned long long g_spin_ticks_per_ns_q16 = 0;

__thread struct mcs_node g_spin_mcs_thread_local_node;
__thread int g_is_spin_mcs_thread_local_node_in_use = 0;
__thread unsigned int g_spin_backoff_jitter_state = 0;

unsigned long long spin_measure_ticks_per_ns_q16(void)
{
    unsigned long long ticks_per_ns_q16;
#if defined(__x86_64__)
    struct timespec begin_time;

    clock_gettime(CLOCK_MONOTONIC, &begin_time);

    const unsigned long long begin_ticks = spin_read_ticks();
    unsigned long long elapsed_ns;

    do {
        struct timespec now_time;

        clock_gettime(CLOCK_MONOTONIC, &now_time);
        elapsed_ns = ((unsigned long long)(now_time.tv_sec - begin_time.tv_sec) * 1000000000ULL) +
                     (unsigned long long)now_time.tv_nsec - (unsigned long long)begin_time.tv_nsec;
    } while (elapsed_ns < SPINLOCK_TICK_CALIBRATION_NS);
    ticks_per_ns_q16 = ((spin_read_ticks() - begin_ticks) << 16) / elapsed_ns;
#elif defined(__aarch64__)
    unsigned long long counter_frequency_hz;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(counter_frequency_hz));
    ticks_per_ns_q16 = (counter_frequency_hz << 16) / 1000000000ULL;
#endif
    return (ticks_per_ns_q16 > 0) ? ticks_per_ns_q16 : 1;
}

void spin_calibrate_backoff_clock(void)
{
    __atomic_store_n(&g_spin_ticks_per_ns_q16, spin_measure_ticks_per_ns_q16(), __ATOMIC_RELAXED);
}
