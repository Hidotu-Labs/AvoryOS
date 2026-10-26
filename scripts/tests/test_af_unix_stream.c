#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <x86intrin.h>

/* --- New Fast Power-of-Two Ring Buffer (from af_unix_internal.h) --- */

static inline size_t unix_fast_round_pow2(size_t n) {
  if (n <= 65536)
    return 65536;
  if (n >= 4 * 1024 * 1024)
    return 4 * 1024 * 1024;
  size_t p = 65536;
  while (p < n && p < 4 * 1024 * 1024)
    p <<= 1;
  return p;
}

typedef struct {
  uint8_t *buf;
  size_t size;
  size_t mask;
  size_t head;
  size_t tail;
  size_t count;
} fast_unix_ring_t;

static void fast_ring_init(fast_unix_ring_t *r, size_t initial_sz) {
  r->size = unix_fast_round_pow2(initial_sz);
  r->mask = r->size - 1;
  r->buf = malloc(r->size);
  r->head = 0;
  r->tail = 0;
  r->count = 0;
}

static void fast_ring_free(fast_unix_ring_t *r) {
  free(r->buf);
  r->buf = NULL;
}

static inline size_t fast_ring_append(fast_unix_ring_t *r, const uint8_t *src, size_t len) {
  size_t space = r->size - r->count;
  size_t to_copy = len < space ? len : space;
  if (to_copy == 0) return 0;

  size_t first = r->size - r->tail;
  if (first > to_copy) first = to_copy;
  memcpy(r->buf + r->tail, src, first);
  if (to_copy > first)
    memcpy(r->buf, src + first, to_copy - first);

  r->tail = (r->tail + to_copy) & r->mask;
  r->count += to_copy;
  return to_copy;
}

static inline size_t fast_ring_consume(fast_unix_ring_t *r, uint8_t *dst, size_t len) {
  size_t to_copy = len < r->count ? len : r->count;
  if (to_copy == 0) return 0;

  size_t first = r->size - r->head;
  if (first > to_copy) first = to_copy;
  memcpy(dst, r->buf + r->head, first);
  if (to_copy > first)
    memcpy(dst + first, r->buf, to_copy - first);

  r->head = (r->head + to_copy) & r->mask;
  r->count -= to_copy;

  // Contiguous reset for ping-pong IPC
  if (r->count == 0) {
    r->head = 0;
    r->tail = 0;
  }
  return to_copy;
}

/* --- Old AF_UNIX Ring Buffer (Modulo Divides & Non-Power-of-Two) --- */

typedef struct {
  uint8_t *buf;
  size_t size;
  size_t head;
  size_t tail;
} old_unix_ring_t;

static void old_ring_init(old_unix_ring_t *r, size_t initial_sz) {
  r->size = initial_sz < 65536 ? 65536 : initial_sz + 4096; // non-power-of-two
  r->buf = malloc(r->size);
  r->head = 0;
  r->tail = 0;
}

static void old_ring_free(old_unix_ring_t *r) {
  free(r->buf);
  r->buf = NULL;
}

static inline size_t old_ring_append(old_unix_ring_t *r, const uint8_t *src, size_t len) {
  size_t size = r->size;
  size_t space = (size > 0) ? (r->head - r->tail - 1 + size) % size : 0;
  size_t to_copy = len < space ? len : space;
  if (to_copy == 0) return 0;

  size_t first = size - r->tail;
  if (first > to_copy) first = to_copy;
  memcpy(r->buf + r->tail, src, first);
  if (to_copy > first)
    memcpy(r->buf, src + first, to_copy - first);

  r->tail += to_copy;
  if (r->tail >= size) r->tail -= size;
  return to_copy;
}

static inline size_t old_ring_consume(old_unix_ring_t *r, uint8_t *dst, size_t len) {
  size_t size = r->size;
  size_t available = (size > 0) ? (r->tail - r->head + size) % size : 0;
  size_t to_copy = len < available ? len : available;
  if (to_copy == 0) return 0;

  size_t first = size - r->head;
  if (first > to_copy) first = to_copy;
  memcpy(dst, r->buf + r->head, first);
  if (to_copy > first)
    memcpy(dst + first, r->buf, to_copy - first);

  r->head += to_copy;
  if (r->head >= size) r->head -= size;
  // No contiguous reset in old code
  return to_copy;
}

/* Correctness Test */
static void run_correctness_tests(void) {
  printf("[*] Running AF_UNIX Ring Buffer Correctness Tests...\n");
  fast_unix_ring_t r;
  fast_ring_init(&r, 65536);
  assert(r.size == 65536);
  assert(r.mask == 65535);

  uint8_t *wbuf = malloc(65536);
  uint8_t *rbuf = malloc(65536);

  uint32_t stream_seed = 0;
  uint32_t total_transferred = 0;

  for (int iter = 0; iter < 50000; iter++) {
    uint32_t sz = 1 + (iter % 3000);
    // Flow control: if not enough space, drain first
    while (r.size - r.count < sz) {
      uint32_t read_bytes = (uint32_t)fast_ring_consume(&r, rbuf, 4096);
      for (uint32_t i = 0; i < read_bytes; i++) {
        assert(rbuf[i] == (uint8_t)(total_transferred + i));
      }
      total_transferred += read_bytes;
    }

    for (uint32_t i = 0; i < sz; i++) {
      wbuf[i] = (uint8_t)(stream_seed + i);
    }
    size_t written = fast_ring_append(&r, wbuf, sz);
    assert(written == sz);
    stream_seed += sz;

    uint32_t rsz = 1 + ((iter * 11) % 3000);
    if (rsz > r.count) rsz = (uint32_t)r.count;
    if (rsz > 0) {
      uint32_t read_bytes = (uint32_t)fast_ring_consume(&r, rbuf, rsz);
      assert(read_bytes == rsz);
      for (uint32_t i = 0; i < read_bytes; i++) {
        assert(rbuf[i] == (uint8_t)(total_transferred + i));
      }
      total_transferred += read_bytes;
    }
  }

  // Drain remainder
  while (r.count > 0) {
    uint32_t read_bytes = (uint32_t)fast_ring_consume(&r, rbuf, 65536);
    for (uint32_t i = 0; i < read_bytes; i++) {
      assert(rbuf[i] == (uint8_t)(total_transferred + i));
    }
    total_transferred += read_bytes;
  }

  assert(total_transferred == stream_seed);
  assert(r.head == 0 && r.tail == 0 && r.count == 0);
  printf("    -> Transferred %u bytes across wrap boundaries: PASS\n", total_transferred);
  printf("    -> Drained ring contiguous reset (head=0, tail=0): PASS\n");

  fast_ring_free(&r);
  free(wbuf);
  free(rbuf);
}

/* Benchmark */
static void run_benchmarks(void) {
  printf("\n[*] Benchmarking AF_UNIX Desktop IPC (Fast Ring vs Old Modulo Ring)...\n");

  const int iterations = 100000;
  const uint32_t chunk_sizes[] = {512, 4096, 16384, 32768, 65536};
  const int num_sizes = sizeof(chunk_sizes) / sizeof(chunk_sizes[0]);

  uint8_t *wbuf = malloc(65536);
  uint8_t *rbuf = malloc(65536);
  memset(wbuf, 0x5A, 65536);

  for (int s = 0; s < num_sizes; s++) {
    uint32_t chunk = chunk_sizes[s];
    printf("\n  [+] Chunk size: %u bytes (%d ping-pong round-trips):\n", chunk, iterations);

    // 1. Benchmark Old Modulo Ring
    old_unix_ring_t old_r;
    old_ring_init(&old_r, 65536);

    uint64_t t0 = __rdtsc();
    for (int i = 0; i < iterations; i++) {
      old_ring_append(&old_r, wbuf, chunk);
      old_ring_consume(&old_r, rbuf, chunk);
    }
    uint64_t t1 = __rdtsc();
    uint64_t old_cycles = t1 - t0;
    old_ring_free(&old_r);

    // 2. Benchmark Fast Power-of-Two Contiguous Reset Ring
    fast_unix_ring_t fast_r;
    fast_ring_init(&fast_r, 65536);

    uint64_t t2 = __rdtsc();
    for (int i = 0; i < iterations; i++) {
      fast_ring_append(&fast_r, wbuf, chunk);
      fast_ring_consume(&fast_r, rbuf, chunk);
    }
    uint64_t t3 = __rdtsc();
    uint64_t fast_cycles = t3 - t2;
    fast_ring_free(&fast_r);

    double old_per_op = (double)old_cycles / (iterations * 2);
    double fast_per_op = (double)fast_cycles / (iterations * 2);
    double speedup = (double)old_cycles / (double)fast_cycles;

    printf("      Old Modulo Ring Buffer: %8.2f cycles/op\n", old_per_op);
    printf("      New Fast Power-of-Two:  %8.2f cycles/op\n", fast_per_op);
    printf("      >>> SPEEDUP: %.2fX FASTER <<<\n", speedup);
  }

  free(wbuf);
  free(rbuf);
}

int main(void) {
  run_correctness_tests();
  run_benchmarks();
  return 0;
}
