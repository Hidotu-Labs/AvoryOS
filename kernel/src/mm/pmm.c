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
uint64_t pmm_get_managed_memory_end(void) { return pmm_highest_page * PAGE_SIZE; }
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
  /* Pooled frames (PCP + pre-zeroed) are out of the buddy lists but
   * reclaimable via drain, so they count as free — same rationale as PCP. */
  return total + pmm_pcp_total_cached() + pmm_zero_pool_total_cached();
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

static volatile uint64_t pmm_ref_errors;

static void pmm_report_ref_error(const char *operation, uint64_t phys,
                                 uint16_t ref, void *caller) {
  if (__atomic_add_fetch(&pmm_ref_errors, 1, __ATOMIC_RELAXED) > 16)
    return;
  klog_puts("[PMM] REF ERROR: ");
  klog_puts(operation);
  klog_puts(" phys="); klog_hex64(phys);
  klog_puts(" ref="); klog_uint64(ref);
  klog_puts(" caller="); klog_hex64((uint64_t)caller);
  klog_puts(" allocated=");
  klog_uint64(pmm_bitmap_test(pmm_bitmap, phys / PAGE_SIZE - pmm_lowest_page));
  klog_puts("\n");
}

/* Never publish an underflow value: another CPU can allocate a PCP page
 * between a fetch_sub(0) and the attempted store(0) repair. */
static bool pmm_put_ref(uint64_t phys, void *caller) {
  uint16_t *ref = &pmm_refcounts[phys / PAGE_SIZE - pmm_lowest_page];
  uint16_t old = __atomic_load_n(ref, __ATOMIC_ACQUIRE);
  for (;;) {
    if (old == 0) {
      pmm_report_ref_error("release of zero reference", phys, old, caller);
      return false;
    }
    if (old == UINT16_MAX)
      return false; /* permanently pinned */
    if (__atomic_compare_exchange_n(ref, &old, old - 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return old == 1;
  }
}

void pmm_incref(void *ptr) {
  if (!ptr || (uint64_t)ptr == pmm_zero_page_phys)
    return;
  if (!pmm_is_managed((uint64_t)ptr))
    return;
  if ((uint64_t)ptr & (PAGE_SIZE - 1))
    return;
  uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;

  uint16_t *ref = &pmm_refcounts[pfn - pmm_lowest_page];
  uint16_t old = __atomic_load_n(ref, __ATOMIC_ACQUIRE);
  for (;;) {
    if (old == UINT16_MAX)
      return;
    if (old == 0 || old == UINT16_MAX - 1) {
      pmm_report_ref_error("invalid retain", (uint64_t)ptr, old,
                           __builtin_return_address(0));
      return;
    }
    if (__atomic_compare_exchange_n(ref, &old, old + 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return;
  }
}

__attribute__((optimize("O3"))) void pmm_decref(void *ptr) {
  if (!ptr || (uint64_t)ptr == pmm_zero_page_phys)
    return;
  if (!pmm_is_managed((uint64_t)ptr))
    return;
  if ((uint64_t)ptr & (PAGE_SIZE - 1))
    return;
  uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;

  if (!pmm_put_ref((uint64_t)ptr, __builtin_return_address(0)))
    return;

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
  size_t num_pages = 1ULL << order;

  if (pfn < pmm_lowest_page || pfn >= pmm_highest_page ||
      num_pages > pmm_highest_page - pfn ||
      (addr & (num_pages * PAGE_SIZE - 1))) {
    return;
  }
  /* Validate the entire rounded allocation before touching any references. */
  for (size_t i = 0; i < num_pages; i++)
    if (!pmm_is_managed(addr + i * PAGE_SIZE))
      return;

  int node = numa_phys_to_node(addr);
  struct numa_node *n = numa_get_node(node);

  for (size_t i = 0; i < num_pages; i++)
    if (numa_phys_to_node(addr + i * PAGE_SIZE) != node)
      return;

  spinlock_acquire(&n->zone.lock);

  for (size_t i = 0; i < num_pages; i++) {
    uint64_t phys = addr + i * PAGE_SIZE;
    /* Mixed/shared blocks must release every last-reference page, even if
     * another page remains shared or was already released. Buddy coalescing
     * rebuilds the larger block when its final constituent becomes free. */
    if (pmm_put_ref(phys, __builtin_return_address(0)))
      buddy_free_internal(&n->zone, phys, 0);
  }

  spinlock_release(&n->zone.lock);
}

void pmm_free_page(void *ptr) { pmm_decref(ptr); }
