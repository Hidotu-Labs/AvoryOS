#include "mm/pmm.h"
#include "mm/pmm_internal.h"
#include "mm/numa.h"
#include "console/klog.h"
#include "hal/hal.h"
#include "lib/string.h"
#include "smp/cpu.h"

void pmm_bitmap_set_range(uint8_t *bm, size_t start_bit, size_t count) {
  if (start_bit < pmm_lowest_page)
    return;
  if (start_bit >= pmm_highest_page)
    return;
  if (start_bit + count > pmm_highest_page)
    count = pmm_highest_page - start_bit;

  size_t idx = start_bit - pmm_lowest_page;
  size_t end_idx = idx + count;

  while (idx < end_idx && (idx % 8) != 0) {
    pmm_bitmap_set(bm, idx);
    idx++;
  }

  if (end_idx > idx) {
    size_t full_bytes = (end_idx - idx) / 8;
    if (full_bytes > 0) {
      memset(&bm[idx / 8], 0xFF, full_bytes);
      idx += full_bytes * 8;
    }
  }

  while (idx < end_idx) {
    pmm_bitmap_set(bm, idx);
    idx++;
  }
}

void pmm_bitmap_clear_range(uint8_t *bm, size_t start_bit, size_t count) {
  if (start_bit < pmm_lowest_page)
    return;
  if (start_bit >= pmm_highest_page)
    return;
  if (start_bit + count > pmm_highest_page)
    count = pmm_highest_page - start_bit;

  size_t idx = start_bit - pmm_lowest_page;
  size_t end_idx = idx + count;

  while (idx < end_idx && (idx % 8) != 0) {
    pmm_bitmap_clear(bm, idx);
    idx++;
  }

  if (end_idx > idx) {
    size_t full_bytes = (end_idx - idx) / 8;
    if (full_bytes > 0) {
      memset(&bm[idx / 8], 0, full_bytes);
      idx += full_bytes * 8;
    }
  }

  while (idx < end_idx) {
    pmm_bitmap_clear(bm, idx);
    idx++;
  }
}

uint64_t pmm_get_zero_page_phys(void) {
  return pmm_zero_page_phys;
}

uint64_t pmm_get_usable_memory(void) { return pmm_usable_memory; }
uint64_t pmm_get_total_memory(void) { return pmm_total_memory; }
uint64_t pmm_get_hhdm_offset(void) { return pmm_physical_memory_offset; }

size_t pmm_get_free_pages(void) {
  size_t total = 0;
  uint32_t count = numa_node_count();
  for (uint32_t i = 0; i < count; i++) {
    struct numa_node *n = numa_get_node(i);
    if (n->online) {
      total += buddy_zone_free_pages(&n->zone);
    }
  }
  return total + pmm_pcp_total_cached();
}

size_t pmm_get_free_pages_including_pcp(void) {
  return pmm_get_free_pages();
}

bool pmm_is_managed(uint64_t phys) {
  uint64_t pfn = phys / PAGE_SIZE;
  if (pfn < pmm_lowest_page || pfn >= pmm_highest_page || pmm_managed_bitmap == NULL)
    return false;
  return pmm_bitmap_test(pmm_managed_bitmap, pfn - pmm_lowest_page);
}

bool pmm_kernel_ptr_is_managed(const void *ptr) {
  uint64_t v = (uint64_t)ptr;
  if (!v || v < pmm_physical_memory_offset)
    return false;
  return pmm_is_managed(v - pmm_physical_memory_offset);
}

void pmm_incref(void *ptr) {
  if (!ptr || (uint64_t)ptr == pmm_zero_page_phys)
    return;
  if (!pmm_is_managed((uint64_t)ptr))
    return;
  uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;

  __atomic_fetch_add(&pmm_refcounts[pfn - pmm_lowest_page], 1, __ATOMIC_RELAXED);
}

__attribute__((optimize("O3"))) void pmm_decref(void *ptr) {
  if (!ptr || (uint64_t)ptr == pmm_zero_page_phys)
    return;
  if (!pmm_is_managed((uint64_t)ptr))
    return;
  uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;

  uint16_t old = __atomic_fetch_sub(&pmm_refcounts[pfn - pmm_lowest_page], 1, __ATOMIC_ACQ_REL);
  if (old > 1) {
    return;
  }
  if (old == 0) {
    __atomic_store_n(&pmm_refcounts[pfn - pmm_lowest_page], 0, __ATOMIC_RELAXED);
    return;
  }

  if (!pmm_bitmap_test(pmm_bitmap, pfn - pmm_lowest_page)) {
    return; // already on the buddy free list
  }

  if (pmm_pcp_free_page(ptr))
    return;

  int node = numa_phys_to_node((uint64_t)ptr);
  struct numa_node *n = numa_get_node(node);

  spinlock_acquire(&n->zone.lock);
  buddy_free_internal(&n->zone, (uint64_t)ptr, 0);
  spinlock_release(&n->zone.lock);
}

uint16_t pmm_get_ref(void *ptr) {
  if (!ptr)
    return 0;
  if (!pmm_is_managed((uint64_t)ptr))
    return 1;
  uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;
  return __atomic_load_n(&pmm_refcounts[pfn - pmm_lowest_page], __ATOMIC_RELAXED);
}

static volatile uint64_t buddy_mark_used_skips;

void pmm_mark_used(void *ptr, size_t count) {
  if (count == 0 || !ptr)
    return;

  uint64_t addr = (uint64_t)ptr;
  if (addr == pmm_zero_page_phys)
    return;

  size_t start_bit = addr / PAGE_SIZE;

  for (size_t i = 0; i < count; i++) {
    size_t pfn = start_bit + i;
    if (pmm_managed_bitmap && pfn >= pmm_lowest_page && pfn < pmm_highest_page &&
        pmm_bitmap_test(pmm_managed_bitmap, pfn - pmm_lowest_page) &&
        !pmm_bitmap_test(pmm_bitmap, pfn - pmm_lowest_page)) {
      uint64_t n =
          __atomic_add_fetch(&buddy_mark_used_skips, 1, __ATOMIC_RELAXED);
      if (n <= 8) {
        klog_puts(KLOG_CLR_RED
                  "[PMM] WARN: pmm_mark_used on a free buddy page " KLOG_CLR_RESET);
        klog_hex64((uint64_t)pfn * PAGE_SIZE);
        klog_puts(" - ignored\n");
      }
      continue;
    }
    pmm_bitmap_set_range(pmm_bitmap, pfn, 1);
  }
}

__attribute__((optimize("O3"))) void *pmm_alloc_pages(size_t count) {
  if (count == 0)
    return NULL;

  hal_irq_state_t flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  int node = (cpu && cpu->cpu_id < MAX_CPUS) ? numa_cpu_to_node(cpu->cpu_id) : 0;
  hal_irq_restore(flags);

  return pmm_alloc_pages_node(count, node);
}

void *pmm_alloc_pages_constrained(size_t count, uint64_t max_phys_addr) {
  return pmm_alloc_pages_range(count, 0, max_phys_addr);
}

void *pmm_alloc_pages_range(size_t count, uint64_t min_phys_addr,
                            uint64_t max_phys_addr) {
  hal_irq_state_t flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  int node = (cpu && cpu->cpu_id < MAX_CPUS) ? numa_cpu_to_node(cpu->cpu_id) : 0;
  hal_irq_restore(flags);

  struct numa_node *n = numa_get_node(node);
  void *res = buddy_alloc_pages_range(&n->zone, count, min_phys_addr, max_phys_addr);
  if (res)
    return res;

  // Try fallback nodes if local node cannot satisfy range
  for (int fb = 0; fb < MAX_NUMA_NODES; fb++) {
    int alt_node = n->fallback_order[fb];
    if (alt_node == NUMA_NO_NODE || alt_node == node)
      continue;
    struct numa_node *an = numa_get_node(alt_node);
    if (!an->online)
      continue;
    res = buddy_alloc_pages_range(&an->zone, count, min_phys_addr, max_phys_addr);
    if (res)
      return res;
  }

  return NULL;
}

__attribute__((optimize("O3"))) void *pmm_alloc_page(void) {
  void *page = pmm_pcp_alloc_page();
  if (page)
    return page;
  return pmm_alloc_pages(1);
}

__attribute__((optimize("O3"))) void *pmm_alloc_huge_page(void) {
  void *phys = pmm_alloc_pages(512);
  if (!phys)
    return NULL;
  if ((uint64_t)phys & 0x1FFFFFULL) {
    pmm_free_pages(phys, 512);
    return NULL;
  }
  return phys;
}

__attribute__((optimize("O3"))) void pmm_free_pages(void *ptr, size_t count) {
  if (!ptr || count == 0 || (uint64_t)ptr == pmm_zero_page_phys)
    return;

  size_t order = pmm_get_order(count);
  if (order >= MAX_ORDER)
    return;

  uint64_t addr = (uint64_t)ptr;
  uint64_t pfn = addr / PAGE_SIZE;

  if (pfn < pmm_lowest_page || pfn >= pmm_highest_page ||
      !pmm_bitmap_test(pmm_managed_bitmap, pfn - pmm_lowest_page)) {
    return;
  }

  if (!pmm_bitmap_test(pmm_bitmap, pfn - pmm_lowest_page)) {
    return;
  }

  int node = numa_phys_to_node(addr);
  struct numa_node *n = numa_get_node(node);

  spinlock_acquire(&n->zone.lock);

  size_t num_pages = 1ULL << order;
  bool all_last = true;
  bool abort_free = false;
  for (size_t i = 0; i < num_pages; i++) {
    // Atomic claim races safely with lock-free pmm_decref()/pmm_incref():
    // exactly one side observes old==1 per page. A load+store here would let
    // both sides see ref==1 and free the same page (buddy double-free during
    // mass exit with shared CoW pages).
    uint16_t old = __atomic_fetch_sub(&pmm_refcounts[pfn + i - pmm_lowest_page], 1,
                                      __ATOMIC_ACQ_REL);
    if (old == 0) {
      // Already free: undo underflow and abort the whole block free. Earlier
      // pages in this block were legitimately decremented (shared puts).
      __atomic_store_n(&pmm_refcounts[pfn + i - pmm_lowest_page], 0, __ATOMIC_RELAXED);
      all_last = false;
      abort_free = true;
      break;
    }
    if (old > 1)
      all_last = false;
  }

  if (abort_free) {
    spinlock_release(&n->zone.lock);
    return;
  }

  if (all_last) {
    buddy_free_internal(&n->zone, addr, order);
  }

  spinlock_release(&n->zone.lock);
}

void pmm_free_page(void *ptr) { pmm_decref(ptr); }
