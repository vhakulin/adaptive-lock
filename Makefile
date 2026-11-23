CC      ?= gcc
CFLAGS  := -O3 -std=c11 -Wall -Wextra -Wpedantic -pthread
SAN_FLG := -O2 -g -std=c11 -Wall -Wextra -pthread

HEADERS := adaptive_lock.h adaptive_lock_internal.h adaptive_lock_calibrate.h

BINS    := test_correctness bench_single_thread bench_synthetic bench_hashtable
SAN_BINS:= test_tsan test_asan

.PHONY: all test sanitize bench clean

all: test sanitize bench

test_correctness: test_correctness.c $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@

bench_single_thread: bench_single_thread.c $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@

bench_synthetic: bench_synthetic.c $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@

bench_hashtable: bench_hashtable.c $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@

test_tsan: test_correctness.c $(HEADERS)
	$(CC) $(SAN_FLG) -fsanitize=thread $< -o $@

test_asan: test_correctness.c $(HEADERS)
	$(CC) $(SAN_FLG) -fsanitize=address,undefined $< -o $@

test: test_correctness
	./test_correctness

sanitize: test_tsan test_asan
	@echo "=== Running ThreadSanitizer ==="
	@./test_tsan || setarch $$(uname -m) -R ./test_tsan
	@echo "=== Running AddressSanitizer + UBSan ==="
	./test_asan

bench: bench_single_thread bench_synthetic bench_hashtable
	@echo "=== 1. Single-Threaded Fast-Path Benchmark ==="
	./bench_single_thread
	@echo "=== 2. Synthetic Contention Benchmark ==="
	./bench_synthetic
	@echo ""
	@echo "=== 3. Hash Table Workload Benchmark ==="
	./bench_hashtable

clean:
	rm -f $(BINS) $(SAN_BINS)

