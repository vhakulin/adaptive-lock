# Benchmarks

Measured against `PTHREAD_MUTEX_ADAPTIVE_NP` on a hybrid x86_64 laptop CPU (up to 40 oversubscribed threads) using hardware cycle (`rdtsc`) acquisition timing and memory-prefaulted latency buffers.

* **`Throughput`**: Total critical sections completed per second across all threads.
* **`p50 / p99 / Max Acquire`**: Per-acquisition wait time measured via `rdtsc` immediately around `*_lock()`.
* **`Thread Skew`**: `slowest_thread_time / fastest_thread_time` (`1.00x` = ideal fairness).

Reproduce locally via `make bench`.

---

## 1. Single-Threaded Fast-Path Benchmark (`bench_single_thread`)

Compares uncontended `lock` + `unlock` throughput on the main thread before and after spawning a thread (`__libc_single_threaded` transition on glibc).

```text
=== Before pthread_create (__libc_single_threaded = 1) ===
pthread_mutex :  120218873 ops/s  ( 8.32 ns/op)
adaptive_lock : 2144963681 ops/s  ( 0.47 ns/op)

=== After pthread_create  (__libc_single_threaded = 0) ===
pthread_mutex :   97046401 ops/s  (10.30 ns/op)
adaptive_lock :  233580791 ops/s  ( 4.28 ns/op)
```

---

## 2. Synthetic Contention Benchmark (`bench_synthetic`)

Tests fixed hold/outside critical-section times (`hold_us / outside_us`), 95/5 bimodal spikes (`1 µs` normal, `500 µs` spike), periodic phase shifts (`1 µs` ↔ `400 µs` every 200 iterations), and two threads pinned to SMT sibling logical CPUs `(0, 1)`.

| Scenario                     | Lock          | Throughput       | p50 Acquire      | p99 Acquire       | Max Acquire        | Thread Skew |
| :--------------------------- | :------------ | ---------------: | ---------------: | ----------------: | -----------------: | ----------: |
| 0. Uncontended (1T, 0us)     | pthread_mutex |   43948098 ops/s | p50:     0.01 us | p99:      0.02 us | max:      83.58 us | skew: 1.00x |
| 0. Uncontended (1T, 0us)     | adaptive_lock |   60583697 ops/s | p50:     0.01 us | p99:      0.01 us | max:     127.07 us | skew: 1.00x |
| 1. Short (40T, 1us/5us)      | pthread_mutex |     628480 ops/s | p50:     1.38 us | p99:    648.38 us | max:    3857.73 us | skew: 1.03x |
| 1. Short (40T, 1us/5us)      | adaptive_lock |     773138 ops/s | p50:     6.56 us | p99:    437.36 us | max:   15824.01 us | skew: 1.23x |
| 2. Medium (40T, 10us/50us)   | pthread_mutex |      80881 ops/s | p50:     4.74 us | p99:   6411.40 us | max:   49353.98 us | skew: 1.03x |
| 2. Medium (40T, 10us/50us)   | adaptive_lock |      96731 ops/s | p50:   104.44 us | p99:   3459.22 us | max:   13636.81 us | skew: 1.04x |
| 3. Long (40T, 250us/500us)   | pthread_mutex |       2784 ops/s | p50: 13576.20 us | p99:  54686.15 us | max:  206382.75 us | skew: 1.11x |
| 3. Long (40T, 250us/500us)   | adaptive_lock |       3960 ops/s | p50:  6314.81 us | p99:  39871.57 us | max:   81115.93 us | skew: 1.07x |
| 4. Bimodal 95/5 (40T)        | pthread_mutex |      28032 ops/s | p50:     0.20 us | p99:  26982.51 us | max:  124732.02 us | skew: 1.16x |
| 4. Bimodal 95/5 (40T)        | adaptive_lock |      35509 ops/s | p50:   491.99 us | p99:   7382.43 us | max:   24567.44 us | skew: 1.05x |
| 5. Phase-Shift (40T)         | pthread_mutex |       4387 ops/s | p50:     0.05 us | p99: 225938.44 us | max: 1338183.02 us | skew: 1.61x |
| 5. Phase-Shift (40T)         | adaptive_lock |       4928 ops/s | p50:  3668.50 us | p99:  49359.50 us | max:  149527.42 us | skew: 1.15x |
| 5. SMT Siblings (0,1)        | pthread_mutex |     160668 ops/s | p50:     4.84 us | p99:     12.38 us | max:     530.11 us | skew: 1.00x |
| 5. SMT Siblings (0,1)        | adaptive_lock |     143351 ops/s | p50:     0.01 us | p99:     81.09 us | max:    1826.46 us | skew: 1.00x |

---

## 3. Hash Table Workload Benchmark (`bench_hashtable`)

Contended 1,024-entry chained hash table across 40 threads mixing fast O(1) key upserts with periodic O(N) full-table checksum audits.

| Hash Table Workload           | Lock          | Throughput       | p50 Acquire     | p99 Acquire      | Max Acquire       | Thread Skew |
| :---------------------------- | :------------ | ---------------: | --------------: | ---------------: | ----------------: | ----------: |
| 1. 100% Fast Upsert (40T)     | pthread_mutex |    3501094 ops/s | p50:    0.39 us | p99:   130.50 us | max:    875.96 us | skew: 1.03x |
| 1. 100% Fast Upsert (40T)     | adaptive_lock |    2922215 ops/s | p50:    1.77 us | p99:    28.64 us | max:  11497.54 us | skew: 1.22x |
| 2. 98% Fast / 2% Audit (40T)  | pthread_mutex |    2942614 ops/s | p50:    0.15 us | p99:   165.38 us | max:   1126.10 us | skew: 1.07x |
| 2. 98% Fast / 2% Audit (40T)  | adaptive_lock |    3111163 ops/s | p50:    0.07 us | p99:     9.44 us | max:  11208.97 us | skew: 1.53x |
| 3. 85% Fast / 15% Audit (40T) | pthread_mutex |    1521499 ops/s | p50:    0.26 us | p99:   284.90 us | max:   1599.94 us | skew: 1.11x |
| 3. 85% Fast / 15% Audit (40T) | adaptive_lock |    1474645 ops/s | p50:    0.81 us | p99:   117.18 us | max:   6945.98 us | skew: 1.17x |

