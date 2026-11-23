# Adaptive User-Space Lock (`adaptive_lock`)

A lightweight, header-only C11 mutex designed for **minimal overhead on the uncontended fast path** while avoiding kernel wait queues (`futex`) under heavy contention.

## Motivation & Philosophy

Most OS mutexes penalize either the uncontended path (extra bookkeeping, atomic RMWs on unlock) or the contended path (`futex` transitions, wait-queue contention, and wakeup convoys). Once kernel wait queues are off the table, contended lock acquisition behaves like **network congestion control**: independent agents competing for a shared bottleneck without a central arbiter.

Combining **Little's Law concurrency sizing**, **AIMD spin-slicing**, **Jacobson/Karels hold-time estimation**, and **CPU topology awareness** yields a self-tuning user-space lock that frequently beats `PTHREAD_MUTEX_ADAPTIVE_NP` on uncontended speed, throughput, and p99 tail latency across bimodal and phase-shifting workloads without a single kernel wakeup.

Because correctness depends strictly on a single atomic variable (`locked`) and bounded sleeps, all heuristic telemetry is decoupled from safety and easy to tune.

---

## Architecture & How It Works

### 1. Zero-Overhead Uncontended Path
* **Single-RMW Invariant**: Acquiring `locked` via `atomic_compare_exchange_weak_explicit` (`memory_order_acquire`) is the **only** hardware atomic RMW (`LOCK` prefix) in the implementation—and is elided entirely via `atomic_signal_fence` when glibc's `__libc_single_threaded` is set.
* **Unlock**: A plain `atomic_store_explicit` (`memory_order_release`, compiling to `MOV` on x86_64 TSO) with zero syscalls and no dirty writes to heuristic cache lines unless `owner_cpu` was set.
* **Cache-Line Isolation**: Synchronization (`locked`), spin heuristics (`active_spinners`, `spin_slice`, `owner_cpu`, `gen`), and hold-time telemetry (`est_ns`, `dev_ns`) sit on separate 64-byte cache lines (`alignas(64)`).
* **Relaxed Telemetry & Self-Healing Reset**: Heuristic fields use `_Atomic` types with `memory_order_relaxed` loads/stores (avoiding C11 data-race UB and `LOCK` bus traffic); any upward drift in `active_spinners` resets automatically on the next uncontended acquire.

### 2. Topology & Preemption Awareness
* **Zero-Syscall `rseq` CPU Lookup**: Reads `cpu_id` directly from the thread-local `rseq` block on glibc ≥ 2.35 (falling back to `sched_getcpu()`).
* **SMT Sibling Avoidance**: Parses `/sys/devices/system/cpu/cpu<N>/topology/thread_siblings_list` at startup and skips spinning if the lock holder runs on the same physical core.
* **Cycle Preemption Detection**: Times spin slices with `rdtsc` / `cntvct_el0`, multiplicatively halving `spin_slice` and backing off if an OS preemption gap is detected.
* **Hybrid Core Calibration**: Measures fastest and slowest core `PAUSE`/`YIELD` latencies at startup so E-cores do not trigger false preemption halving.

### 3. Queuing- & Network-Inspired Contention Control
* **Little's Law Spinner Cap (`active_spinners`)**: Caps concurrent spinners to `clamp(sleep_floor / est_ns, 1, num_phys_cores)`—scaling smoothly from all physical cores on sub-microsecond holds down to 1 poller on long holds.
* **Tier 1 Pre-Spin & AIMD Slices**: Runs a ~350 ns register-only `PAUSE` pre-spin before `rdtsc` accounting, then grows `spin_slice` additively on clean slices and halves it on preemption.
* **Alpha-Beta Estimator & Jittered Aging**: Smooths hold duration (`est_ns`) and deviation (`dev_ns`) with outlier clamping, waking sleepers via `clock_nanosleep` inside `[est - dev, est + dev]` with wait-round halving.

---

## Repository Layout

* `adaptive_lock.h` / `adaptive_lock_internal.h` / `adaptive_lock_calibrate.h` — Core lock, internals, and startup calibration.
* `test_correctness.c` — Multi-core, bimodal-spike, and single-core oversubscribed invariant tests.
* `bench_single_thread.c` / `bench_synthetic.c` / `bench_hashtable.c` — Benchmark suite (`BENCHMARKS.md`).
* `Makefile` — `make test`, `make sanitize` (TSan + ASan/UBSan), and `make bench`.

---

## Quick Start

```c
#include "adaptive_lock.h"
#include "adaptive_lock_calibrate.h"

int main(void) {
    adaptive_lock_calibrate(); /* optional; static defaults used if omitted */

    adaptive_lock_t lock;
    adaptive_lock_init(&lock);

    adaptive_lock(&lock);
    /* critical section */
    adaptive_unlock(&lock);
    return 0;
}
```

---

## Benchmarks Summary

See **[BENCHMARKS.md](BENCHMARKS.md)** for full tables against `PTHREAD_MUTEX_ADAPTIVE_NP` (up to 40 oversubscribed threads on a hybrid x86_64 CPU):

* **Uncontended Fast Path**: `0.47 ns/op` (`2.14B ops/s`) single-threaded (`__libc_single_threaded = 1`) and `4.28 ns/op` (`233.6M ops/s`) multi-threaded (**2.4x faster** than `pthread_mutex`).
* **Bimodal & Phase-Shift Contention**: **27% higher throughput** and **3.6x lower p99 latency** (`7.38 ms` vs. `26.98 ms`) under 95/5 spikes; **4.6x lower p99 latency** (`49.4 ms` vs. `225.9 ms`) under phase shifts.
* **Hash Table (98% Fast Upsert / 2% Full Audit)**: **17.5x lower p99 latency** (`9.44 µs` vs. `165.38 µs`) at `3.11M ops/s`.

---

## Trade-Offs & Limitations

* **The Context-Switch / Timer "Dead Zone" (~2–25 µs)**: Every user-space lock without kernel wakeups faces a physical gap between pure cache-coherency spin time (~0.2–2 µs) and the OS `hrtimer` sleep floor (~15–50 µs). Under extreme oversubscription on sub-microsecond critical sections (such as 40 threads contending continuously on a 200 ns hash-table upsert), `futex`-backed locks can park excess threads until an explicit `FUTEX_WAKE`, whereas `adaptive_lock` excels once critical sections or contention bursts exceed the wakeup floor—eliminating `futex` convoy stalls and improving throughput, fairness, and p99 tail latency.
* **Idle CPU on Multi-Second Holds**: Because waiting threads self-wake via `clock_nanosleep` (capped at `ALOCK_MAX_SLEEP_NS = 10 ms`) rather than parking in a kernel `futex` queue, a lock held for seconds under high thread counts will generate periodic timer wakeups (~100 Hz per waiter).
* **Barging vs. Strict FIFO**: Like `pthread_mutex`, this is a non-FIFO lock that allows newly arriving threads to acquire a freshly released lock without waiting for sleeping threads to wake up—maximizing throughput at the expense of strict arrival ordering.
* **Platform Support**: Optimized for Linux (`rseq` / `sched_getcpu()`, `/sys` topology, `clock_nanosleep`), with portable C11 fallbacks on non-Linux systems.
* **Experimental**: This design is experimental. As usual, if this breaks you get to keep all pieces.
* **AI Assistance**: AI assistance was used in the design and implementation.

