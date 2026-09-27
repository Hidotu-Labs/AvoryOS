#include "vmm.h"
#include "../console/klog.h"
#include "pmm.h"
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

bool vmm_initialized = false;

static uint64_t vmm_deep_clone_table(uint64_t src_phys, int level) {
  if (level < 1)
    return 0;

  void *new_page_phys = pmm_alloc();
  if (!new_page_phys)
    return 0;
  uint64_t *new_table_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_page_phys);
  uint64_t *src_table_virt = (uint64_t *)PHYS_TO_VIRT(src_phys);

  for (int i = 0; i < 512; i++) {
    uint64_t entry = src_table_virt[i];
    if (!(entry & PAGE_FLAG_PRESENT)) {
      new_table_virt[i] = 0;
      continue;
    }

    // Huge page (level 2/3) or leaf (level 1): copy the mapping itself.
    if ((level > 1 && (entry & PAGE_FLAG_PS)) || level == 1) {
      new_table_virt[i] = entry;
      continue;
    }

    // Recurse to clone the sub-table.
    uint64_t sub_phys = entry & PAGE_MASK;
    uint64_t new_sub_phys = vmm_deep_clone_table(sub_phys, level - 1);
    if (!new_sub_phys)
      return 0; // OOM — caller should roll back

    new_table_virt[i] = (new_sub_phys & PAGE_MASK) | (entry & ~PAGE_MASK);
  }

  return (uint64_t)new_page_phys;
}

void vmm_init(void) {
  // 1. Identify and protect the current (boot) page tables in PMM.
  uint64_t *boot_pml4 = vmm_get_active_pml4();
  pmm_mark_used(boot_pml4, 1);

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)boot_pml4);

  // 2. Clear out Limine's lower-half identity mapping.
  for (int i = 0; i < 256; i++)
    pml4_virt[i] = 0;

  // 3. Create a fresh, private PML4 and DEEP-COPY the kernel half.
  //    This is vital — Limine places its tables in reclaimable memory.
  void *new_pml4_phys = pmm_alloc();
  if (!new_pml4_phys) {
    klog_puts("[VMM] FATAL: Failed to allocate new kernel PML4\n");
    return;
  }
  uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
  for (int i = 0; i < 512; i++)
    new_pml4_virt[i] = 0;

  for (int i = 256; i < 512; i++) {
    uint64_t entry = pml4_virt[i];
    if (entry & PAGE_FLAG_PRESENT) {
      if (entry & PAGE_FLAG_PS) {
        new_pml4_virt[i] = entry;
      } else {
        uint64_t cloned = vmm_deep_clone_table(entry & PAGE_MASK, 3);
        new_pml4_virt[i] = (cloned & PAGE_MASK) | (entry & ~PAGE_MASK);
      }
    }
  }

  vmm_set_kernel_pml4((uint64_t *)new_pml4_phys);

  // Switch to the newly allocated kernel-owned PML4.
  __asm__ volatile("mov %0, %%cr3" ::"r"(new_pml4_phys) : "memory");
  vmm_initialized = true;
  klog_puts("[VMM] Switched to new, independent kernel-owned PML4.\n");
}

static void vmm_protect_table_recursive(uint64_t phys, int level) {
  if (level < 1)
    return;

  if (pmm_is_reclaimable(phys))
    pmm_mark_used((void *)phys, 1);

  uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(phys);
  for (int i = 0; i < 512; i++) {
    uint64_t entry = virt[i];
    if (!(entry & PAGE_FLAG_PRESENT))
      continue;

    if (level > 1) {
      if (entry & PAGE_FLAG_PS) {
        uint64_t leaf_phys = entry & PAGE_MASK;
        if (pmm_is_reclaimable(leaf_phys)) {
          size_t page_count = (level == 3) ? 0x40000 : 0x200;
          pmm_mark_used((void *)leaf_phys, page_count);
        }
      } else {
        vmm_protect_table_recursive(entry & PAGE_MASK, level - 1);
      }
    } else {
      uint64_t leaf_phys = entry & PAGE_MASK;
      if (pmm_is_reclaimable(leaf_phys))
        pmm_mark_used((void *)leaf_phys, 1);
    }
  }
}

void vmm_protect_active_tables(void) {
  uint64_t *pml4 = vmm_get_active_pml4();
  vmm_protect_table_recursive((uint64_t)pml4, 4);
}
