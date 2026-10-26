#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#define PID_MAX_LIMIT 65536
#define PID_MAX_DEFAULT 32768
#define PID_RESERVED 2

static uint64_t pid_bitmap[PID_MAX_LIMIT / 64];
static uint64_t pid_summary[PID_MAX_LIMIT / 64 / 64];
static uint64_t pid_top_summary = 0;
static uint32_t last_allocated_pid = 1;
static bool pid_bitmap_initialized = false;

struct test_thread {
  uint32_t tid;
  struct test_thread *global_next;
  struct test_thread *global_prev;
};

static struct test_thread *tid_map[PID_MAX_LIMIT];
static struct test_thread *global_thread_list = NULL;

static inline void pid_set_bit(uint32_t pid) {
  if (__builtin_expect(pid >= PID_MAX_LIMIT, 0)) return;
  uint32_t word = pid / 64;
  uint32_t bit = pid % 64;
  pid_bitmap[word] |= (1ULL << bit);
  if (__builtin_expect(pid_bitmap[word] == ~0ULL, 0)) {
    uint32_t sword = word / 64;
    uint32_t sbit = word % 64;
    pid_summary[sword] |= (1ULL << sbit);
    if (__builtin_expect(pid_summary[sword] == ~0ULL, 0)) {
      pid_top_summary |= (1ULL << sword);
    }
  }
}

static inline void pid_clear_bit(uint32_t pid) {
  if (__builtin_expect(pid >= PID_MAX_LIMIT, 0)) return;
  uint32_t word = pid / 64;
  uint32_t bit = pid % 64;
  pid_bitmap[word] &= ~(1ULL << bit);
  uint32_t sword = word / 64;
  uint32_t sbit = word % 64;
  pid_summary[sword] &= ~(1ULL << sbit);
  pid_top_summary &= ~(1ULL << sword);
}

static inline bool pid_is_allocated(uint32_t pid) {
  if (pid >= PID_MAX_LIMIT) return false;
  return (pid_bitmap[pid / 64] & (1ULL << (pid % 64))) != 0;
}

static void pid_bitmap_init(void) {
  memset(pid_bitmap, 0, sizeof(pid_bitmap));
  memset(pid_summary, 0, sizeof(pid_summary));
  pid_top_summary = 0;
  memset(tid_map, 0, sizeof(tid_map));

  // Mark PID 0 as reserved
  pid_set_bit(0);
  pid_bitmap_initialized = true;
}

static uint32_t sched_find_free_pid(uint32_t start) {
  const uint32_t max_pid = PID_MAX_DEFAULT;
  if (start >= max_pid)
    start = PID_RESERVED;

  uint32_t word = start / 64;
  uint32_t bit = start % 64;

  // 1. Current word from 'bit'
  uint64_t mask = (bit == 0) ? ~0ULL : ~((1ULL << bit) - 1);
  uint64_t w = pid_bitmap[word] | ~mask;
  if (w != ~0ULL) {
    uint32_t free_bit = __builtin_ctzll(~w);
    uint32_t pid = word * 64 + free_bit;
    if (pid < max_pid)
      return pid;
  }

  // 2. Scan remaining words in current summary block (up to 64 words)
  uint32_t sword = word / 64;
  uint32_t sbit = (word % 64) + 1;
  if (sbit < 64) {
    uint64_t smask = ~((1ULL << sbit) - 1);
    uint64_t sw = pid_summary[sword] | ~smask;
    if (sw != ~0ULL) {
      uint32_t free_sword_bit = __builtin_ctzll(~sw);
      uint32_t free_word_idx = sword * 64 + free_sword_bit;
      uint32_t free_bit = __builtin_ctzll(~pid_bitmap[free_word_idx]);
      uint32_t pid = free_word_idx * 64 + free_bit;
      if (pid < max_pid)
        return pid;
    }
  }

  // 3. Scan remaining summary blocks using top_summary
  uint32_t max_swords = max_pid / 4096;
  uint32_t top_bit = sword + 1;
  if (top_bit < max_swords) {
    uint64_t top_mask = ~((1ULL << top_bit) - 1);
    uint64_t tw = pid_top_summary | ~top_mask;
    if (tw != ~0ULL) {
      uint32_t free_sword = __builtin_ctzll(~tw);
      if (free_sword < max_swords) {
        uint32_t free_sword_bit = __builtin_ctzll(~pid_summary[free_sword]);
        uint32_t free_word = free_sword * 64 + free_sword_bit;
        uint32_t free_bit = __builtin_ctzll(~pid_bitmap[free_word]);
        uint32_t pid = free_word * 64 + free_bit;
        if (pid < max_pid)
          return pid;
      }
    }
  }

  // 4. Wrap around and search from PID_RESERVED
  if (start > PID_RESERVED) {
    start = PID_RESERVED;
    word = start / 64;
    bit = start % 64;

    mask = (bit == 0) ? ~0ULL : ~((1ULL << bit) - 1);
    w = pid_bitmap[word] | ~mask;
    if (w != ~0ULL) {
      uint32_t pid = word * 64 + __builtin_ctzll(~w);
      if (pid < max_pid)
        return pid;
    }

    sword = word / 64;
    sbit = (word % 64) + 1;
    if (sbit < 64) {
      uint64_t smask = ~((1ULL << sbit) - 1);
      uint64_t sw = pid_summary[sword] | ~smask;
      if (sw != ~0ULL) {
        uint32_t free_sword_bit = __builtin_ctzll(~sw);
        uint32_t free_word_idx = sword * 64 + free_sword_bit;
        uint32_t pid = free_word_idx * 64 + __builtin_ctzll(~pid_bitmap[free_word_idx]);
        if (pid < max_pid)
          return pid;
      }
    }

    top_bit = sword + 1;
    if (top_bit < max_swords) {
      uint64_t top_mask = ~((1ULL << top_bit) - 1);
      uint64_t tw = pid_top_summary | ~top_mask;
      if (tw != ~0ULL) {
        uint32_t free_sword = __builtin_ctzll(~tw);
        if (free_sword < max_swords) {
          uint32_t free_sword_bit = __builtin_ctzll(~pid_summary[free_sword]);
          uint32_t free_word = free_sword * 64 + free_sword_bit;
          uint32_t pid = free_word * 64 + __builtin_ctzll(~pid_bitmap[free_word]);
          if (pid < max_pid)
            return pid;
        }
      }
    }
  }

  return 0; // Exhausted
}

static uint32_t sched_alloc_tid_locked(void) {
  if (__builtin_expect(!pid_bitmap_initialized, 0)) {
    pid_bitmap_init();
  }

  if (!pid_is_allocated(1)) {
    pid_set_bit(1);
    last_allocated_pid = 1;
    return 1;
  }

  uint32_t pid = sched_find_free_pid(last_allocated_pid + 1);
  if (__builtin_expect(pid == 0, 0))
    return 0;

  pid_set_bit(pid);
  last_allocated_pid = pid;
  return pid;
}

static void sched_free_tid_locked(uint32_t tid) {
  if (tid < PID_RESERVED || tid >= PID_MAX_LIMIT)
    return;
  pid_clear_bit(tid);
  tid_map[tid] = NULL;
}

// Old linear lookup
static struct test_thread* old_find_thread(uint32_t tid) {
  struct test_thread *curr = global_thread_list;
  while (curr) {
    if (curr->tid == tid)
      return curr;
    curr = curr->global_next;
  }
  return NULL;
}

// New O(1) table lookup
static inline struct test_thread* new_find_thread(uint32_t tid) {
  if (__builtin_expect(tid < PID_MAX_LIMIT, 1)) {
    return tid_map[tid];
  }
  return old_find_thread(tid);
}

static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

// Correctness tests
static void test_correctness(void) {
  printf("[TEST 1] Testing PID allocation and recycling correctness... ");
  pid_bitmap_init();

  // PID 1 allocated first
  uint32_t p1 = sched_alloc_tid_locked();
  assert(p1 == 1);

  // Allocate 1000 PIDs
  uint32_t pids[1000];
  for (int i = 0; i < 1000; i++) {
    pids[i] = sched_alloc_tid_locked();
    assert(pids[i] == (uint32_t)(i + 2));
    assert(pid_is_allocated(pids[i]));
  }

  // Free some PIDs in the middle
  sched_free_tid_locked(pids[100]); // PID 102
  sched_free_tid_locked(pids[200]); // PID 202
  assert(!pid_is_allocated(pids[100]));
  assert(!pid_is_allocated(pids[200]));

  // Free all PIDs up to 1000
  for (int i = 0; i < 1000; i++) {
    sched_free_tid_locked(pids[i]);
  }

  // Next allocation should continue sequentially then wrap
  uint32_t next_p = sched_alloc_tid_locked();
  assert(next_p == 1002);
  sched_free_tid_locked(next_p);

  printf("PASS!\n");
}

static void test_lookup_and_unlink(void) {
  printf("[TEST 2] Testing O(1) direct TID table and doubly-linked unlinking... ");
  pid_bitmap_init();
  global_thread_list = NULL;

  const int COUNT = 100;
  struct test_thread threads[COUNT];

  for (int i = 0; i < COUNT; i++) {
    uint32_t tid = sched_alloc_tid_locked();
    threads[i].tid = tid;
    tid_map[tid] = &threads[i];

    // Doubly-linked insert
    threads[i].global_next = global_thread_list;
    threads[i].global_prev = NULL;
    if (global_thread_list) {
      global_thread_list->global_prev = &threads[i];
    }
    global_thread_list = &threads[i];
  }

  // Verify O(1) lookups match old linear lookups
  for (int i = 0; i < COUNT; i++) {
    assert(new_find_thread(threads[i].tid) == &threads[i]);
    assert(old_find_thread(threads[i].tid) == &threads[i]);
  }

  // Unlink middle element
  struct test_thread *victim = &threads[50];
  if (victim->global_prev) {
    victim->global_prev->global_next = victim->global_next;
  } else if (global_thread_list == victim) {
    global_thread_list = victim->global_next;
  }
  if (victim->global_next) {
    victim->global_next->global_prev = victim->global_prev;
  }
  sched_free_tid_locked(victim->tid);

  assert(new_find_thread(victim->tid) == NULL);
  assert(old_find_thread(victim->tid) == NULL);
  printf("PASS!\n");
}

// Benchmark
static void benchmark_pid_alloc_free(void) {
  printf("\n=== Microbenchmark 1: PID / TID Allocation & Free (1,000,000 ops) ===\n");
  pid_bitmap_init();

  const int ITERS = 1000000;
  uint64_t t0 = rdtsc();
  for (int i = 0; i < ITERS; i++) {
    uint32_t p = sched_alloc_tid_locked();
    sched_free_tid_locked(p);
  }
  uint64_t t1 = rdtsc();
  double new_cycles = (double)(t1 - t0) / ITERS;

  printf("New O(1) Hierarchical Bitmap (Alloc + Free): %.2f cycles / op\n", new_cycles);
}

static void benchmark_tid_lookup(void) {
  printf("\n=== Microbenchmark 2: TID Lookup with 200 Live Threads ===\n");
  pid_bitmap_init();
  global_thread_list = NULL;

  const int COUNT = 200;
  struct test_thread threads[COUNT];
  for (int i = 0; i < COUNT; i++) {
    uint32_t tid = sched_alloc_tid_locked();
    threads[i].tid = tid;
    tid_map[tid] = &threads[i];

    threads[i].global_next = global_thread_list;
    threads[i].global_prev = NULL;
    if (global_thread_list) {
      global_thread_list->global_prev = &threads[i];
    }
    global_thread_list = &threads[i];
  }

  const int ITERS = 2000000;

  // Measure Old (linear scan)
  volatile struct test_thread *sink_old = NULL;
  uint64_t t0 = rdtsc();
  for (int i = 0; i < ITERS; i++) {
    sink_old = old_find_thread(threads[150].tid);
    __asm__ volatile("" : : "r"(sink_old) : "memory");
  }
  uint64_t t1 = rdtsc();
  double old_cycles = (double)(t1 - t0) / ITERS;

  // Measure New (direct O(1) array index)
  volatile struct test_thread *sink_new = NULL;
  t0 = rdtsc();
  for (int i = 0; i < ITERS; i++) {
    sink_new = new_find_thread(threads[150].tid);
    __asm__ volatile("" : : "r"(sink_new) : "memory");
  }
  t1 = rdtsc();
  double new_cycles = (double)(t1 - t0) / ITERS;

  printf("Old Linear List Walk (200 threads): %.2f cycles / lookup\n", old_cycles);
  printf("New O(1) Direct Table Index:        %.2f cycles / lookup\n", new_cycles);
  printf("Speedup: \033[1;32m%.2fX FASTER\033[0m\n", old_cycles / new_cycles);
}

int main(void) {
  test_correctness();
  test_lookup_and_unlink();
  benchmark_pid_alloc_free();
  benchmark_tid_lookup();
  return 0;
}
