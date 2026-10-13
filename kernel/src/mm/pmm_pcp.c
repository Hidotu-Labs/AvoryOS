#include "mm/pmm_internal.h"
#include "mm/numa.h"
#include "mm/page_ops.h"
#include "hal/hal.h"
#include "console/klog.h"
#include "smp/cpu.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

static struct pcp_cache pcp_caches[MAX_CPUS];
static bool pcp_initialized = false;

void pmm_pcp_init(void) {
  for (uint32_t i = 0; i < MAX_CPUS; i++) {
    pcp_caches[i].count = 0;
    spinlock_init(&pcp_caches[i].lock);
  }
  pcp_initialized = true;
  klog_puts("[PMM] Per-CPU Page Frame Allocator (PCP) initialized.\n");
}

size_t pmm_pcp_total_cached(void) {
  size_t total = 0;
  for (uint32_t i = 0; i < MAX_CPUS; i++) {
    total += __atomic_load_n(&pcp_caches[i].count, __ATOMIC_RELAXED);
  }
  return total;
}

__attribute__((optimize("O3"))) void *pmm_pcp_alloc_page(void) {
  if (!pcp_initialized)
    return NULL;

  hal_irq_state_t flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || cpu->cpu_id >= MAX_CPUS) {
    hal_irq_restore(flags);
    return NULL;
  }

  struct pcp_cache *pcp = &pcp_caches[cpu->cpu_id];
  spinlock_acquire(&pcp->lock);
  if (pcp->count > 0) {
    void *page = pcp->pages[--pcp->count];
    uint64_t pfn = (uint64_t)page / PAGE_SIZE;
    __atomic_store_n(&pmm_refcounts[pfn - pmm_lowest_page], 1, __ATOMIC_RELEASE);
    spinlock_release(&pcp->lock);
    hal_irq_restore(flags);
    return page;
  }

  int node = numa_cpu_to_node(cpu->cpu_id);
  struct numa_node *n = numa_get_node(node);

  // Refill batch from local node's buddy zone
  spinlock_acquire(&n->zone.lock);
  for (uint32_t i = 0; i < PCP_BATCH; i++) {
    void *p = buddy_alloc_page_locked(&n->zone);
    if (!p)
      break;
    /* Cached frames have no external owner. Restore one reference only
     * when a frame is actually handed to the caller. */
    __atomic_store_n(&pmm_refcounts[(uint64_t)p / PAGE_SIZE - pmm_lowest_page],
                      0, __ATOMIC_RELEASE);
    pcp->pages[pcp->count++] = p;
  }
  spinlock_release(&n->zone.lock);

  // If local node was empty, try fallback nodes
  if (pcp->count == 0) {
    for (int fb = 0; fb < MAX_NUMA_NODES; fb++) {
      int alt_node = n->fallback_order[fb];
      if (alt_node == NUMA_NO_NODE || alt_node == node)
        continue;
      struct numa_node *an = numa_get_node(alt_node);
      if (!an->online)
        continue;

      spinlock_acquire(&an->zone.lock);
      for (uint32_t i = 0; i < PCP_BATCH; i++) {
        void *p = buddy_alloc_page_locked(&an->zone);
        if (!p)
          break;
        __atomic_store_n(&pmm_refcounts[(uint64_t)p / PAGE_SIZE - pmm_lowest_page],
                          0, __ATOMIC_RELEASE);
        pcp->pages[pcp->count++] = p;
      }
      spinlock_release(&an->zone.lock);
      if (pcp->count > 0)
        break;
    }
  }

  void *page = NULL;
  if (pcp->count > 0) {
    page = pcp->pages[--pcp->count];
    uint64_t pfn = (uint64_t)page / PAGE_SIZE;
    __atomic_store_n(&pmm_refcounts[pfn - pmm_lowest_page], 1, __ATOMIC_RELEASE);
  }
  spinlock_release(&pcp->lock);

  // Steal from another CPU's cache if local and buddy are empty
  if (!page) {
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
      if (i == cpu->cpu_id)
        continue;
      struct pcp_cache *other = &pcp_caches[i];
      spinlock_acquire(&other->lock);
      if (other->count > 0) {
        page = other->pages[--other->count];
        uint64_t pfn = (uint64_t)page / PAGE_SIZE;
        __atomic_store_n(&pmm_refcounts[pfn - pmm_lowest_page], 1, __ATOMIC_RELEASE);
      }
      spinlock_release(&other->lock);
      if (page)
        break;
    }
  }
  hal_irq_restore(flags);
  return page;
}

__attribute__((optimize("O3"))) bool pmm_pcp_free_page(void *ptr) {
  if (!pcp_initialized || !ptr)
    return false;

  hal_irq_state_t flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || cpu->cpu_id >= MAX_CPUS) {
    hal_irq_restore(flags);
    return false;
  }

  struct pcp_cache *pcp = &pcp_caches[cpu->cpu_id];
  spinlock_acquire(&pcp->lock);
  if (pcp->count < PCP_CAPACITY) {
    pcp->pages[pcp->count++] = ptr;
    spinlock_release(&pcp->lock);
    hal_irq_restore(flags);
    return true;
  }

  /* Cache is full: pop PCP_BATCH pages into a local array to drain.
   * Keep 'ptr' in the local cache as a hot page. */
  void *drain_batch[PCP_BATCH];
  for (uint32_t i = 0; i < PCP_BATCH; i++) {
    drain_batch[i] = pcp->pages[--pcp->count];
  }
  pcp->pages[pcp->count++] = ptr;
  spinlock_release(&pcp->lock);
  hal_irq_restore(flags);

  /* Drain popped pages to their respective NUMA buddy zones without holding pcp->lock */
  for (uint32_t i = 0; i < PCP_BATCH; i++) {
    void *drain_p = drain_batch[i];
    int drain_node = numa_phys_to_node((uint64_t)drain_p);
    struct numa_node *dn = numa_get_node(drain_node);
    spinlock_acquire(&dn->zone.lock);
    buddy_free_internal(&dn->zone, (uint64_t)drain_p, 0);
    spinlock_release(&dn->zone.lock);
  }

  return true;
}

void pmm_pcp_drain_local(void) {
  if (!pcp_initialized)
    return;

  hal_irq_state_t flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || cpu->cpu_id >= MAX_CPUS) {
    hal_irq_restore(flags);
    return;
  }

  struct pcp_cache *pcp = &pcp_caches[cpu->cpu_id];
  spinlock_acquire(&pcp->lock);
  uint32_t count = pcp->count;
  void *drain_buf[PCP_CAPACITY];
  for (uint32_t i = 0; i < count; i++) {
    drain_buf[i] = pcp->pages[i];
  }
  pcp->count = 0;
  spinlock_release(&pcp->lock);
  hal_irq_restore(flags);

  for (uint32_t i = 0; i < count; i++) {
    int drain_node = numa_phys_to_node((uint64_t)drain_buf[i]);
    struct numa_node *dn = numa_get_node(drain_node);
    spinlock_acquire(&dn->zone.lock);
    buddy_free_internal(&dn->zone, (uint64_t)drain_buf[i], 0);
    spinlock_release(&dn->zone.lock);
  }
}

void pmm_pcp_drain_all(void) {
  if (!pcp_initialized)
    return;

  pmm_zero_pool_drain_all();

  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    struct pcp_cache *pcp = &pcp_caches[c];
    spinlock_acquire(&pcp->lock);
    uint32_t count = pcp->count;
    void *drain_buf[PCP_CAPACITY];
    for (uint32_t i = 0; i < count; i++) {
      drain_buf[i] = pcp->pages[i];
    }
    pcp->count = 0;
    spinlock_release(&pcp->lock);

    for (uint32_t i = 0; i < count; i++) {
      int drain_node = numa_phys_to_node((uint64_t)drain_buf[i]);
      struct numa_node *dn = numa_get_node(drain_node);
      spinlock_acquire(&dn->zone.lock);
      buddy_free_internal(&dn->zone, (uint64_t)drain_buf[i], 0);
      spinlock_release(&dn->zone.lock);
    }
  }
}

/* ---- Pre-zeroed page pool -------------------------------------------------
 *
 * Anonymous faults, BSS, CoW-zero breaks and exec-stack setup all need a
 * freshly zeroed frame (~1-2 us of memset on the fault path).  This pool
 * moves the zeroing to idle CPUs: the idle loops refill it in the background
 * and fault paths pop an already-zeroed frame with no memset.
 *
 * Ownership matches the PCP convention: pooled frames are out of the buddy
 * free lists with refcount 0 (owned by the pool); pop sets refcount 1.  The
 * per-CPU pcp lock guards the local pool (short sections only).  Allocation
 * and zeroing in refill happen outside the lock.  page_zero is the GP-reg
 * non-temporal engine, safe in any context (see page_ops_init).
 *
 * Bounds: ZERO_POOL_CAP frames per CPU are hidden from the buddy allocator,
 * so every OOM path must be able to reclaim them — pmm_zero_pool_drain_all()
 * (also called from pmm_pcp_drain_all above) returns them to PCP/buddy, and
 * pmm_get_free_pages() counts them as free.  Consumers must always fall back
 * to alloc+zero inline when the pool is empty, and must only use pooled pages
 * where a zeroed frame is actually wanted (never for pages that will be fully
 * overwritten, e.g. CoW copies) and where no NUMA bind policy applies.
 */
#define ZERO_POOL_CAP 32
#define ZERO_POOL_REFILL 8

static void *zero_pool[MAX_CPUS][ZERO_POOL_CAP];
static uint32_t zero_count[MAX_CPUS];
/* Observability: hits+misses say whether the pool is warm when faults need
 * it; refilled counts idle-produced pages. Reported in fork_probe_maybe_dump
 * next to the TSC table so boot-vs-steady-state hit rate is visible. */
static uint64_t zero_hits;
static uint64_t zero_misses;
static uint64_t zero_refilled;

void pmm_zero_pool_stats(uint64_t *hits, uint64_t *misses,
                         uint64_t *refilled, size_t *cached) {
  if (hits)
    *hits = __atomic_load_n(&zero_hits, __ATOMIC_RELAXED);
  if (misses)
    *misses = __atomic_load_n(&zero_misses, __ATOMIC_RELAXED);
  if (refilled)
    *refilled = __atomic_load_n(&zero_refilled, __ATOMIC_RELAXED);
  if (cached)
    *cached = pmm_zero_pool_total_cached();
}

void *pmm_alloc_zeroed_page(void) {
  hal_irq_state_t flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || cpu->cpu_id >= MAX_CPUS) {
    hal_irq_restore(flags);
    return NULL;
  }

  void *page = NULL;
  struct pcp_cache *pcp = &pcp_caches[cpu->cpu_id];
  spinlock_acquire(&pcp->lock);
  if (zero_count[cpu->cpu_id] > 0) {
    page = zero_pool[cpu->cpu_id][--zero_count[cpu->cpu_id]];
    uint64_t pfn = (uint64_t)page / PAGE_SIZE;
    __atomic_store_n(&pmm_refcounts[pfn - pmm_lowest_page], 1,
                     __ATOMIC_RELEASE);
  }
  spinlock_release(&pcp->lock);
  hal_irq_restore(flags);
  if (page)
    __atomic_add_fetch(&zero_hits, 1, __ATOMIC_RELAXED);
  else
    __atomic_add_fetch(&zero_misses, 1, __ATOMIC_RELAXED);
  return page;
}

bool pmm_zero_pool_needs_refill(void) {
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || cpu->cpu_id >= MAX_CPUS)
    return false;
  return __atomic_load_n(&zero_count[cpu->cpu_id], __ATOMIC_RELAXED) <
         ZERO_POOL_CAP;
}

size_t pmm_zero_pool_total_cached(void) {
  size_t total = 0;
  for (uint32_t i = 0; i < MAX_CPUS; i++)
    total += __atomic_load_n(&zero_count[i], __ATOMIC_RELAXED);
  return total;
}

void pmm_zero_pool_refill(uint32_t max_pages) {
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || cpu->cpu_id >= MAX_CPUS)
    return;
  uint32_t id = cpu->cpu_id;

  for (uint32_t i = 0; i < max_pages; i++) {
    void *p = pmm_pcp_alloc_page();
    if (!p)
      return;
    page_zero(PHYS_TO_VIRT((uint64_t)p));
    /* Park refcount-0 in the pool, same convention as PCP cached frames. */
    __atomic_store_n(&pmm_refcounts[(uint64_t)p / PAGE_SIZE - pmm_lowest_page],
                     0, __ATOMIC_RELEASE);
    hal_irq_state_t flags = hal_irq_save();
    spinlock_acquire(&pcp_caches[id].lock);
    bool full = (zero_count[id] >= ZERO_POOL_CAP);
    if (!full) {
      zero_pool[id][zero_count[id]++] = p;
      __atomic_add_fetch(&zero_refilled, 1, __ATOMIC_RELAXED);
    }
    spinlock_release(&pcp_caches[id].lock);
    hal_irq_restore(flags);
    if (full) {
      /* Lost the race with another refiller: hand it straight back. */
      pmm_pcp_free_page(p);
      return;
    }
  }
}

void pmm_zero_pool_drain_all(void) {
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    void *drain_buf[ZERO_POOL_CAP];
    hal_irq_state_t flags = hal_irq_save();
    spinlock_acquire(&pcp_caches[c].lock);
    uint32_t count = zero_count[c] <= ZERO_POOL_CAP ? zero_count[c] : 0;
    for (uint32_t i = 0; i < count; i++)
      drain_buf[i] = zero_pool[c][i];
    zero_count[c] = 0;
    spinlock_release(&pcp_caches[c].lock);
    hal_irq_restore(flags);

    for (uint32_t i = 0; i < count; i++)
      pmm_pcp_free_page(drain_buf[i]);
  }
}
