#ifndef AVORY_ZRAM_H
#define AVORY_ZRAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct mm_struct;
struct zram_stats {
  uint64_t capacity_pages, stored_pages, compressed_bytes, memory_bytes,
      memory_limit;
  uint64_t same_pages, peak_bytes, rejected_pages, allocation_failures;
  uint64_t swapouts, swapins, scans, pressure_yields;
  bool enabled;
};
void zram_init(void);
bool zram_is_enabled(void);
void zram_get_stats(struct zram_stats *out);
/* Immutable compressed objects. Each PTE and in-flight reader owns a ref. */
bool zram_store_init(uint32_t capacity, uint64_t limit);
uint32_t zram_store_page(const void *page);
bool zram_slot_get(uint32_t slot);
void zram_slot_put(uint32_t slot);
int zram_slot_read(uint32_t slot, void *page);
void zram_store_stats(struct zram_stats *out);
bool zram_under_pressure(void);
void zram_balance(void);
size_t zram_reclaim(size_t target);
int zram_fault(struct mm_struct *mm, uint64_t cr3, uint64_t va, bool write,
               bool exec);
size_t zram_pageout(struct mm_struct *mm, uint64_t cr3, uint64_t start,
                    uint64_t end);
/* Caller holds mm->lock; used by mremap's copy while old VMA is stable. */
int zram_copy_swapped(uint64_t *pml4, uint64_t va, void *page);
#endif
