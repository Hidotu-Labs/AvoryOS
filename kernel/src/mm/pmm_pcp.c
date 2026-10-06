#include "mm/pmm_internal.h"
#include "mm/numa.h"
#include "hal/hal.h"
#include "console/klog.h"
#include "smp/cpu.h"

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
