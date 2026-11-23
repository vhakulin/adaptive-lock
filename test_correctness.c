#define _GNU_SOURCE
#include "adaptive_lock.h"
#include "adaptive_lock_calibrate.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_ACCOUNTS 16
#define INITIAL_BAL  10000L

typedef struct {
    adaptive_lock_t lock;
    _Atomic int in_critical_section;
    uint64_t total_acquires;
    int64_t accounts[NUM_ACCOUNTS];
    uint64_t checksum_history;
} shared_state_t;

typedef struct {
    shared_state_t *st;
    int thread_id;
    int iterations;
    bool pin_single_cpu;
    bool inject_spikes;
} test_worker_arg_t;

static void *correctness_worker(void *ptr) {
    test_worker_arg_t *arg = (test_worker_arg_t *)ptr;
    shared_state_t *st = arg->st;

    if (arg->pin_single_cpu) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }

    uint64_t rng = (uint64_t)(arg->thread_id + 1) * 0x9e3779b97f4a7c15ULL;

    for (int i = 0; i < arg->iterations; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;

        /* Periodically test trylock */
        if ((i & 7) == 0) {
            if (!adaptive_trylock(&st->lock)) {
                adaptive_lock(&st->lock);
            }
        } else {
            adaptive_lock(&st->lock);
        }

        /* 1. Verify strict mutual exclusion: no other thread is inside */
        int prev = atomic_fetch_add_explicit(&st->in_critical_section, 1, memory_order_seq_cst);
        if (prev != 0) {
            fprintf(stderr, "FAIL: Mutual exclusion violated! Concurrent holders = %d\n", prev + 1);
            abort();
        }

        /* 2. Verify conservation invariant across all accounts */
        int64_t sum = 0;
        for (int a = 0; a < NUM_ACCOUNTS; ++a) {
            sum += st->accounts[a];
        }
        if (sum != NUM_ACCOUNTS * INITIAL_BAL) {
            fprintf(stderr, "FAIL: Invariant broken! Expected %ld, got %ld\n",
                    (long)(NUM_ACCOUNTS * INITIAL_BAL), (long)sum);
            abort();
        }

        /* 3. Mutate non-atomic shared state across multiple cache lines */
        int src = (int)(rng % NUM_ACCOUNTS);
        int dst = (int)((rng >> 8) % NUM_ACCOUNTS);
        int64_t delta = (int64_t)((rng >> 16) % 500) + 1;
        st->accounts[src] -= delta;
        st->accounts[dst] += delta;

        /* Optional variable critical section delay to exercise estimator & preemption */
        if (arg->inject_spikes && (rng % 32ULL) == 0) {
            uint64_t end = alock_now_ns() + 150000ULL; /* 150 us spike */
            while (alock_now_ns() < end) alock_cpu_relax();
        } else {
            for (volatile int spin = 0; spin < 32; ++spin) {}
        }

        st->total_acquires++;
        st->checksum_history ^= (uint64_t)st->accounts[src] + st->total_acquires;

        /* 4. Leave critical section */
        int after = atomic_fetch_sub_explicit(&st->in_critical_section, 1, memory_order_seq_cst);
        if (after != 1) {
            fprintf(stderr, "FAIL: Exit count mismatch! Expected 1, got %d\n", after);
            abort();
        }

        adaptive_unlock(&st->lock);
    }
    return NULL;
}

static void run_test(const char *label, int threads, int iters, bool pin_cpu, bool spikes) {
    shared_state_t st;
    adaptive_lock_init(&st.lock);
    atomic_init(&st.in_critical_section, 0);
    st.total_acquires = 0;
    st.checksum_history = 0;
    for (int i = 0; i < NUM_ACCOUNTS; ++i) {
        st.accounts[i] = INITIAL_BAL;
    }

    pthread_t *tids = malloc(sizeof(pthread_t) * (size_t)threads);
    test_worker_arg_t *args = malloc(sizeof(test_worker_arg_t) * (size_t)threads);

    for (int i = 0; i < threads; ++i) {
        args[i] = (test_worker_arg_t){
            .st = &st,
            .thread_id = i,
            .iterations = iters,
            .pin_single_cpu = pin_cpu,
            .inject_spikes = spikes,
        };
        assert(pthread_create(&tids[i], NULL, correctness_worker, &args[i]) == 0);
    }

    for (int i = 0; i < threads; ++i) {
        assert(pthread_join(tids[i], NULL) == 0);
    }

    /* Final post-join checks */
    uint64_t expected_ops = (uint64_t)threads * (uint64_t)iters;
    assert(st.total_acquires == expected_ops);
    assert(atomic_load(&st.in_critical_section) == 0);

    uint32_t final_locked = atomic_load(&st.lock.locked);
    uint64_t final_gen = atomic_load(&st.lock.gen);
    assert(final_locked == 0);  /* Lock must be unlocked */
    assert(final_gen > 0);      /* Relaxed generation counter must have advanced */

    int64_t final_sum = 0;
    for (int i = 0; i < NUM_ACCOUNTS; ++i) final_sum += st.accounts[i];
    assert(final_sum == NUM_ACCOUNTS * INITIAL_BAL);

    printf("[PASS] %-38s (acquires=%llu, gen=%llu)\n",
           label,
           (unsigned long long)st.total_acquires,
           (unsigned long long)final_gen);

    free(tids);
    free(args);
}

int main(void) {
    adaptive_lock_calibrate();
    run_test("1. Multi-core tight contention (12T)",  12, 50000, false, false);
    run_test("2. Multi-core bimodal spikes (12T)",     12,  5000, false, true);
    run_test("3. Single-core oversubscribed (12T->1C)",12, 10000, true,  false);
    printf("All correctness and invariant checks passed!\n");
    return 0;
}

