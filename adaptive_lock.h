// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ville Repokorpi Hakulinen

#ifndef ADAPTIVE_LOCK_H
#define ADAPTIVE_LOCK_H

#include "adaptive_lock_internal.h"

#define ADAPTIVE_LOCK_INITIALIZER { \
    .locked = 0, \
    .active_spinners = 0, \
    .spin_slice = ALOCK_DEFAULT_INIT_SPIN_SLICE, \
    .owner_cpu = -1, \
    .gen = 0, \
    .est_ns = 50000L, \
    .dev_ns = 25000L, \
}

static inline void adaptive_lock_init(adaptive_lock_t *l) {
    atomic_init(&l->locked, 0);
    atomic_init(&l->active_spinners, 0);
    atomic_init(&l->spin_slice, alock_cfg.init_spin_slice);
    atomic_init(&l->owner_cpu, -1);
    atomic_init(&l->gen, 0);
    atomic_init(&l->est_ns, alock_cfg.min_sleep_ns * 2);
    atomic_init(&l->dev_ns, alock_cfg.min_sleep_ns);
}

static inline bool adaptive_trylock(adaptive_lock_t *l) {
    if (atomic_load_explicit(&l->locked, memory_order_relaxed) != 0) {
        return false;
    }
    return alock_try_acquire_fast(l);
}

static inline int32_t alock_allowed_spinners(int64_t est_ns, int64_t sleep_floor_ns) {
    int64_t denom = est_ns > 500L ? est_ns : 500L;
    int64_t n = sleep_floor_ns / denom;
    if (n < 1) n = 1;
    if (n > alock_cfg.num_phys_cores) n = alock_cfg.num_phys_cores;
    return (int32_t)n;
}

static inline void adaptive_lock(adaptive_lock_t *l) {
    if (ALOCK_IS_SINGLE_THREADED()) {
        if (__builtin_expect(atomic_load_explicit(&l->locked, memory_order_relaxed) == 0, 1)) {
            atomic_store_explicit(&l->locked, 1, memory_order_relaxed);
            atomic_signal_fence(memory_order_acquire);
            return;
        }
    }

    if (__builtin_expect(alock_try_acquire_fast(l), 1)) {
        if (__builtin_expect(atomic_load_explicit(&l->active_spinners, memory_order_relaxed) != 0, 0)) {
            atomic_store_explicit(&l->active_spinners, 0, memory_order_relaxed);
        }
        return;
    }

    const int32_t  min_slice         = alock_cfg.min_spin_slice;
    const int32_t  max_slice         = alock_cfg.max_spin_slice;
    const uint64_t preempt_gap_ticks = alock_cfg.preempt_gap_ticks;
    const int64_t  min_sleep         = alock_cfg.min_sleep_ns;
    const int64_t  sleep_floor       = min_sleep * 2;

    int my_cpu = alock_current_cpu();
    int32_t owner = atomic_load_explicit(&l->owner_cpu, memory_order_relaxed);
    bool same_core = alock_is_same_physical_core(my_cpu, owner);
    int64_t init_est = atomic_load_explicit(&l->est_ns, memory_order_relaxed);

    /* Tier 1: Zero-clock local pre-spin gated by physical core & Little's Law concurrency */
    int32_t init_allowed = alock_allowed_spinners(init_est, sleep_floor);
    int32_t cur_spinners = atomic_load_explicit(&l->active_spinners, memory_order_relaxed);

    if (!same_core && init_est <= sleep_floor && cur_spinners < init_allowed) {
        atomic_store_explicit(&l->active_spinners, cur_spinners + 1, memory_order_relaxed);
        const int32_t pre_iters = alock_cfg.pre_spin_iters;
        for (int32_t i = 0; i < pre_iters; ++i) {
            alock_cpu_relax();
            if (atomic_load_explicit(&l->locked, memory_order_relaxed) == 0 &&
                alock_try_acquire_fast(l)) {
                int32_t sp = atomic_load_explicit(&l->active_spinners, memory_order_relaxed);
                atomic_store_explicit(&l->active_spinners, sp > 1 ? sp - 1 : 0, memory_order_relaxed);
                atomic_store_explicit(&l->owner_cpu, my_cpu, memory_order_relaxed);
                return;
            }
        }
        int32_t sp = atomic_load_explicit(&l->active_spinners, memory_order_relaxed);
        atomic_store_explicit(&l->active_spinners, sp > 1 ? sp - 1 : 0, memory_order_relaxed);
    }

    /* Tier 2: Contended adaptive spin + rdtsc preemption detection + hrtimer sleep */
    uint64_t same_gen = atomic_load_explicit(&l->gen, memory_order_relaxed);
    int64_t same_gen_waited_ns = 0;
    int32_t local_spin_budget = alock_cfg.max_spin_tokens;
    uint32_t sleep_rounds = 0;

    while (true) {
        my_cpu = alock_current_cpu();
        owner = atomic_load_explicit(&l->owner_cpu, memory_order_relaxed);
        same_core = alock_is_same_physical_core(my_cpu, owner);

        int64_t cur_est = atomic_load_explicit(&l->est_ns, memory_order_relaxed);
        int64_t rem_est = cur_est - same_gen_waited_ns;
        bool long_remaining = (rem_est > sleep_floor);

        int32_t max_spinners = alock_allowed_spinners(cur_est, sleep_floor);
        cur_spinners = atomic_load_explicit(&l->active_spinners, memory_order_relaxed);

        if (!same_core && !long_remaining && local_spin_budget > 0 && cur_spinners < max_spinners) {
            int32_t cur_slice = atomic_load_explicit(&l->spin_slice, memory_order_relaxed);
            if (cur_slice < min_slice) cur_slice = min_slice;

            int32_t slice = local_spin_budget < cur_slice ? local_spin_budget : cur_slice;
            local_spin_budget -= slice;

            atomic_store_explicit(&l->active_spinners, cur_spinners + 1, memory_order_relaxed);
            uint64_t spin_t0 = alock_ticks();

            for (int32_t i = 0; i < slice; ++i) {
                if (atomic_load_explicit(&l->locked, memory_order_relaxed) == 0 &&
                    alock_try_acquire_fast(l)) {
                    int32_t sp = atomic_load_explicit(&l->active_spinners, memory_order_relaxed);
                    atomic_store_explicit(&l->active_spinners, sp > 1 ? sp - 1 : 0, memory_order_relaxed);
                    atomic_store_explicit(&l->owner_cpu, my_cpu, memory_order_relaxed);
                    return;
                }
                alock_cpu_relax();
            }

            uint64_t dticks = alock_ticks() - spin_t0;
            int32_t sp = atomic_load_explicit(&l->active_spinners, memory_order_relaxed);
            atomic_store_explicit(&l->active_spinners, sp > 1 ? sp - 1 : 0, memory_order_relaxed);

            bool was_preempted = (dticks > preempt_gap_ticks);

            if (__builtin_expect(was_preempted, 0)) {
                int32_t halved = cur_slice >> 1;
                if (halved < min_slice) halved = min_slice;
                atomic_store_explicit(&l->spin_slice, halved, memory_order_relaxed);
            } else if (slice == cur_slice && cur_slice < max_slice) {
                int32_t grown = cur_slice + (min_slice >> 1);
                if (grown <= cur_slice) grown = cur_slice + 1;
                if (grown > max_slice) grown = max_slice;
                atomic_store_explicit(&l->spin_slice, grown, memory_order_relaxed);
            }

            uint64_t cur_gen = atomic_load_explicit(&l->gen, memory_order_relaxed);
            if (cur_gen == same_gen) {
                same_gen_waited_ns += alock_ticks_to_ns(dticks);
            } else {
                same_gen = cur_gen;
                same_gen_waited_ns = 0;
                local_spin_budget = alock_cfg.max_spin_tokens;
            }

            if (!was_preempted && local_spin_budget > 0) {
                continue;
            }
        }

        /* Targeted jitter sleep with wait-round aging */
        int64_t est = atomic_load_explicit(&l->est_ns, memory_order_relaxed);
        int64_t dev = atomic_load_explicit(&l->dev_ns, memory_order_relaxed);

        int64_t rem = est - same_gen_waited_ns;
        if (rem < min_sleep) rem = min_sleep;

        uint32_t shift = sleep_rounds > 4 ? 4 : sleep_rounds;
        int64_t aged_dev = dev >> shift;
        int64_t aged_rem = rem >> (shift > 1 ? 1 : 0);
        if (aged_rem < min_sleep) aged_rem = min_sleep;

        int64_t low = aged_rem - aged_dev;
        if (low < min_sleep) low = min_sleep;
        int64_t high = aged_rem + aged_dev;
        if (high < low) high = low;
        if (high > ALOCK_MAX_SLEEP_NS) high = ALOCK_MAX_SLEEP_NS;

        uint64_t span = (uint64_t)(high - low) + 1ULL;
        int64_t target_sleep = low + (int64_t)(alock_tls_rand() % span);

        uint64_t t0 = alock_ticks();
        alock_sleep_ns(target_sleep);
        int64_t actual_slept = alock_ticks_to_ns(alock_ticks() - t0);

        uint64_t cur_gen = atomic_load_explicit(&l->gen, memory_order_relaxed);
        uint32_t is_locked = atomic_load_explicit(&l->locked, memory_order_relaxed);

        if (cur_gen == same_gen) {
            same_gen_waited_ns += actual_slept;
            if (is_locked && same_gen_waited_ns > atomic_load_explicit(&l->est_ns, memory_order_relaxed)) {
                alock_update_estimator(l, same_gen_waited_ns);
            }
        } else {
            uint64_t delta = cur_gen - same_gen;
            int64_t avg_cycle_ns = actual_slept / (int64_t)delta;
            alock_update_estimator(l, avg_cycle_ns);
            same_gen = cur_gen;
            same_gen_waited_ns = 0;
            local_spin_budget = alock_cfg.max_spin_tokens;
        }

        if (is_locked == 0 && alock_try_acquire_fast(l)) {
            atomic_store_explicit(&l->owner_cpu, alock_current_cpu(), memory_order_relaxed);
            return;
        }
        sleep_rounds++;
    }
}

static inline void adaptive_unlock(adaptive_lock_t *l) {
    if (ALOCK_IS_SINGLE_THREADED()) {
        atomic_signal_fence(memory_order_release);
        atomic_store_explicit(&l->locked, 0, memory_order_relaxed);
        return;
    }

    /* Avoid dirtying Cache Line 1 on uncontended unlocks where owner_cpu is already -1 */
    if (__builtin_expect(atomic_load_explicit(&l->owner_cpu, memory_order_relaxed) != -1, 0)) {
        atomic_store_explicit(&l->owner_cpu, -1, memory_order_relaxed);
    }

    /* Plain relaxed increment (no LOCK prefix) so sleepers/spinners see cycle progress */
    uint64_t g = atomic_load_explicit(&l->gen, memory_order_relaxed);
    atomic_store_explicit(&l->gen, g + 1ULL, memory_order_relaxed);

    /* Compiles to a plain MOV $0 on x86_64 TSO — zero LOCK prefix */
    atomic_store_explicit(&l->locked, 0, memory_order_release);
}

#endif /* ADAPTIVE_LOCK_H */

