#define _GNU_SOURCE
#include "adaptive_lock.h"
#include "adaptive_lock_calibrate.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 50000000ULL

static void *noop_thread(void *arg) {
    (void)arg;
    return NULL;
}

static void bench_main_thread(const char *label) {
    adaptive_lock_t alock;
    pthread_mutex_t pmutex;
    adaptive_lock_init(&alock);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
    pthread_mutex_init(&pmutex, &attr);
    pthread_mutexattr_destroy(&attr);

    volatile uint64_t sink = 0;

    /* Warmup */
    for (uint64_t i = 0; i < 1000000ULL; ++i) {
        pthread_mutex_lock(&pmutex);
        sink++;
        pthread_mutex_unlock(&pmutex);
    }

    uint64_t t0 = alock_now_ns();
    for (uint64_t i = 0; i < ITERS; ++i) {
        pthread_mutex_lock(&pmutex);
        sink++;
        pthread_mutex_unlock(&pmutex);
    }
    uint64_t dt_pthread = alock_now_ns() - t0;

    /* Warmup */
    for (uint64_t i = 0; i < 1000000ULL; ++i) {
        adaptive_lock(&alock);
        sink++;
        adaptive_unlock(&alock);
    }

    t0 = alock_now_ns();
    for (uint64_t i = 0; i < ITERS; ++i) {
        adaptive_lock(&alock);
        sink++;
        adaptive_unlock(&alock);
    }
    uint64_t dt_adaptive = alock_now_ns() - t0;

    printf("=== %s (__libc_single_threaded = %d) ===\n",
           label, (int)ALOCK_IS_SINGLE_THREADED());
    printf("pthread_mutex : %10.0f ops/s  (%5.2f ns/op)\n",
           (double)ITERS / ((double)dt_pthread / 1e9),
           (double)dt_pthread / (double)ITERS);
    printf("adaptive_lock : %10.0f ops/s  (%5.2f ns/op)\n\n",
           (double)ITERS / ((double)dt_adaptive / 1e9),
           (double)dt_adaptive / (double)ITERS);
    (void)sink;
    pthread_mutex_destroy(&pmutex);
}

int main(void) {
    adaptive_lock_calibrate();

    /* Phase 1: Pure single-threaded process (before any pthread_create) */
    bench_main_thread("Before pthread_create");

    /* Spawn and join a no-op thread -> permanently flips __libc_single_threaded to 0 */
    pthread_t tid;
    pthread_create(&tid, NULL, noop_thread, NULL);
    pthread_join(tid, NULL);

    /* Phase 2: Same main thread, now in multi-threaded mode */
    bench_main_thread("After pthread_create ");

    return 0;
}

