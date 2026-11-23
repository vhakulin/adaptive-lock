// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ville Repokorpi Hakulinen

#ifndef ADAPTIVE_LOCK_CALIBRATE_H
#define ADAPTIVE_LOCK_CALIBRATE_H

#include "adaptive_lock_internal.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef __linux__
#include <unistd.h>

static inline void alock_build_smt_map(void) {
    for (int i = 0; i < ALOCK_MAX_CPUS; ++i) {
        alock_phys_core[i] = (int16_t)i;
    }

    int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus <= 0) return;
    if (ncpus > ALOCK_MAX_CPUS) ncpus = ALOCK_MAX_CPUS;

    int32_t unique_phys = 0;

    for (int cpu = 0; cpu < ncpus; ++cpu) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char buf[256];
        if (fgets(buf, sizeof(buf), f)) {
            int first_sibling = -1;
            char *p = buf;
            while (*p) {
                if (*p >= '0' && *p <= '9') {
                    int id = (int)strtol(p, &p, 10);
                    if (first_sibling < 0 || id < first_sibling) {
                        first_sibling = id;
                    }
                } else {
                    p++;
                }
            }
            if (first_sibling >= 0 && first_sibling < ALOCK_MAX_CPUS) {
                alock_phys_core[cpu] = (int16_t)first_sibling;
                if (first_sibling == cpu) {
                    unique_phys++;
                }
            }
        }
        fclose(f);
    }

    if (unique_phys > 0) {
        alock_cfg.num_phys_cores = unique_phys;
    }
    alock_phys_map_ready = true;
}

static inline double alock_measure_spin_ns_on_current_cpu(void) {
    const int iters = 4096;
    double best_ns_per_iter = 1e9;

    for (int i = 0; i < 512; ++i) alock_cpu_relax();

    for (int trial = 0; trial < 5; ++trial) {
        uint64_t t0 = alock_now_ns();
        for (int i = 0; i < iters; ++i) {
            alock_cpu_relax();
        }
        uint64_t dt = alock_now_ns() - t0;
        double ns_per = (double)dt / (double)iters;
        if (ns_per < best_ns_per_iter) {
            best_ns_per_iter = ns_per;
        }
    }
    return best_ns_per_iter;
}
#endif

static inline int alock_cmp_i64(const void *a, const void *b) {
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

static inline void adaptive_lock_calibrate(void) {
    double fast_spin_ns = 4.0;
    double slow_spin_ns = 8.0;

    uint64_t ns0 = alock_now_ns();
    uint64_t tk0 = alock_ticks();
    alock_sleep_ns(2000000L);
    uint64_t dns = alock_now_ns() - ns0;
    uint64_t dtk = alock_ticks() - tk0;
    double ticks_per_ns = (dns > 0 && dtk > 0) ? ((double)dtk / (double)dns) : 3.0;

#ifdef __linux__
    alock_build_smt_map();

    cpu_set_t orig_mask;
    bool have_orig_mask = (sched_getaffinity(0, sizeof(orig_mask), &orig_mask) == 0);

    fast_spin_ns = 1e9;
    slow_spin_ns = 0.0;

    if (have_orig_mask) {
        int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
        if (ncpus > ALOCK_MAX_CPUS) ncpus = ALOCK_MAX_CPUS;

        for (int cpu = 0; cpu < ncpus; ++cpu) {
            if (!CPU_ISSET(cpu, &orig_mask)) continue;
            cpu_set_t single;
            CPU_ZERO(&single);
            CPU_SET(cpu, &single);
            if (sched_setaffinity(0, sizeof(single), &single) == 0) {
                double ns = alock_measure_spin_ns_on_current_cpu();
                if (ns < fast_spin_ns) fast_spin_ns = ns;
                if (ns > slow_spin_ns) slow_spin_ns = ns;
            }
        }
        sched_setaffinity(0, sizeof(orig_mask), &orig_mask);
    }

    if (slow_spin_ns <= 0.0 || fast_spin_ns > 1e8) {
        double ns = alock_measure_spin_ns_on_current_cpu();
        fast_spin_ns = ns;
        slow_spin_ns = ns * 1.5;
    }
#endif

    if (fast_spin_ns < 0.2) fast_spin_ns = 0.2;
    if (slow_spin_ns < fast_spin_ns) slow_spin_ns = fast_spin_ns;

    int64_t sleep_samples[11];
    alock_sleep_ns(1000);
    for (int i = 0; i < 11; ++i) {
        uint64_t t0 = alock_now_ns();
        alock_sleep_ns(1000);
        sleep_samples[i] = (int64_t)(alock_now_ns() - t0);
    }
    qsort(sleep_samples, 11, sizeof(int64_t), alock_cmp_i64);
    int64_t measured_sleep_floor = sleep_samples[5];
    if (measured_sleep_floor < 10000L) measured_sleep_floor = 10000L;
    if (measured_sleep_floor > 200000L) measured_sleep_floor = 200000L;

    /* Pre-spin: ~350 ns of zero-clock local polling */
    int32_t pre_iters = (int32_t)(350.0 / fast_spin_ns);
    if (pre_iters < 16)  pre_iters = 16;
    if (pre_iters > 512) pre_iters = 512;

    /* Target ~1.5 us per spin slice on fastest core */
    int32_t max_slice = (int32_t)(1500.0 / fast_spin_ns);
    if (max_slice < 64)   max_slice = 64;
    if (max_slice > 2048) max_slice = 2048;

    int32_t min_slice = max_slice / 16;
    if (min_slice < 8) min_slice = 8;

    int32_t init_slice = max_slice / 2;
    if (init_slice < min_slice) init_slice = min_slice;

    /* Per-thread spin budget: ~25% of one hrtimer sleep floor */
    int32_t max_tokens = (int32_t)((double)(measured_sleep_floor / 4) / fast_spin_ns);
    if (max_tokens < max_slice * 2) max_tokens = max_slice * 2;
    if (max_tokens > 8192)          max_tokens = 8192;

    int64_t slow_max_slice_ns = (int64_t)((double)max_slice * slow_spin_ns);
    int64_t preempt_gap = slow_max_slice_ns * 5;
    if (preempt_gap < 10000L)  preempt_gap = 10000L;
    if (preempt_gap > 100000L) preempt_gap = 100000L;

    alock_cfg.pre_spin_iters    = pre_iters;
    alock_cfg.max_spin_tokens   = max_tokens;
    alock_cfg.init_spin_slice   = init_slice;
    alock_cfg.min_spin_slice    = min_slice;
    alock_cfg.max_spin_slice    = max_slice;
    alock_cfg.preempt_gap_ns    = preempt_gap;
    alock_cfg.preempt_gap_ticks = (uint64_t)((double)preempt_gap * ticks_per_ns);
    alock_cfg.ticks_to_ns_mult  = (uint32_t)((16777216.0 / ticks_per_ns) + 0.5);
    alock_cfg.min_sleep_ns      = measured_sleep_floor / 2;
    if (alock_cfg.min_sleep_ns < 10000L) alock_cfg.min_sleep_ns = 10000L;
}

#endif /* ADAPTIVE_LOCK_CALIBRATE_H */

