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
#define LOCAL_CACHE_CAPACITY 16

struct cpu_local_slab {
  uint16_t count;
  void *entries[LOCAL_CACHE_CAPACITY];
};

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
extern struct cpu_local_slab heap_cpu_slab_caches[MAX_CPUS][CACHE_COUNT];
extern struct big_alloc *heap_big_alloc_head;

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
