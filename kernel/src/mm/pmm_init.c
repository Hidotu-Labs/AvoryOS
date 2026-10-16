#include "mm/pmm_internal.h"
#include "mm/numa.h"
#include "console/klog.h"
#include "hal/hal.h"
#include "lib/string.h"

uint8_t *pmm_bitmap = NULL;
uint8_t *pmm_managed_bitmap = NULL;
uint16_t *pmm_refcounts = NULL;
size_t pmm_bitmap_size = 0;
uint64_t pmm_lowest_page = 0xFFFFFFFFFFFFFFFF;
uint64_t pmm_highest_page = 0;
uint64_t pmm_page_count = 0;
uint64_t pmm_usable_memory = 0;
uint64_t pmm_total_memory = 0;
uint64_t pmm_physical_memory_offset = 0;
struct limine_memmap_response *pmm_internal_memmap = NULL;
uint64_t pmm_zero_page_phys = 0;

void pmm_init_early(uint64_t hhdm_offset) {
  pmm_physical_memory_offset = hhdm_offset;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
  pmm_physical_memory_offset = hhdm_offset;
  pmm_internal_memmap = memmap;
  pmm_highest_page = 0;
  pmm_lowest_page = 0xFFFFFFFFFFFFFFFF;

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    if (entry->type == LIMINE_MEMMAP_USABLE ||
        entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
        entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES ||
        entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
        entry->type == LIMINE_MEMMAP_ACPI_NVS ||
        entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
      pmm_total_memory += entry->length;
    }

    uint64_t top = entry->base + entry->length;

    // Only managed memory bounds determine metadata overhead
    if (entry->type == LIMINE_MEMMAP_USABLE ||
        entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
      if (entry->base / PAGE_SIZE < pmm_lowest_page) {
        pmm_lowest_page = entry->base / PAGE_SIZE;
      }
      if ((top + PAGE_SIZE - 1) / PAGE_SIZE > pmm_highest_page) {
        pmm_highest_page = (top + PAGE_SIZE - 1) / PAGE_SIZE;
      }
    }
  }

  if (pmm_highest_page > pmm_lowest_page) {
    pmm_page_count = pmm_highest_page - pmm_lowest_page;
  } else {
    pmm_page_count = 0;
  }

  pmm_bitmap_size = pmm_page_count / 8;
  if (pmm_page_count % 8 != 0)
    pmm_bitmap_size++;

  size_t refcount_table_size = pmm_page_count * sizeof(uint16_t);
  size_t total_metadata_size = pmm_bitmap_size * 2 + refcount_table_size;
  total_metadata_size = (total_metadata_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  klog_puts("[PMM] Required Metadata Size: ");
  klog_uint64(total_metadata_size / 1024);
  klog_puts(" KB\n");

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    if (entry->type == LIMINE_MEMMAP_USABLE &&
        entry->length >= total_metadata_size) {
      pmm_bitmap = (uint8_t *)(entry->base + hhdm_offset);
      pmm_managed_bitmap = pmm_bitmap + pmm_bitmap_size;
      pmm_refcounts = (uint16_t *)((uint64_t)pmm_managed_bitmap + pmm_bitmap_size);

      memset(pmm_bitmap, 0xFF, pmm_bitmap_size);
      memset(pmm_managed_bitmap, 0, pmm_bitmap_size);
      break;
    }
  }

  if (pmm_bitmap == NULL) {
    klog_puts("[PMM] FATAL: Could not find a large enough usable memory region "
              "for PMM metadata!\n");
    while (1)
      hal_cpu_halt();
  }

  buddy_zone_init(&b_zone);

  uint64_t bitmap_phys_base = (uint64_t)pmm_bitmap - hhdm_offset;

  klog_puts("[PMM] Identified Physical Memory: ");
  klog_uint64(pmm_total_memory / 1024 / 1024);
  klog_puts(" MB\n");
  klog_puts("[PMM] Metadata overhead (Bitmaps + Refcounts): ");
  klog_uint64(total_metadata_size / 1024);
  klog_puts(" KB\n");

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    if (entry->type == LIMINE_MEMMAP_USABLE) {
      uint64_t entry_phys_base = entry->base;
      uint64_t entry_length = entry->length;

      uint64_t p = 0;
      while (p < entry_length) {
        uint64_t phys = entry_phys_base + p;
        uint64_t pfn = phys / PAGE_SIZE;

        // Skip first 1MB
        if (phys < 0x100000) {
          p += (0x100000 - phys);
          continue;
        }

        // Skip full metadata region
        if (phys >= bitmap_phys_base &&
            phys < bitmap_phys_base + total_metadata_size) {
          p += (bitmap_phys_base + total_metadata_size - phys);
          continue;
        }

        size_t order = 0;
        while (order < MAX_ORDER - 1) {
          uint64_t next_order_pages = 1ULL << (order + 1);
          uint64_t next_order_size = next_order_pages * PAGE_SIZE;

          if (p + next_order_size > entry_length)
            break;
          if ((phys % next_order_size) != 0)
            break;

          uint64_t phys_end = phys + next_order_size;
          if (!(phys_end <= bitmap_phys_base ||
                phys >= bitmap_phys_base + total_metadata_size)) {
            break;
          }

          order++;
        }

        pmm_bitmap_set_range(pmm_managed_bitmap, pfn, (1ULL << order));
        pmm_bitmap_clear_range(pmm_bitmap, pfn, (1ULL << order));

        memset(&pmm_refcounts[pfn - pmm_lowest_page], 0,
               (1ULL << order) * sizeof(uint16_t));

        int node = numa_phys_to_node(phys);
        struct numa_node *n = numa_get_node(node);

        struct buddy_block *block = virt_to_buddy(phys);
        block->order = order;
        block->magic = n->zone.magic;
        list_add_tail(&block->node, &n->zone.free_list[order]);

        n->usable_memory += (1ULL << order) * PAGE_SIZE;
        pmm_usable_memory += (1ULL << order) * PAGE_SIZE;
        p += (1ULL << order) * PAGE_SIZE;
      }
    }
  }

  klog_puts("[PMM] Initialized. Usable memory: ");
  klog_uint64(pmm_usable_memory / 1024 / 1024);
  klog_puts(" MB\n");

  // Allocate and permanently pin the shared zero page
  void *zp = pmm_alloc_page();
  if (zp) {
    pmm_zero_page_phys = (uint64_t)zp;
    memset((void *)(pmm_zero_page_phys + hhdm_offset), 0, PAGE_SIZE);
    uint64_t pfn = pmm_zero_page_phys / PAGE_SIZE;
    pmm_refcounts[pfn - pmm_lowest_page] = 0xFFFF; // Permanently pinned
  }
}
