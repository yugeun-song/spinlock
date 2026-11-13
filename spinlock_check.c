#define _GNU_SOURCE

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(SPINLOCK_DEBUG)
# include <fcntl.h>
# include <signal.h>
# include <sys/wait.h>
#endif

#include "spinlock.h"

#define CHECK_MAX_THREAD_COUNT 8
#define CHECK_MIN_THREAD_COUNT 2
#define CHECK_ITERATIONS_PER_THREAD 100000
#define CHECK_HOLD_PAUSE_COUNT 16
#define CHECK_PARK_FORCING_BACKOFF_MAX_NS 8

#if defined(SPINLOCK_DEBUG)
# define DEBUG_STATE_TEXT "on"
#else
# define DEBUG_STATE_TEXT "off"
#endif

struct contended_lock_pair {
    struct spinlock_ttas ttas_lock;
    struct spinlock_mcs mcs_outer_lock;
    struct spinlock_mcs mcs_inner_lock;
};

struct overlap_probe {
    long long protected_counter;
    char counter_padding[CACHE_LINE_SIZE - sizeof(long long)];
    int threads_inside_critical_section;
    int overlap_violation_count;
    char probe_padding[CACHE_LINE_SIZE - (2 * sizeof(int))];
} __attribute__((aligned(CACHE_LINE_SIZE)));

typedef void (*lock_callback)(struct contended_lock_pair *const mut_locks,
                              struct mcs_node *const mut_node_a, struct mcs_node *const mut_node_b);

struct contended_worker_args {
    struct contended_lock_pair *locks;
    struct overlap_probe *probe;
    pthread_barrier_t *start_barrier;
    int iterations;
    lock_callback acquire;
    lock_callback release;
};

#if defined(SPINLOCK_DEBUG)
enum misuse_kind {
    MISUSE_TTAS_UNLOCK_NOT_HELD,
    MISUSE_MCS_UNLOCK_EMPTY_QUEUE,
    MISUSE_MCS_WRAPPER_NESTED,
};
#endif

static int gs_failed_check_count = 0;
static int gs_passed_check_count = 0;
static int gs_check_thread_count = CHECK_MAX_THREAD_COUNT;
static int gs_check_iterations_per_thread = CHECK_ITERATIONS_PER_THREAD;

static void report_pass(const char *const check_name)
{
    ++gs_passed_check_count;
    printf("[PASS] %s\n", check_name);
}

static void report_fail(const char *const check_name, const char *const detail)
{
    ++gs_failed_check_count;
    printf("[FAIL] %s: %s\n", check_name, detail);
}

static void report_skip(const char *const check_name, const char *const reason)
{
    printf("[SKIP] %s: %s\n", check_name, reason);
}

static void probe_enter_critical_section(struct overlap_probe *const mut_probe)
{
    const int previously_inside = __atomic_fetch_add(&mut_probe->threads_inside_critical_section, 1,
                                                     __ATOMIC_ACQ_REL);
    if (previously_inside != 0) {
        __atomic_fetch_add(&mut_probe->overlap_violation_count, 1, __ATOMIC_RELAXED);
    }
    mut_probe->protected_counter += 1;
}

static void probe_leave_critical_section(struct overlap_probe *const mut_probe)
{
    for (int pause_index = 0; pause_index < CHECK_HOLD_PAUSE_COUNT; ++pause_index) {
        cpu_relax();
    }
    __atomic_fetch_sub(&mut_probe->threads_inside_critical_section, 1, __ATOMIC_ACQ_REL);
}

static void acquire_ttas(struct contended_lock_pair *const mut_locks,
                         struct mcs_node *const mut_node_a, struct mcs_node *const mut_node_b)
{
    spin_lock_ttas(&mut_locks->ttas_lock);
}

static void acquire_ttas_park(struct contended_lock_pair *const mut_locks,
                              struct mcs_node *const mut_node_a, struct mcs_node *const mut_node_b)
{
    spin_lock_ttas_park(&mut_locks->ttas_lock);
}

static void release_ttas(struct contended_lock_pair *const mut_locks,
                         struct mcs_node *const mut_node_a, struct mcs_node *const mut_node_b)
{
    spin_unlock_ttas(&mut_locks->ttas_lock);
}

static void acquire_mcs_wrapper(struct contended_lock_pair *const mut_locks,
                                struct mcs_node *const mut_node_a,
                                struct mcs_node *const mut_node_b)
{
    spin_lock_mcs(&mut_locks->mcs_outer_lock);
}

static void release_mcs_wrapper(struct contended_lock_pair *const mut_locks,
                                struct mcs_node *const mut_node_a,
                                struct mcs_node *const mut_node_b)
{
    spin_unlock_mcs(&mut_locks->mcs_outer_lock);
}

static void acquire_mcs_explicit_node(struct contended_lock_pair *const mut_locks,
                                      struct mcs_node *const mut_node_a,
                                      struct mcs_node *const mut_node_b)
{
    spin_lock_mcs_node(&mut_locks->mcs_outer_lock, mut_node_a);
}

static void release_mcs_explicit_node(struct contended_lock_pair *const mut_locks,
                                      struct mcs_node *const mut_node_a,
                                      struct mcs_node *const mut_node_b)
{
    spin_unlock_mcs_node(&mut_locks->mcs_outer_lock, mut_node_a);
}

static void acquire_mcs_nested(struct contended_lock_pair *const mut_locks,
                               struct mcs_node *const mut_node_a,
                               struct mcs_node *const mut_node_b)
{
    spin_lock_mcs_node(&mut_locks->mcs_outer_lock, mut_node_a);
    spin_lock_mcs_node(&mut_locks->mcs_inner_lock, mut_node_b);
}

static void release_mcs_nested(struct contended_lock_pair *const mut_locks,
                               struct mcs_node *const mut_node_a,
                               struct mcs_node *const mut_node_b)
{
    spin_unlock_mcs_node(&mut_locks->mcs_inner_lock, mut_node_b);
    spin_unlock_mcs_node(&mut_locks->mcs_outer_lock, mut_node_a);
}

static void acquire_broken_no_op(struct contended_lock_pair *const mut_locks,
                                 struct mcs_node *const mut_node_a,
                                 struct mcs_node *const mut_node_b)
{
}

static void release_broken_no_op(struct contended_lock_pair *const mut_locks,
                                 struct mcs_node *const mut_node_a,
                                 struct mcs_node *const mut_node_b)
{
}

static void *run_contended_worker(void *const mut_arg)
{
    struct contended_worker_args *args = mut_arg;
    struct mcs_node *node_a = aligned_alloc(CACHE_LINE_SIZE, sizeof(*node_a));
    struct mcs_node *node_b = aligned_alloc(CACHE_LINE_SIZE, sizeof(*node_b));

    if (node_a == NULL || node_b == NULL) {
        perror("aligned_alloc");
        exit(EXIT_FAILURE);
    }
    memset(node_a, 0, sizeof(*node_a));
    memset(node_b, 0, sizeof(*node_b));

    pthread_barrier_wait(args->start_barrier);

    for (int iteration = 0; iteration < args->iterations; ++iteration) {
        args->acquire(args->locks, node_a, node_b);
        probe_enter_critical_section(args->probe);
        probe_leave_critical_section(args->probe);
        args->release(args->locks, node_a, node_b);
    }

    free(node_a);
    node_a = NULL;
    free(node_b);
    node_b = NULL;
    return NULL;
}

static void run_contended_check(const char *const check_name, lock_callback acquire,
                                lock_callback release, int is_overlap_expected)
{
    struct contended_lock_pair locks;
    struct overlap_probe probe = { 0 };
    pthread_barrier_t start_barrier;

    spin_init_ttas(&locks.ttas_lock);
    spin_init_mcs(&locks.mcs_outer_lock);
    spin_init_mcs(&locks.mcs_inner_lock);

    const int barrier_init_result = pthread_barrier_init(&start_barrier, NULL,
                                                         (unsigned)gs_check_thread_count);
    if (barrier_init_result != 0) {
        fprintf(stderr, "pthread_barrier_init: %s\n", strerror(barrier_init_result));
        exit(EXIT_FAILURE);
    }

    struct contended_worker_args worker_args = {
        .locks = &locks,
        .probe = &probe,
        .start_barrier = &start_barrier,
        .iterations = gs_check_iterations_per_thread,
        .acquire = acquire,
        .release = release
    };
    pthread_t worker_threads[CHECK_MAX_THREAD_COUNT];

    for (int worker_index = 0; worker_index < gs_check_thread_count; ++worker_index) {
        const int create_result = pthread_create(&worker_threads[worker_index], NULL,
                                                 run_contended_worker, &worker_args);
        if (create_result != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(create_result));
            exit(EXIT_FAILURE);
        }
    }
    for (int worker_index = 0; worker_index < gs_check_thread_count; ++worker_index) {
        pthread_join(worker_threads[worker_index], NULL);
    }
    pthread_barrier_destroy(&start_barrier);

    long long expected_counter = (long long)gs_check_iterations_per_thread * gs_check_thread_count;

    if (is_overlap_expected != 0) {
        if (probe.overlap_violation_count > 0) {
            report_pass(check_name);
        } else {
            report_fail(check_name, "the overlap detector saw no violation from a no-op lock, so it could not prove itself");
        }
        return;
    }

    char detail[256];

    if (probe.overlap_violation_count != 0) {
        snprintf(detail, sizeof(detail), "%d overlapping critical-section entries",
                 probe.overlap_violation_count);
        report_fail(check_name, detail);
        return;
    }
    if (probe.protected_counter != expected_counter) {
        snprintf(detail, sizeof(detail), "protected counter %lld, expected %lld",
                 probe.protected_counter, expected_counter);
        report_fail(check_name, detail);
        return;
    }
    if (locks.ttas_lock.is_locked != SPINLOCK_UNLOCKED) {
        report_fail(check_name, "ttas lock still marked locked after every thread released it");
        return;
    }
    if (locks.mcs_outer_lock.queue_tail != NULL || locks.mcs_inner_lock.queue_tail != NULL) {
        report_fail(check_name, "mcs queue tail not NULL after every thread released it");
        return;
    }
    report_pass(check_name);
}

static void check_ttas_uncontended_state_transitions(void)
{
    const char *check_name = "ttas uncontended lock/unlock toggles is_locked";
    struct spinlock_ttas lock;

    spin_init_ttas(&lock);
    if (lock.is_locked != SPINLOCK_UNLOCKED) {
        report_fail(check_name, "spin_init_ttas did not clear is_locked");
        return;
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
        spin_lock_ttas(&lock);
        if (lock.is_locked != SPINLOCK_LOCKED) {
            report_fail(check_name, "is_locked not set after spin_lock_ttas");
            return;
        }
        spin_unlock_ttas(&lock);
        if (lock.is_locked != SPINLOCK_UNLOCKED) {
            report_fail(check_name, "is_locked not cleared after spin_unlock_ttas");
            return;
        }
        spin_lock_ttas_park(&lock);
        if (lock.is_locked != SPINLOCK_LOCKED) {
            report_fail(check_name, "is_locked not set after spin_lock_ttas_park");
            return;
        }
        spin_unlock_ttas(&lock);
    }
    report_pass(check_name);
}

static void check_ttas_cas_fails_while_held(void)
{
    const char *check_name = "ttas cas fails while the lock is held";
    struct spinlock_ttas lock;

    spin_init_ttas(&lock);
    if (ttas_try_acquire(&lock) != 0) {
        report_fail(check_name, "cas on a free lock failed");
        return;
    }
    if (ttas_try_acquire(&lock) == 0) {
        report_fail(check_name, "cas on a held lock succeeded");
        return;
    }
    if (lock.is_locked != SPINLOCK_LOCKED) {
        report_fail(check_name, "failed cas altered the lock word");
        return;
    }
    spin_unlock_ttas(&lock);
    if (ttas_try_acquire(&lock) != 0) {
        report_fail(check_name, "cas after unlock failed");
        return;
    }
    spin_unlock_ttas(&lock);
    report_pass(check_name);
}

static void check_mcs_uncontended_state_transitions(void)
{
    const char *check_name = "mcs uncontended lock/unlock maintains queue_tail";
    struct spinlock_mcs lock;
    struct mcs_node explicit_node = { 0 };

    spin_init_mcs(&lock);
    if (lock.queue_tail != NULL) {
        report_fail(check_name, "spin_init_mcs left queue_tail non-NULL");
        return;
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
        spin_lock_mcs_node(&lock, &explicit_node);
        if (lock.queue_tail != &explicit_node) {
            report_fail(check_name, "queue_tail is not the holder's node while held");
            return;
        }
        if (explicit_node.next_waiter != NULL) {
            report_fail(check_name, "holder's next_waiter not NULL without a successor");
            return;
        }
        spin_unlock_mcs_node(&lock, &explicit_node);
        if (lock.queue_tail != NULL) {
            report_fail(check_name, "queue_tail not NULL after the only holder released");
            return;
        }
        spin_lock_mcs(&lock);
        if (lock.queue_tail != &g_spin_mcs_thread_local_node) {
            report_fail(check_name, "wrapper did not enqueue the thread-local node");
            return;
        }
        spin_unlock_mcs(&lock);
        if (lock.queue_tail != NULL) {
            report_fail(check_name, "queue_tail not NULL after the wrapper released");
            return;
        }
    }
    report_pass(check_name);
}

static void check_mcs_two_locks_two_nodes_same_thread(void)
{
    const char *check_name = "mcs one thread holds two locks through two explicit nodes";
    struct spinlock_mcs lock_a;
    struct spinlock_mcs lock_b;
    struct mcs_node node_a = { 0 };
    struct mcs_node node_b = { 0 };

    spin_init_mcs(&lock_a);
    spin_init_mcs(&lock_b);

    spin_lock_mcs_node(&lock_a, &node_a);
    spin_lock_mcs_node(&lock_b, &node_b);
    if (lock_a.queue_tail != &node_a || lock_b.queue_tail != &node_b) {
        report_fail(check_name, "queue tails do not point at the holder's nodes");
        return;
    }
    spin_unlock_mcs_node(&lock_b, &node_b);
    spin_unlock_mcs_node(&lock_a, &node_a);
    if (lock_a.queue_tail != NULL || lock_b.queue_tail != NULL) {
        report_fail(check_name, "queue tails not NULL after release");
        return;
    }
    report_pass(check_name);
}

static void *run_mcs_helper_thread(void *const mut_arg)
{
    struct spinlock_mcs *lock = mut_arg;

    spin_lock_mcs(lock);
    spin_unlock_mcs(lock);
    return NULL;
}

static void check_mcs_wrapper_node_is_per_thread(void)
{
    const char *check_name = "mcs wrapper node is thread-local, another thread queues behind it";
    struct spinlock_mcs lock;
    pthread_t helper_thread;

    spin_init_mcs(&lock);
    spin_lock_mcs(&lock);
    if (pthread_create(&helper_thread, NULL, run_mcs_helper_thread, &lock) != 0) {
        report_fail(check_name, "pthread_create failed");
        spin_unlock_mcs(&lock);
        return;
    }

    int wait_spins = 0;

    while (mcs_load_acquire_node(&g_spin_mcs_thread_local_node.next_waiter) == NULL) {
        cpu_relax();
        ++wait_spins;
        if (wait_spins > 200000000) {
            report_fail(check_name, "helper thread never linked behind the holder");
            spin_unlock_mcs(&lock);
            pthread_join(helper_thread, NULL);
            return;
        }
    }
    if (g_spin_mcs_thread_local_node.next_waiter == &g_spin_mcs_thread_local_node) {
        report_fail(check_name, "helper thread shares the holder's thread-local node");
        spin_unlock_mcs(&lock);
        pthread_join(helper_thread, NULL);
        return;
    }
    spin_unlock_mcs(&lock);
    pthread_join(helper_thread, NULL);
    if (lock.queue_tail != NULL) {
        report_fail(check_name, "queue_tail not NULL after both threads released");
        return;
    }
    report_pass(check_name);
}

static void check_backoff_cap_boundaries(void)
{
    const char *check_name = "ttas backoff survives extreme cap values";
    struct spinlock_ttas lock;
    const int saved_backoff_min_ns = g_conf_backoff_min_ns;
    const int saved_backoff_max_ns = g_conf_backoff_max_ns;

    spin_init_ttas(&lock);

    g_conf_backoff_min_ns = 1;
    g_conf_backoff_max_ns = 1;
    spin_lock_ttas(&lock);
    spin_unlock_ttas(&lock);

    g_conf_backoff_min_ns = INT_MAX;
    g_conf_backoff_max_ns = INT_MAX;
    spin_lock_ttas(&lock);
    spin_unlock_ttas(&lock);

    g_conf_backoff_min_ns = 16;
    g_conf_backoff_max_ns = 4;
    spin_lock_ttas(&lock);
    spin_unlock_ttas(&lock);

    g_conf_backoff_min_ns = saved_backoff_min_ns;
    g_conf_backoff_max_ns = saved_backoff_max_ns;
    report_pass(check_name);
}

static void check_struct_layout(void)
{
    const char *check_name = "lock and node types each occupy exactly one aligned cache line";

    if (sizeof(struct spinlock_ttas) != CACHE_LINE_SIZE ||
        sizeof(struct spinlock_mcs) != CACHE_LINE_SIZE ||
        sizeof(struct mcs_node) != CACHE_LINE_SIZE) {
        char detail[256];

        snprintf(detail, sizeof(detail), "sizes ttas %zu, mcs %zu, node %zu",
                 sizeof(struct spinlock_ttas), sizeof(struct spinlock_mcs),
                 sizeof(struct mcs_node));
        report_fail(check_name, detail);
        return;
    }
    if (__alignof__(struct spinlock_ttas) != CACHE_LINE_SIZE ||
        __alignof__(struct spinlock_mcs) != CACHE_LINE_SIZE ||
        __alignof__(struct mcs_node) != CACHE_LINE_SIZE) {
        report_fail(check_name, "alignment is not CACHE_LINE_SIZE");
        return;
    }
    if (((unsigned long)&g_spin_mcs_thread_local_node) % CACHE_LINE_SIZE != 0) {
        report_fail(check_name, "thread-local mcs node is not cache-line aligned");
        return;
    }
    report_pass(check_name);
}

static void check_ttas_park_contended(void)
{
    const int saved_backoff_max_ns = g_conf_backoff_max_ns;

    g_conf_backoff_max_ns = CHECK_PARK_FORCING_BACKOFF_MAX_NS;
    run_contended_check("ttas_park contended, cap forced low so waiters park", acquire_ttas_park,
                        release_ttas, 0);
    g_conf_backoff_max_ns = saved_backoff_max_ns;
}

#if defined(SPINLOCK_DEBUG)
static void perform_misuse(enum misuse_kind kind)
{
    struct spinlock_ttas ttas_lock;
    struct spinlock_mcs mcs_lock_a;
    struct spinlock_mcs mcs_lock_b;
    struct mcs_node explicit_node = { 0 };

    spin_init_ttas(&ttas_lock);
    spin_init_mcs(&mcs_lock_a);
    spin_init_mcs(&mcs_lock_b);

    switch (kind) {
    case MISUSE_TTAS_UNLOCK_NOT_HELD:
        spin_unlock_ttas(&ttas_lock);
        break;
    case MISUSE_MCS_UNLOCK_EMPTY_QUEUE:
        spin_unlock_mcs_node(&mcs_lock_a, &explicit_node);
        break;
    case MISUSE_MCS_WRAPPER_NESTED:
        spin_lock_mcs(&mcs_lock_a);
        spin_lock_mcs(&mcs_lock_b);
        break;
    }
}

static void check_debug_misuse_aborts(const char *const check_name, enum misuse_kind kind)
{
    fflush(stdout);

    pid_t child_pid = fork();
    if (child_pid < 0) {
        report_fail(check_name, "fork failed");
        return;
    }
    if (child_pid == 0) {
        const int devnull_fd = open("/dev/null", O_WRONLY);
        if (devnull_fd >= 0) {
            dup2(devnull_fd, STDERR_FILENO);
        }
        perform_misuse(kind);
        _exit(0);
    }

    int wait_status;

    if (waitpid(child_pid, &wait_status, 0) < 0) {
        report_fail(check_name, "waitpid failed");
        return;
    }
    if (WIFSIGNALED(wait_status) != 0 && WTERMSIG(wait_status) == SIGABRT) {
        report_pass(check_name);
        return;
    }

    char detail[256];

    if (WIFEXITED(wait_status) != 0) {
        snprintf(detail, sizeof(detail), "child exited normally with status %d instead of aborting",
                 WEXITSTATUS(wait_status));
    } else {
        snprintf(detail, sizeof(detail), "child ended with unexpected wait status 0x%x",
                 wait_status);
    }
    report_fail(check_name, detail);
}
#endif /* defined(SPINLOCK_DEBUG) */

static void check_debug_misuse_detection(void)
{
#if defined(SPINLOCK_DEBUG)
    check_debug_misuse_aborts("debug: spin_unlock_ttas on a free lock aborts",
                              MISUSE_TTAS_UNLOCK_NOT_HELD);
    check_debug_misuse_aborts("debug: spin_unlock_mcs_node on an empty queue aborts",
                              MISUSE_MCS_UNLOCK_EMPTY_QUEUE);
    check_debug_misuse_aborts("debug: nested spin_lock_mcs wrapper aborts",
                              MISUSE_MCS_WRAPPER_NESTED);
#else
    report_skip("debug misuse detection", "SPINLOCK_DEBUG not compiled in (release build)");
#endif
}

static void choose_iterations_per_thread(void)
{
    const char *override_text = getenv("SPINLOCK_CHECK_ITERATIONS");

    if (override_text == NULL || override_text[0] == '\0') {
        return;
    }

    char *parse_end = NULL;
    long override_value = strtol(override_text, &parse_end, 10);

    if (*parse_end != '\0' || override_value < 1 || override_value > INT_MAX) {
        fprintf(stderr, "SPINLOCK_CHECK_ITERATIONS must be an integer in 1..%d, got '%s'\n",
                INT_MAX, override_text);
        exit(EXIT_FAILURE);
    }
    gs_check_iterations_per_thread = (int)override_value;
}

static void choose_thread_count(void)
{
    const long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (online_cpus >= CHECK_MAX_THREAD_COUNT) {
        gs_check_thread_count = CHECK_MAX_THREAD_COUNT;
    } else if (online_cpus >= CHECK_MIN_THREAD_COUNT) {
        gs_check_thread_count = (int)online_cpus;
    } else {
        gs_check_thread_count = CHECK_MIN_THREAD_COUNT;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    choose_thread_count();
    choose_iterations_per_thread();

    printf("spinlock_check: %d worker threads, %d iterations each, SPINLOCK_DEBUG %s\n",
           gs_check_thread_count, gs_check_iterations_per_thread, DEBUG_STATE_TEXT);

    check_struct_layout();
    check_ttas_uncontended_state_transitions();
    check_ttas_cas_fails_while_held();
    check_mcs_uncontended_state_transitions();
    check_mcs_two_locks_two_nodes_same_thread();
    check_mcs_wrapper_node_is_per_thread();
    check_backoff_cap_boundaries();

    run_contended_check("overlap detector proves itself on a no-op lock", acquire_broken_no_op,
                        release_broken_no_op, 1);
    run_contended_check("ttas contended, no overlapping critical sections", acquire_ttas,
                        release_ttas, 0);
    check_ttas_park_contended();
    run_contended_check("mcs wrapper contended, no overlapping critical sections",
                        acquire_mcs_wrapper, release_mcs_wrapper, 0);
    run_contended_check("mcs explicit heap node contended, no overlapping critical sections",
                        acquire_mcs_explicit_node, release_mcs_explicit_node, 0);
    run_contended_check("mcs two locks nested per thread, no overlapping critical sections",
                        acquire_mcs_nested, release_mcs_nested, 0);

    check_debug_misuse_detection();

    printf("spinlock_check: %d passed, %d failed\n", gs_passed_check_count, gs_failed_check_count);
    if (gs_failed_check_count != 0) {
        return EXIT_FAILURE;
    }

    return 0;
}
