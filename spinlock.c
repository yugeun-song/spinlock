#include "spinlock.h"

int g_conf_backoff_min_ns = SPINLOCK_DEFAULT_BACKOFF_MIN_NS;
int g_conf_backoff_max_ns = SPINLOCK_DEFAULT_BACKOFF_MAX_NS;
unsigned long long g_spin_ticks_per_ns_q16 = 0;

__thread mcs_node_t spin_mcs_thread_local_node;
__thread int spin_mcs_thread_local_node_in_use = 0;
__thread unsigned int spin_backoff_jitter_state = 0;
