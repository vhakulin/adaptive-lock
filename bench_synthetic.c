#define _GNU_SOURCE
#include "adaptive_lock.h"
#include "adaptive_lock_calibrate.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { LOCK_ADAPTIVE, LOCK_PTHREAD } lock_kind_t;
typedef enum { WORK_FIXED, WORK_BIMODAL_95_5, WORK_PHASE_SHIFT } work_mode_t;

typedef struct {
    lock_kind_t kind;
    work_mode_t mode;
    adaptive_lock_t *alock;
    pthread_mutex_t *pmutex;
    int iterations;
    int hold_us;
    int spike_us;
    int outside_us;
    int pin_cpu;
    uint64_t *shared_counter;
    uint64_t *latencies_ns;
    uint64_t thread_elapsed_ns;
} bench_arg_t;

/* Low-overhead hardware-tick busy wait (avoids vDSO clock_gettime contention) */
static inline void busy_wait_us(int us) {
    if (us <= 0) return;
    uint64_t target_ns = (uint64_t)us * 1000ULL;
    uint64_t t0 = alock_ticks();
    while ((uint64_t)alock_ticks_to_ns(alock_ticks() - t0) < target_ns) {
        alock_cpu_relax();
    }
}

static void *bench_worker(void *ptr) {
    bench_arg_t *arg = (bench_arg_t *)ptr;
    if (arg->pin_cpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(arg->pin_cpu, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }

    uint64_t rng = alock_ticks() ^ (uintptr_t)&arg;
    uint64_t thread_t0 = alock_now_ns();

    for (int i = 0; i < arg->iterations; ++i) {
        int current_hold = arg->hold_us;
        if (arg->mode == WORK_BIMODAL_95_5) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            if ((rng % 20ULL) == 0) { /* 5% probability */
                current_hold = arg->spike_us;
            }
        } else if (arg->mode == WORK_PHASE_SHIFT) {
            /* Flip between fast regime and slow regime every 200 iterations */
            if (((i / 200) & 1) == 1) {
                current_hold = arg->spike_us;
            }
        }

        uint64_t t0 = alock_ticks();
        if (arg->kind == LOCK_ADAPTIVE) {
            adaptive_lock(arg->alock);
        } else {
            pthread_mutex_lock(arg->pmutex);
        }
        uint64_t t1 = alock_ticks();

        (*arg->shared_counter)++;
        busy_wait_us(current_hold);

        if (arg->kind == LOCK_ADAPTIVE) {
            adaptive_unlock(arg->alock);
        } else {
            pthread_mutex_unlock(arg->pmutex);
        }

        arg->latencies_ns[i] = (uint64_t)alock_ticks_to_ns(t1 - t0);
        busy_wait_us(arg->outside_us);
    }

    arg->thread_elapsed_ns = alock_now_ns() - thread_t0;
    return NULL;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static void run_scenario(const char *name, lock_kind_t kind, work_mode_t mode,
                         int threads, int iters_per_thread,
                         int hold_us, int spike_us, int outside_us,
                         const int *pin_cpus, bool is_warmup) {
    adaptive_lock_t alock;
    pthread_mutex_t pmutex;
    adaptive_lock_init(&alock);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
    pthread_mutex_init(&pmutex, &attr);
    pthread_mutexattr_destroy(&attr);

    uint64_t counter = 0;
    size_t total_ops = (size_t)threads * (size_t)iters_per_thread;
    uint64_t *all_lats = malloc(sizeof(uint64_t) * total_ops);
    pthread_t *tids = malloc(sizeof(pthread_t) * (size_t)threads);
    bench_arg_t *args = malloc(sizeof(bench_arg_t) * (size_t)threads);

    /* Prefault latency buffer so minor page faults don't distort acquisition timing */
    memset(all_lats, 0, sizeof(uint64_t) * total_ops);

    uint64_t t_start = alock_now_ns();
    for (int i = 0; i < threads; ++i) {
        args[i] = (bench_arg_t){
            .kind = kind,
            .mode = mode,
            .alock = &alock,
            .pmutex = &pmutex,
            .iterations = iters_per_thread,
            .hold_us = hold_us,
            .spike_us = spike_us,
            .outside_us = outside_us,
            .pin_cpu = pin_cpus ? pin_cpus[i] : -1,
            .shared_counter = &counter,
            .latencies_ns = all_lats + (size_t)i * (size_t)iters_per_thread,
            .thread_elapsed_ns = 0,
        };
        pthread_create(&tids[i], NULL, bench_worker, &args[i]);
    }

    uint64_t min_t_ns = UINT64_MAX;
    uint64_t max_t_ns = 0;
    for (int i = 0; i < threads; ++i) {
        pthread_join(tids[i], NULL);
        if (args[i].thread_elapsed_ns < min_t_ns) min_t_ns = args[i].thread_elapsed_ns;
        if (args[i].thread_elapsed_ns > max_t_ns) max_t_ns = args[i].thread_elapsed_ns;
    }
    uint64_t elapsed_ns = alock_now_ns() - t_start;

    if (!is_warmup) {
        qsort(all_lats, total_ops, sizeof(uint64_t), cmp_u64);
        uint64_t p50 = all_lats[total_ops / 2];
        uint64_t p99 = all_lats[(total_ops * 99) / 100];
        uint64_t max_lat = all_lats[total_ops - 1];
        double ops_per_sec = (double)total_ops / ((double)elapsed_ns / 1e9);
        double fairness_ratio = (min_t_ns > 0) ? ((double)max_t_ns / (double)min_t_ns) : 1.0;

        printf("| %-28s | %-13s | %10.0f ops/s | p50: %8.2f us | p99: %9.2f us | max: %10.2f us | skew: %4.2fx |\n",
               name,
               kind == LOCK_ADAPTIVE ? "adaptive_lock" : "pthread_mutex",
               ops_per_sec,
               (double)p50 / 1000.0,
               (double)p99 / 1000.0,
               (double)max_lat / 1000.0,
               fairness_ratio);
    }

    pthread_mutex_destroy(&pmutex);
    free(all_lats);
    free(tids);
    free(args);
}

/* Finds two logical CPUs sharing a physical core (SMT siblings); falls back to CPU 0 + CPU 0 */
static void find_smt_pair(int pair[2]) {
    pair[0] = 0;
    pair[1] = 0;
#ifdef __linux__
    int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus > ALOCK_MAX_CPUS) ncpus = ALOCK_MAX_CPUS;
    for (int i = 0; i < ncpus; ++i) {
        for (int j = i + 1; j < ncpus; ++j) {
            if (alock_is_same_physical_core(i, j)) {
                pair[0] = i;
                pair[1] = j;
                return;
            }
        }
    }
#endif
}

#define NTHREADS 40

int main(void) {
    adaptive_lock_calibrate();

    int smt_pair[2];
    find_smt_pair(smt_pair);

    /* Discard warmup runs to ramp CPU clocks */
    run_scenario("warmup", LOCK_PTHREAD,  WORK_FIXED, 4, 50000, 0, 0, 0, NULL, true);
    run_scenario("warmup", LOCK_ADAPTIVE, WORK_FIXED, 4, 50000, 0, 0, 0, NULL, true);

    printf("| %-28s | %-13s | %-16s | %-16s | %-17s | %-18s | %-11s |\n",
           "Scenario", "Lock", "Throughput", "p50 Acquire", "p99 Acquire", "Max Acquire", "Thread Skew");
    printf("| :--------------------------- | :------------ | ---------------: | ---------------: | ----------------: | -----------------: | ----------: |\n");

    run_scenario("0. Uncontended (1T, 0us)",    LOCK_PTHREAD,  WORK_FIXED,        1, 5000000,   0,   0,   0, NULL,     false);
    run_scenario("0. Uncontended (1T, 0us)",    LOCK_ADAPTIVE, WORK_FIXED,        1, 5000000,   0,   0,   0, NULL,     false);

    run_scenario("1. Short (40T, 1us/5us)",     LOCK_PTHREAD,  WORK_FIXED, NTHREADS,   25000,   1,   0,   5, NULL,     false);
    run_scenario("1. Short (40T, 1us/5us)",     LOCK_ADAPTIVE, WORK_FIXED, NTHREADS,   25000,   1,   0,   5, NULL,     false);

    run_scenario("2. Medium (40T, 10us/50us)", LOCK_PTHREAD,  WORK_FIXED, NTHREADS,   25000,  10,   0, 50, NULL,     false);
    run_scenario("2. Medium (40T, 10us/50us)", LOCK_ADAPTIVE, WORK_FIXED, NTHREADS,   25000,  10,   0, 50, NULL,     false);

    run_scenario("3. Long (40T, 250us/500us)",  LOCK_PTHREAD,  WORK_FIXED, NTHREADS,     500, 250,   0, 500, NULL,     false);
    run_scenario("3. Long (40T, 250us/500us)",  LOCK_ADAPTIVE, WORK_FIXED, NTHREADS,     500, 250,   0, 500, NULL,     false);

    run_scenario("4. Bimodal 95/5 (40T)",       LOCK_PTHREAD,  WORK_BIMODAL_95_5, NTHREADS, 3000, 1, 500, 20, NULL,   false);
    run_scenario("4. Bimodal 95/5 (40T)",       LOCK_ADAPTIVE, WORK_BIMODAL_95_5, NTHREADS, 3000, 1, 500, 20, NULL,   false);

    run_scenario("5. Phase-Shift (40T)",        LOCK_PTHREAD,  WORK_PHASE_SHIFT, NTHREADS, 2000, 1, 400, 50, NULL,   false);
    run_scenario("5. Phase-Shift (40T)",        LOCK_ADAPTIVE, WORK_PHASE_SHIFT, NTHREADS, 2000, 1, 400, 50, NULL,   false);

    char smt_label[32];
    snprintf(smt_label, sizeof(smt_label), "5. SMT Siblings (%d,%d)", smt_pair[0], smt_pair[1]);
    run_scenario(smt_label,                     LOCK_PTHREAD,  WORK_FIXED,        2,  100000,   5,   0,   2, smt_pair, false);
    run_scenario(smt_label,                     LOCK_ADAPTIVE, WORK_FIXED,        2,  100000,   5,   0,   2, smt_pair, false);

    return 0;
}

