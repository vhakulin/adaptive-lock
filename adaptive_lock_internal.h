// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ville Repokorpi Hakulinen

#ifndef ADAPTIVE_LOCK_INTERNAL_H
#define ADAPTIVE_LOCK_INTERNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdatomic.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __linux__
#include <sched.h>
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 35))
#include <sys/rseq.h>
static inline int alock_current_cpu(void) {
    const volatile struct rseq *rseq_ptr =
        (const volatile struct rseq *)((const char *)__builtin_thread_pointer() + __rseq_offset);
    int cpu = (int)rseq_ptr->cpu_id;
    return __builtin_expect(cpu >= 0, 1) ? cpu : sched_getcpu();
}
#else
#define alock_current_cpu() sched_getcpu()
#endif
#else
#define alock_current_cpu() (-1)
#endif

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 32))
#include <sys/single_threaded.h>
#define ALOCK_IS_SINGLE_THREADED() (__builtin_expect(__libc_single_threaded != 0, 0))
#else
#define ALOCK_IS_SINGLE_THREADED() (0)
#endif

#if defined(__x86_64__) || defined(__i386__)
#define alock_cpu_relax() __builtin_ia32_pause()
static inline uint64_t alock_ticks(void) {
    return __builtin_ia32_rdtsc();
}
#define ALOCK_HAS_HW_TICKS 1
#elif defined(__aarch64__)
#define alock_cpu_relax() __asm__ __volatile__("yield" ::: "memory")
static inline uint64_t alock_ticks(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#define ALOCK_HAS_HW_TICKS 1
#else
#define alock_cpu_relax() ((void)0)
#define ALOCK_HAS_HW_TICKS 0
#endif

/* Fallback defaults (used if adaptive_lock_calibrate() is not called) */
#define ALOCK_DEFAULT_PRE_SPIN_ITERS   64
#define ALOCK_DEFAULT_MAX_SPIN_TOKENS  1024
#define ALOCK_DEFAULT_INIT_SPIN_SLICE  64
#define ALOCK_DEFAULT_MIN_SPIN_SLICE   8
#define ALOCK_DEFAULT_MAX_SPIN_SLICE   256
#define ALOCK_DEFAULT_PREEMPT_GAP_NS   15000L
#define ALOCK_DEFAULT_MIN_SLEEP_NS     25000L
#define ALOCK_DEFAULT_PHYS_CORES       8
#define ALOCK_MAX_SLEEP_NS             10000000L
#define ALOCK_MAX_CPUS                 1024

typedef struct {
    int32_t  pre_spin_iters;
    int32_t  max_spin_tokens;
    int32_t  init_spin_slice;
    int32_t  min_spin_slice;
    int32_t  max_spin_slice;
    int32_t  num_phys_cores;
    int64_t  preempt_gap_ns;
    uint64_t preempt_gap_ticks;
    uint32_t ticks_to_ns_mult;
    int64_t  min_sleep_ns;
} alock_config_t;

static alock_config_t alock_cfg = {
    .pre_spin_iters    = ALOCK_DEFAULT_PRE_SPIN_ITERS,
    .max_spin_tokens   = ALOCK_DEFAULT_MAX_SPIN_TOKENS,
    .init_spin_slice   = ALOCK_DEFAULT_INIT_SPIN_SLICE,
    .min_spin_slice    = ALOCK_DEFAULT_MIN_SPIN_SLICE,
    .max_spin_slice    = ALOCK_DEFAULT_MAX_SPIN_SLICE,
    .num_phys_cores    = ALOCK_DEFAULT_PHYS_CORES,
    .preempt_gap_ns    = ALOCK_DEFAULT_PREEMPT_GAP_NS,
    .preempt_gap_ticks = ALOCK_DEFAULT_PREEMPT_GAP_NS * 3ULL,
    .ticks_to_ns_mult  = 5592405U,
    .min_sleep_ns      = ALOCK_DEFAULT_MIN_SLEEP_NS,
};

static int16_t alock_phys_core[ALOCK_MAX_CPUS];
static bool alock_phys_map_ready = false;

typedef struct {
    /* Cache line 0: Pure synchronization bit (0 = unlocked, 1 = locked) */
    alignas(64) _Atomic uint32_t locked;

    /* Cache line 1: Heuristic spinner concurrency, AIMD slice, owner hint, & relaxed gen */
    alignas(64) _Atomic int32_t active_spinners;
    _Atomic int32_t spin_slice;
    _Atomic int32_t owner_cpu;
    _Atomic uint64_t gen;

    /* Cache line 2: Heuristic integer alpha-beta hold-time estimator */
    alignas(64) _Atomic int64_t est_ns;
    _Atomic int64_t dev_ns;
} adaptive_lock_t;

static inline uint64_t alock_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#if !ALOCK_HAS_HW_TICKS
static inline uint64_t alock_ticks(void) {
    return alock_now_ns();
}
#endif

static inline int64_t alock_ticks_to_ns(uint64_t dticks) {
#if ALOCK_HAS_HW_TICKS
    return (int64_t)((dticks * (uint64_t)alock_cfg.ticks_to_ns_mult) >> 24);
#else
    return (int64_t)dticks;
#endif
}

static inline uint64_t alock_tls_rand(void) {
    static __thread uint64_t seed = 0;
    if (__builtin_expect(seed == 0, 0)) {
        seed = alock_ticks() ^ (uintptr_t)&seed;
    }
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

static inline void alock_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    struct timespec req = {
        .tv_sec = ns / 1000000000L,
        .tv_nsec = ns % 1000000000L,
    };
    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
}

static inline bool alock_is_same_physical_core(int cpu_a, int cpu_b) {
    if (cpu_a < 0 || cpu_b < 0) return false;
    if (cpu_a == cpu_b) return true;
    if (__builtin_expect(alock_phys_map_ready, 1) &&
        cpu_a < ALOCK_MAX_CPUS && cpu_b < ALOCK_MAX_CPUS) {
        return alock_phys_core[cpu_a] == alock_phys_core[cpu_b];
    }
    return false;
}

static inline bool alock_try_acquire_fast(adaptive_lock_t *l) {
    if (ALOCK_IS_SINGLE_THREADED()) {
        if (atomic_load_explicit(&l->locked, memory_order_relaxed) == 0) {
            atomic_store_explicit(&l->locked, 1, memory_order_relaxed);
            atomic_signal_fence(memory_order_acquire);
            return true;
        }
        return false;
    }
    uint32_t expected = 0;
    return atomic_compare_exchange_weak_explicit(
        &l->locked, &expected, 1, memory_order_acquire, memory_order_relaxed);
}

static inline void alock_update_estimator(adaptive_lock_t *l, int64_t sample_ns) {
    if (sample_ns < 500L) sample_ns = 500L;
    if (sample_ns > ALOCK_MAX_SLEEP_NS) sample_ns = ALOCK_MAX_SLEEP_NS;

    int64_t est = atomic_load_explicit(&l->est_ns, memory_order_relaxed);
    int64_t dev = atomic_load_explicit(&l->dev_ns, memory_order_relaxed);

    int64_t max_step = est << 2;
    if (sample_ns > max_step) sample_ns = max_step;

    int64_t err = sample_ns - est;
    int64_t abs_err = err < 0 ? -err : err;

    atomic_store_explicit(&l->est_ns, est + (err >> 3), memory_order_relaxed);
    atomic_store_explicit(&l->dev_ns, dev + ((abs_err - dev) >> 2), memory_order_relaxed);
}

#endif /* ADAPTIVE_LOCK_INTERNAL_H */

