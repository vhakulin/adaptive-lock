#define _GNU_SOURCE
#include "adaptive_lock.h"
#include "adaptive_lock_calibrate.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_BUCKETS 128
#define NTHREADS 40

typedef struct entry {
    uint64_t key;
    uint64_t value;
    struct entry *next;
} entry_t;

typedef struct {
    entry_t **buckets;
    size_t num_buckets;
    size_t size;
    uint64_t checksum;
} hash_table_t;

typedef enum { LOCK_ADAPTIVE, LOCK_PTHREAD } lock_kind_t;

typedef struct {
    lock_kind_t kind;
    adaptive_lock_t *alock;
    pthread_mutex_t *pmutex;
    hash_table_t *ht;
    int thread_id;
    int iterations;
    int scan_pct;      /* % of ops that perform an O(N) table scan/maintenance */
    int outside_us;
    uint64_t *latencies_ns;
    uint64_t thread_elapsed_ns;
} ds_bench_arg_t;

static inline void busy_wait_us(int us) {
    if (us <= 0) return;
    uint64_t target_ns = (uint64_t)us * 1000ULL;
    uint64_t t0 = alock_ticks();
    while ((uint64_t)alock_ticks_to_ns(alock_ticks() - t0) < target_ns) {
        alock_cpu_relax();
    }
}

static void ht_init(hash_table_t *ht, size_t buckets) {
    ht->buckets = calloc(buckets, sizeof(entry_t *));
    ht->num_buckets = buckets;
    ht->size = 0;
    ht->checksum = 0;

    /* Pre-populate with 1,024 keys */
    for (uint64_t k = 1; k <= 1024; ++k) {
        size_t idx = (k * 11400714819323198485ULL) % buckets;
        entry_t *e = malloc(sizeof(entry_t));
        e->key = k;
        e->value = k * 3;
        e->next = ht->buckets[idx];
        ht->buckets[idx] = e;
        ht->size++;
    }
}

static void ht_destroy(hash_table_t *ht) {
    for (size_t i = 0; i < ht->num_buckets; ++i) {
        entry_t *cur = ht->buckets[i];
        while (cur) {
            entry_t *nxt = cur->next;
            free(cur);
            cur = nxt;
        }
    }
    free(ht->buckets);
}

/* Fast O(1) lookup-and-increment inside critical section */
static inline void ht_upsert_fast(hash_table_t *ht, uint64_t key) {
    size_t idx = (key * 11400714819323198485ULL) % ht->num_buckets;
    entry_t *cur = ht->buckets[idx];
    while (cur) {
        if (cur->key == key) {
            cur->value++;
            ht->checksum += cur->value;
            return;
        }
        cur = cur->next;
    }
}

/* Heavy O(N) table audit/aggregation inside critical section (~50-200 us) */
static inline void ht_full_audit(hash_table_t *ht) {
    uint64_t acc = 0;
    for (size_t i = 0; i < ht->num_buckets; ++i) {
        entry_t *cur = ht->buckets[i];
        while (cur) {
            acc ^= (cur->key * 31ULL) + cur->value;
            cur = cur->next;
        }
    }
    ht->checksum ^= acc;
}

static void *ds_worker(void *ptr) {
    ds_bench_arg_t *arg = (ds_bench_arg_t *)ptr;
    uint64_t rng = (uint64_t)(arg->thread_id + 1) * 0xdeadbeefcafebabeULL;
    uint64_t thread_t0 = alock_now_ns();

    for (int i = 0; i < arg->iterations; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        uint64_t key = (rng % 1024ULL) + 1ULL;
        bool do_audit = ((int)(rng % 100ULL) < arg->scan_pct);

        uint64_t t0 = alock_ticks();
        if (arg->kind == LOCK_ADAPTIVE) {
            adaptive_lock(arg->alock);
        } else {
            pthread_mutex_lock(arg->pmutex);
        }
        uint64_t t1 = alock_ticks();

        if (do_audit) {
            ht_full_audit(arg->ht);
        } else {
            ht_upsert_fast(arg->ht, key);
        }

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

static void run_ds_bench(const char *name, lock_kind_t kind, int threads,
                         int iters, int scan_pct, int outside_us, bool is_warmup) {
    adaptive_lock_t alock;
    pthread_mutex_t pmutex;
    adaptive_lock_init(&alock);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
    pthread_mutex_init(&pmutex, &attr);
    pthread_mutexattr_destroy(&attr);

    hash_table_t ht;
    ht_init(&ht, INITIAL_BUCKETS);

    size_t total_ops = (size_t)threads * (size_t)iters;
    uint64_t *all_lats = malloc(sizeof(uint64_t) * total_ops);
    pthread_t *tids = malloc(sizeof(pthread_t) * (size_t)threads);
    ds_bench_arg_t *args = malloc(sizeof(ds_bench_arg_t) * (size_t)threads);

    /* Prefault latency buffer so minor page faults don't distort acquisition timing */
    memset(all_lats, 0, sizeof(uint64_t) * total_ops);

    uint64_t t_start = alock_now_ns();
    for (int i = 0; i < threads; ++i) {
        args[i] = (ds_bench_arg_t){
            .kind = kind,
            .alock = &alock,
            .pmutex = &pmutex,
            .ht = &ht,
            .thread_id = i,
            .iterations = iters,
            .scan_pct = scan_pct,
            .outside_us = outside_us,
            .latencies_ns = all_lats + (size_t)i * (size_t)iters,
            .thread_elapsed_ns = 0,
        };
        pthread_create(&tids[i], NULL, ds_worker, &args[i]);
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

        printf("| %-29s | %-13s | %10.0f ops/s | p50: %7.2f us | p99: %8.2f us | max: %9.2f us | skew: %4.2fx |\n",
               name,
               kind == LOCK_ADAPTIVE ? "adaptive_lock" : "pthread_mutex",
               ops_per_sec,
               (double)p50 / 1000.0,
               (double)p99 / 1000.0,
               (double)max_lat / 1000.0,
               fairness_ratio);
    }

    ht_destroy(&ht);
    pthread_mutex_destroy(&pmutex);
    free(all_lats);
    free(tids);
    free(args);
}

int main(void) {
    adaptive_lock_calibrate();

    /* Discard warmup runs to ramp CPU clocks */
    run_ds_bench("warmup", LOCK_PTHREAD,  4, 25000, 0, 0, true);
    run_ds_bench("warmup", LOCK_ADAPTIVE, 4, 25000, 0, 0, true);

    printf("| %-29s | %-13s | %-16s | %-15s | %-16s | %-17s | %-11s |\n",
           "Hash Table Workload", "Lock", "Throughput", "p50 Acquire", "p99 Acquire", "Max Acquire", "Thread Skew");
    printf("| :---------------------------- | :------------ | ---------------: | --------------: | ---------------: | ----------------: | ----------: | \n");

    run_ds_bench("1. 100% Fast Upsert (40T)",     LOCK_PTHREAD,  NTHREADS, 30000,  0,  3, false);
    run_ds_bench("1. 100% Fast Upsert (40T)",     LOCK_ADAPTIVE, NTHREADS, 30000,  0,  3, false);
    run_ds_bench("2. 98% Fast / 2% Audit (40T)",  LOCK_PTHREAD,  NTHREADS, 15000,  2,  5, false);
    run_ds_bench("2. 98% Fast / 2% Audit (40T)",  LOCK_ADAPTIVE, NTHREADS, 15000,  2,  5, false);
    run_ds_bench("3. 85% Fast / 15% Audit (40T)", LOCK_PTHREAD,  NTHREADS,  5000, 15, 10, false);
    run_ds_bench("3. 85% Fast / 15% Audit (40T)", LOCK_ADAPTIVE, NTHREADS,  5000, 15, 10, false);

    return 0;
}

