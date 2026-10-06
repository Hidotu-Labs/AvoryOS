#ifndef PMM_INTERNAL_H
#define PMM_INTERNAL_H

#include "mm/pmm.h"
#include "lib/list.h"
#include "lock/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_ORDER 20

#define PCP_CAPACITY 128
#define PCP_BATCH 32

struct buddy_zone {
  struct list_head free_list[MAX_ORDER];
  spinlock_t lock;
};

struct buddy_block {
  struct list_head node;
  size_t order;
};

struct pcp_cache {
  void *pages[PCP_CAPACITY];
  uint32_t count;
  spinlock_t lock;
};

/* Global PMM shared state */
extern struct buddy_zone b_zone;
extern uint8_t *pmm_bitmap;
extern uint8_t *pmm_managed_bitmap;
extern uint16_t *pmm_refcounts;
extern size_t pmm_bitmap_size;
extern uint64_t pmm_lowest_page;
extern uint64_t pmm_highest_page;
extern uint64_t pmm_page_count;
extern uint64_t pmm_usable_memory;
extern uint64_t pmm_total_memory;
extern uint64_t pmm_physical_memory_offset;
extern struct limine_memmap_response *pmm_internal_memmap;
extern uint64_t pmm_zero_page_phys;

/* Bitmap and conversion inline helpers */
static inline void pmm_bitmap_set(uint8_t *bm, size_t bit) {
  bm[bit / 8] |= (1 << (bit % 8));
}

static inline void pmm_bitmap_clear(uint8_t *bm, size_t bit) {
  bm[bit / 8] &= ~(1 << (bit % 8));
}

static inline int pmm_bitmap_test(const uint8_t *bm, size_t bit) {
  return (bm[bit / 8] & (1 << (bit % 8))) != 0;
}

void pmm_bitmap_set_range(uint8_t *bm, size_t start_bit, size_t count);
void pmm_bitmap_clear_range(uint8_t *bm, size_t start_bit, size_t count);

static inline struct buddy_block *virt_to_buddy(uint64_t phys) {
  return (struct buddy_block *)(phys + pmm_physical_memory_offset);
}

static inline uint64_t buddy_to_phys(struct buddy_block *block) {
  return (uint64_t)block - pmm_physical_memory_offset;
}

static inline size_t pmm_get_order(size_t count) {
  size_t order = 0;
  size_t size = 1;
  while (size < count) {
    size *= 2;
    order++;
  }
  return order;
}

/* Buddy Allocator API */
void buddy_zone_init(struct buddy_zone *zone);
void buddy_free_internal(struct buddy_zone *zone, uint64_t phys, size_t order);
void *buddy_alloc_internal(struct buddy_zone *zone, size_t order);
void *buddy_alloc_page_locked(struct buddy_zone *zone);
void *buddy_alloc_pages_range(struct buddy_zone *zone, size_t count,
                             uint64_t min_phys_addr, uint64_t max_phys_addr);
size_t buddy_zone_free_pages(struct buddy_zone *zone);
bool buddy_node_valid(const struct list_head *node, size_t order);
void buddy_report_corruption(const char *what, size_t order,
                             const struct list_head *node);

/* Per-CPU Allocator API */
void *pmm_pcp_alloc_page(void);
bool pmm_pcp_free_page(void *ptr);
size_t pmm_pcp_total_cached(void);
void pmm_pcp_drain_local(void);
void pmm_pcp_drain_all(void);

#endif
