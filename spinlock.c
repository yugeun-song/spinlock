#include "spinlock.h"

int g_conf_spin_min = SPINLOCK_DEFAULT_SPIN_MIN;
int g_conf_spin_max = SPINLOCK_DEFAULT_SPIN_MAX;

__thread mcs_node_t spin_mcs_thread_local_node;
__thread int spin_mcs_thread_local_node_in_use = 0;
