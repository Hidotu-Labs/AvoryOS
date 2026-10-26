#ifndef HEAP_INTERNAL_H
#define HEAP_INTERNAL_H

#include "mm/heap.h"
#include "lock/spinlock.h"
#include "smp/cpu.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLAB_MAGIC 0x51ABCAFECAFE51ABULL
#define BIG_MAGIC  0xB16A110CB16A110CULL

#define BITMAP_SET(bmp, index)   ((bmp)[(index) / 32] |= (1U << ((index) % 32)))
#define BITMAP_CLEAR(bmp, index) ((bmp)[(index) / 32] &= ~(1U << ((index) % 32)))
#define BITMAP_TEST(bmp, index)  (((bmp)[(index) / 32] & (1U << ((index) % 32))) != 0)

struct slab_cache;

struct slab {
  uint64_t magic;
  struct slab *next;
  struct slab *prev;
  uint32_t free_count;
  uint32_t total_count;
  struct slab_cache *cache;
  uint32_t bitmap[4]; // 128 bits
} __attribute__((aligned(64)));

struct slab_cache {
  size_t obj_size;
  spinlock_t lock;      // Fine-grained lock per size class
  struct slab *partial;
  struct slab *full;
  struct slab *free;
};

#define CACHE_COUNT 6

struct big_alloc {
  uint64_t magic;
  size_t pages;
  struct big_alloc *next;
  struct big_alloc *prev;
  uint64_t padding[4];
} __attribute__((aligned(64)));

extern bool heap_initialized;
extern spinlock_t heap_vmem_lock;
extern struct slab_cache heap_caches[CACHE_COUNT];
extern struct big_alloc *heap_big_alloc_head;

#define HEAP_PCPU_CLASSES  4  /* 32, 64, 128, 256 bytes */
#define HEAP_PCPU_CAPACITY 8  /* Objects per per-CPU magazine */
#define HEAP_PCPU_BATCH    4  /* Objects moved to/from slab on refill/drain */

struct heap_pcpu_cache {
  void *entries[HEAP_PCPU_CAPACITY];
  uint32_t count;
  volatile uint32_t active;
  spinlock_t lock;
} __attribute__((aligned(64)));

extern struct heap_pcpu_cache heap_pcpu_caches[MAX_CPUS][HEAP_PCPU_CLASSES];
void heap_pcpu_flush_all(void);
void heap_pcpu_flush_cpu(uint32_t cpu_id);

/* Opt-in per-size-class object diagnostics for self-tests.  Every hook is
 * guarded by one branch over a cold volatile, so the default allocation
 * paths keep their exact shape; the counters are diagnostic slop only. */
struct heap_slab_dbg {
  unsigned long alloc_pcpu; /* fast-path per-CPU freeslot pops */
  unsigned long alloc_slab; /* slow-path served by partial/free slabs */
  unsigned long alloc_new;  /* slow-path backed by a fresh PMM page */
  unsigned long free_park;  /* parked into the per-CPU cache */
  unsigned long free_slab;  /* delivered to the slab lists */
  unsigned long new_slabs;  /* fresh slab pages taken from PMM */
};
extern volatile int heap_slab_dbg_on;
void heap_slab_dbg_reset(void);
void heap_slab_dbg_dump(const char *tag);

/* Trace & debugging */
void heap_trace_record(void *ptr, void *caller);
void heap_trace_warn_double(const char *where, void *ptr);

/* Slab subsystem */
void heap_slab_init(void);
void *heap_slab_alloc(size_t size);
bool heap_slab_free(void *ptr, uint64_t page_base);
size_t heap_slab_ksize(const void *ptr, uint64_t page_base);
void heap_slab_get_info(char *buf);
void heap_oom_reclaim(void);

/* Big allocator subsystem */
void heap_big_init(void);
void *heap_big_alloc(size_t size);
bool heap_big_free(void *ptr, uint64_t page_base);
size_t heap_big_ksize(const void *ptr, uint64_t page_base);
void heap_big_get_info(char *buf);

#endif
