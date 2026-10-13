#include "dma_alloc.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../hal/hal.h"
#include "pmm.h"
#include "vmm.h"
#include "tlb_shootdown.h"
#include <stdbool.h>

static const char *dma_last_failure = "none";
static uint32_t dma_last_flags;
static uint64_t dma_last_phys;

// DMA memory regions - we prefer low memory (1-16MB) for DMA buffers
// to avoid conflicts with MMIO regions and framebuffers at higher addresses
#define DMA_LOW_START 0x100000       // 1MB
#define DMA_LOW_END 0x1000000        // 16MB
#define DMA_32BIT_END 0x100000000ULL // 4GB

void dma_alloc_init(void) { klog_puts("[DMA] Allocator initialized\n"); }

/* DMA RAM has no remaining device/user aliases when its owner releases it.
 * Restore the shared direct map before the PMM can give the frames to a WB
 * user mapping or LinuxKPI allocation. Retyping a dirty WB page after the
 * PTE change is too late: flush it while the old type is still installed. */
static bool dma_retype_page(uint64_t phys, uint64_t cache_flags) {
  uint64_t virt = phys + pmm_get_hhdm_offset();
  __asm__ volatile("mfence" ::: "memory");
  for (size_t i = 0; i < PAGE_SIZE; i += 64)
    __asm__ volatile("clflush (%0)" :: "r"(virt + i) : "memory");
  __asm__ volatile("mfence" ::: "memory");
  return vmm_map_page(vmm_get_kernel_pml4(), virt, phys,
                      PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_NX |
                          cache_flags);
}

static bool dma_restore_pages(uint64_t phys, size_t pages) {
  for (size_t i = 0; i < pages; i++) {
    if (!dma_retype_page(phys + i * PAGE_SIZE, 0)) {
      klog_puts("[DMA] Cache restore failed; retaining DMA frames\n");
      return false;
    }
  }
  return true;
}

/* An IRQ-masked caller cannot wait for remote TLB acknowledgements. Keep
 * restored frames out of the PMM until the VMM drains the pending work. */
static void dma_release_restored_pages(uint64_t phys, size_t pages) {
  if (tlb_flush_deferred_drain()) {
    pmm_free_pages((void *)phys, pages);
    return;
  }
  while (pages) {
    uint16_t batch = pages > 32768 ? 32768 : (uint16_t)pages;
    vmm_defer_frame_free((void *)phys, batch);
    phys += (uint64_t)batch * PAGE_SIZE;
    pages -= batch;
  }
}

dma_buffer_t *dma_alloc(size_t size, uint32_t flags) {
  if (size == 0 || size > SIZE_MAX - (PAGE_SIZE - 1))
    return NULL;

  /* PMM and heap already serialize their allocations. A DMA-wide spinlock
   * would mask IRQs and prevent the VMM from completing cache shootdowns.
   * Coherent x86 DMA RAM stays WB, including for IRQ-masked callers. */
  __atomic_store_n(&dma_last_failure, "none", __ATOMIC_RELAXED);
  __atomic_store_n(&dma_last_flags, flags, __ATOMIC_RELAXED);
  __atomic_store_n(&dma_last_phys, 0, __ATOMIC_RELAXED);
  if ((flags & DMA_FLAG_NOCACHE) && !hal_irq_enabled()) {
    __atomic_store_n(&dma_last_failure, "uncached DMA requires IRQs enabled",
                     __ATOMIC_RELAXED);
    return NULL;
  }

  // The buddy allocator rounds to a power of two. Track the full physical
  // extent so every page is mapped, scrubbed, and eventually returned.
  size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  size_t allocated_pages = 1;
  while (allocated_pages < pages) {
    if (allocated_pages > SIZE_MAX / 2)
      return NULL;
    allocated_pages <<= 1;
  }

  // Allocate physical memory
  void *phys = NULL;

  if (flags & DMA_FLAG_ANYWHERE) {
    phys = pmm_alloc_pages(allocated_pages);
  }

  if (!phys && (flags & DMA_FLAG_LOW)) {
    // Try low memory first (1-16MB region)
    phys = pmm_alloc_pages_range(allocated_pages, DMA_LOW_START, DMA_LOW_END);
  }

  if (!phys && (flags & DMA_FLAG_32BIT)) {
    // Try below 4GB
    phys = pmm_alloc_pages_range(allocated_pages, DMA_LOW_START, DMA_32BIT_END);
  }

  if (!phys && !(flags & DMA_FLAG_ANYWHERE)) {
    // Default: try low memory, then anywhere below 4GB
    phys = pmm_alloc_pages_range(allocated_pages, DMA_LOW_START, DMA_LOW_END);
    if (!phys) {
      phys = pmm_alloc_pages_range(allocated_pages, DMA_LOW_END, DMA_32BIT_END);
    }
  }

  if (!phys) {
    __atomic_store_n(&dma_last_failure, "PMM physical allocation", __ATOMIC_RELAXED);
    klog_puts("[DMA] Failed to allocate physical memory\n");
    return NULL;
  }
  __atomic_store_n(&dma_last_phys, (uint64_t)phys, __ATOMIC_RELAXED);

  uint64_t virt = (uint64_t)phys + pmm_get_hhdm_offset();
  for (size_t i = 0; (flags & DMA_FLAG_NOCACHE) && i < allocated_pages; i++) {
    uint64_t page_phys = (uint64_t)phys + i * PAGE_SIZE;
    if (!dma_retype_page(page_phys, PAGE_FLAG_PCD | PAGE_FLAG_PWT)) {
      __atomic_store_n(&dma_last_failure, "VMM page mapping", __ATOMIC_RELAXED);
      klog_puts("[DMA] Failed to map page\n");
      if (dma_restore_pages((uint64_t)phys, i))
        dma_release_restored_pages((uint64_t)phys, allocated_pages);
      return NULL;
    }
  }
  if ((flags & DMA_FLAG_NOCACHE) && !tlb_flush_deferred_drain()) {
    __atomic_store_n(&dma_last_failure, "incomplete DMA cache shootdown", __ATOMIC_RELAXED);
    if (dma_restore_pages((uint64_t)phys, allocated_pages))
      dma_release_restored_pages((uint64_t)phys, allocated_pages);
    return NULL;
  }

  // Allocate the dma_buffer_t structure
  // We use kmalloc for this small structure (not DMA memory)
  extern void *kmalloc(size_t);
  dma_buffer_t *buf = (dma_buffer_t *)kmalloc(sizeof(dma_buffer_t));
  if (!buf) {
    __atomic_store_n(&dma_last_failure, "heap metadata allocation", __ATOMIC_RELAXED);
    klog_puts("[DMA] Failed to allocate buffer structure\n");
    if (!(flags & DMA_FLAG_NOCACHE))
      pmm_free_pages(phys, allocated_pages);
    else if (dma_restore_pages((uint64_t)phys, allocated_pages))
      dma_release_restored_pages((uint64_t)phys, allocated_pages);
    return NULL;
  }

  buf->virt = (void *)virt;
  buf->phys = (uint64_t)phys;
  buf->size = size;
  buf->pages = allocated_pages;
  buf->flags = flags;

  // Zero the buffer
  memset(buf->virt, 0, allocated_pages * PAGE_SIZE);

  return buf;
}

void dma_free(dma_buffer_t *buf) {
  if (!buf)
    return;

  // buf->pages records the complete power-of-two buddy allocation.
  if (!(buf->flags & DMA_FLAG_NOCACHE))
    pmm_free_pages((void *)buf->phys, buf->pages);
  else if (dma_restore_pages(buf->phys, buf->pages))
    dma_release_restored_pages(buf->phys, buf->pages);

  // Free the structure
  extern void kfree(void *);
  kfree(buf);

}

// Convenience function for single page allocation
void *dma_alloc_page(uint64_t *phys_out) {
  return dma_alloc_page_flags(DMA_FLAG_32BIT, phys_out);
}

void *dma_alloc_page_flags(uint32_t flags, uint64_t *phys_out) {
  dma_buffer_t *buf = dma_alloc(PAGE_SIZE, flags);
  if (!buf)
    return NULL;

  if (phys_out)
    *phys_out = buf->phys;

  void *virt = buf->virt;

  // Free the structure but keep the memory
  extern void kfree(void *);
  kfree(buf);

  return virt;
}

void *dma_alloc_pages(size_t count, uint64_t *phys_out) {
  return dma_alloc_pages_flags(count, DMA_FLAG_32BIT, phys_out);
}

void *dma_alloc_pages_flags(size_t count, uint32_t flags,
                            uint64_t *phys_out) {
  if (count > SIZE_MAX / PAGE_SIZE)
    return NULL;
  dma_buffer_t *buf = dma_alloc(count * PAGE_SIZE, flags);
  if (!buf)
    return NULL;

  if (phys_out)
    *phys_out = buf->phys;

  void *virt = buf->virt;

  extern void kfree(void *);
  kfree(buf);

  return virt;
}

void dma_free_page(void *virt) {
  if (!virt)
    return;

  // Calculate physical address from virtual
  uint64_t phys = (uint64_t)virt - pmm_get_hhdm_offset();
  if (dma_restore_pages(phys, 1))
    dma_release_restored_pages(phys, 1);
}

const char *dma_get_last_failure(void) { return __atomic_load_n(&dma_last_failure, __ATOMIC_RELAXED); }

uint32_t dma_get_last_flags(void) { return __atomic_load_n(&dma_last_flags, __ATOMIC_RELAXED); }

uint64_t dma_get_last_phys(void) { return __atomic_load_n(&dma_last_phys, __ATOMIC_RELAXED); }
