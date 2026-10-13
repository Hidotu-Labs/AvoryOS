/* DMA RAM must return to the PMM with a WB direct mapping. */
#include "mm/dma_alloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/tlb_shootdown.h"
#include "hal/hal.h"
#include "smp/cpu.h"
#include "console/klog.h"
#include <stdbool.h>
#include <stdint.h>

static bool dma_leaf_cache(uint64_t phys, uint64_t cache) {
  uint64_t va = phys + pmm_get_hhdm_offset();
  uint64_t table = (uint64_t)vmm_get_kernel_pml4();
  const unsigned shifts[] = {39, 30, 21, 12};
  for (unsigned level = 0; level < 4; level++) {
    uint64_t *entries = (void *)(table + pmm_get_hhdm_offset());
    uint64_t entry = entries[(va >> shifts[level]) & 511];
    if (!(entry & PAGE_FLAG_PRESENT))
      return false;
    if (level == 3)
      return (entry & (PAGE_FLAG_PAT | PAGE_FLAG_PCD | PAGE_FLAG_PWT)) == cache;
    if (level && (entry & PAGE_FLAG_PS))
      return ((entry & (PAGE_FLAG_PCD | PAGE_FLAG_PWT)) |
              ((entry & (1ULL << 12)) ? PAGE_FLAG_PAT : 0)) == cache;
    table = entry & PAGE_MASK;
  }
  return false;
}

void test_dma_cache(void) {
  bool ok = true;
  for (unsigned cycle = 0; cycle < 8; cycle++) {
    uint32_t flags = DMA_FLAG_32BIT | (cycle & 1 ? DMA_FLAG_NOCACHE : 0);
    tlb_shootdown_stats_t before, after;
    tlb_shootdown_get_stats(&before);
    dma_buffer_t *buf = dma_alloc(3 * PAGE_SIZE - 7, flags);
    if (!buf) { ok = false; break; }
    uint64_t phys = buf->phys;
    size_t pages = buf->pages;
    tlb_shootdown_get_stats(&after);
    if ((flags & DMA_FLAG_NOCACHE) && cpu_get_count() > 1)
      ok &= after.ipis_sent > before.ipis_sent;
    ok &= pages == 4;
    for (size_t i = 0; i < pages; i++)
      ok &= dma_leaf_cache(phys + i * PAGE_SIZE,
                          flags & DMA_FLAG_NOCACHE ? PAGE_FLAG_PCD | PAGE_FLAG_PWT : 0);
    dma_free(buf);
    /* Only inspect page tables; never touch the returned frame contents. */
    for (size_t i = 0; i < pages; i++)
      ok &= dma_leaf_cache(phys + i * PAGE_SIZE, 0);
  }
  uint64_t phys;
  void *page = dma_alloc_page(&phys);
  if (page) {
    ok &= dma_leaf_cache(phys, 0);
    dma_free_page(page);
    ok &= dma_leaf_cache(phys, 0);
  } else {
    ok = false;
  }
  dma_buffer_t *uncached = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT | DMA_FLAG_NOCACHE);
  if (uncached) {
    phys = uncached->phys;
    hal_irq_state_t irq = hal_irq_save();
    dma_free(uncached);
    ok &= pmm_get_ref((void *)phys) != 0; /* Held until remote invalidation. */
    hal_irq_restore(irq);
    vmm_drain_deferred_work();
    ok &= pmm_get_ref((void *)phys) == 0;
  } else {
    ok = false;
  }
  klog_puts(ok ? "[  OK  ] Native DMA: coherent WB, explicit UC and WB restoration\n" :
                "[ FAIL ] Native DMA: cache type did not follow ownership\n");
}
