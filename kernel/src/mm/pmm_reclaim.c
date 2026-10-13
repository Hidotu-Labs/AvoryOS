#include "mm/pmm_internal.h"
#include "mm/numa.h"
#include "console/klog.h"
#include "hal/hal.h"
#include "lib/string.h"

extern void vmm_protect_active_tables(void);

bool pmm_is_reclaimable(uint64_t phys) {
  if (!pmm_internal_memmap)
    return false;
  for (uint64_t i = 0; i < pmm_internal_memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = pmm_internal_memmap->entries[i];
    if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
      if (phys >= entry->base && phys < (entry->base + entry->length)) {
        return true;
      }
    }
  }
  return false;
}

void pmm_reclaim_bootloader(uint64_t kernel_phys_base) {
  if (!pmm_internal_memmap)
    return;

  // STEP 0: Buffer the memory map locally.
  // The original memmap structure is in Bootloader Reclaimable memory!
  static struct limine_memmap_entry static_entries[128];
  uint64_t entry_count = pmm_internal_memmap->entry_count;
  if (entry_count > 128)
    entry_count = 128;

  for (uint64_t i = 0; i < entry_count; i++) {
    static_entries[i] = *pmm_internal_memmap->entries[i];
  }

  klog_puts("[PMM] Analyzing buffered memory map for reclamation...\n");

  // Allocate a temporary bitmap to track which pages are reclaimable.
  size_t temp_count = (pmm_bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint8_t *temp_bitmap_phys = pmm_alloc_pages(temp_count);
  if (!temp_bitmap_phys) {
    klog_puts("[PMM] ERROR: Failed to allocate temp bitmap for reclaim.\n");
    return;
  }
  uint8_t *temp_bitmap =
      (uint8_t *)((uint64_t)temp_bitmap_phys + pmm_physical_memory_offset);
  memset(temp_bitmap, 0, pmm_bitmap_size);

  // Step 1: Mark all reclaimable regions (excluding ACPI until fully parsed)
  for (uint64_t i = 0; i < entry_count; i++) {
    struct limine_memmap_entry *entry = &static_entries[i];

    bool reclaim = false;
    if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
      reclaim = true;
    } else if (entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
      (void)kernel_phys_base;
    }

    if (reclaim) {
      uint64_t start = entry->base;
      uint64_t len = entry->length;
      if (start < 0x100000) {
        uint64_t diff = 0x100000 - start;
        if (diff >= len)
          continue;
        start = 0x100000;
        len -= diff;
      }
      size_t pfn_start = start / PAGE_SIZE;
      size_t pfn_count = len / PAGE_SIZE;
      pmm_bitmap_set_range(temp_bitmap, pfn_start, pfn_count);
    }
  }

  // Step 2: VMM marks active page tables as used in main bitmap
  vmm_protect_active_tables();

  // Step 3: Safety check: If a page is marked for reclaim but VMM says it's
  // used, don't reclaim.
  for (size_t b = 0; b < pmm_bitmap_size; b++) {
    if (temp_bitmap[b]) {
      for (int i = 0; i < 8; i++) {
        if (temp_bitmap[b] & (1 << i)) {
          uint64_t pfn = b * 8 + i + pmm_lowest_page;
          if (pmm_bitmap_test(pmm_bitmap, pfn - pmm_lowest_page)) {
            temp_bitmap[b] &= ~(1 << i);
          }
        }
      }
    }
  }

  // Step 4: Safely perform the reclamation
  klog_puts("[PMM] Reclaiming unprotected bootloader pages...\n");

  hal_irq_state_t flags = hal_irq_save();

  for (size_t b = 0; b < pmm_bitmap_size; b++) {
    if (temp_bitmap[b]) {
      for (int i = 0; i < 8; i++) {
        if (temp_bitmap[b] & (1 << i)) {
          uint64_t start_pfn = (uint64_t)b * 8 + i + pmm_lowest_page;
          size_t count = 0;
          uint64_t curr_pfn = start_pfn;

          while (curr_pfn < pmm_highest_page) {
            size_t curr_idx = curr_pfn - pmm_lowest_page;
            if (temp_bitmap[curr_idx / 8] & (1 << (curr_idx % 8))) {
              count++;
              temp_bitmap[curr_idx / 8] &= ~(1 << (curr_idx % 8));
              curr_pfn++;
            } else
              break;
          }

          uint64_t p = 0;
          while (p < count) {
            uint64_t phys = (start_pfn + p) * PAGE_SIZE;
            size_t remaining = count - p;
            size_t order = 0;
            while (order < MAX_ORDER - 1) {
              uint64_t n_pages = 1ULL << (order + 1);
              if (n_pages > remaining || (phys % (n_pages * PAGE_SIZE)) != 0)
                break;
              if (numa_phys_to_node(phys) !=
                  numa_phys_to_node(phys + n_pages * PAGE_SIZE - 1))
                break;
              order++;
            }

            uint64_t p_count = 1ULL << order;
            uint64_t base_pfn = phys / PAGE_SIZE;
            pmm_bitmap_set_range(pmm_managed_bitmap, base_pfn, p_count);
            memset(&pmm_refcounts[base_pfn - pmm_lowest_page], 0,
                   p_count * sizeof(uint16_t));
            struct numa_node *node = numa_get_node(numa_phys_to_node(phys));
            spinlock_acquire(&node->zone.lock);
            buddy_free_internal(&node->zone, phys, order);
            node->usable_memory += p_count * PAGE_SIZE;
            spinlock_release(&node->zone.lock);
            pmm_usable_memory += p_count * PAGE_SIZE;
            p += p_count;
          }
        }
      }
    }
  }

  pmm_internal_memmap = NULL;
  hal_irq_restore(flags);

  klog_puts("[PMM] Bootloader memory reclaimed. Usable RAM now: ");
  klog_uint64(pmm_usable_memory / 1024 / 1024);
  klog_puts(" MB\n");

  pmm_free_pages(temp_bitmap_phys, temp_count);
}
