#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "pmm.h"
#include "tlb_shootdown.h"
#include "vma.h"
#include "vmm.h"
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

static uint64_t *clone_table(uint64_t *src_table_phys, int level, size_t start,
                             size_t end) {
  void *new_table_phys = pmm_alloc();
  if (!new_table_phys)
    return NULL;

  uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
  uint64_t *src_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_table_phys);

  for (size_t i = 0; i < 512; i++)
    new_virt[i] = 0;

  for (size_t i = start; i < end; i++) {
    if (!(src_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_virt[i] & PAGE_FLAG_USER))
      continue; // skip kernel / Limine mappings

    if (src_virt[i] & PAGE_FLAG_PS) {
      // 2MB huge page at level 2: deep-copy the 2MB data instead of treating
      // user heap memory as a page table.
      if (level == 2) {
        void *new_huge = pmm_alloc_huge_page();
        if (new_huge) {
          uint64_t src_huge_phys = src_virt[i] & PAGE_MASK;
          memcpy(PHYS_TO_VIRT((uint64_t)new_huge),
                 PHYS_TO_VIRT(src_huge_phys),
                 2ULL * 1024 * 1024);
          new_virt[i] = ((uint64_t)new_huge & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
        } else {
          void *pt_phys = pmm_alloc();
          if (!pt_phys)
            return NULL;
          uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt_phys);
          uint64_t src_huge_phys = src_virt[i] & PAGE_MASK;
          uint64_t pte_flags = (src_virt[i] & ~PAGE_MASK & ~PAGE_FLAG_PS) | PAGE_FLAG_PRESENT;
          for (size_t p = 0; p < 512; p++) {
            void *new_p = pmm_alloc();
            if (!new_p)
              return NULL;
            memcpy(PHYS_TO_VIRT((uint64_t)new_p),
                   PHYS_TO_VIRT(src_huge_phys + p * PAGE_SIZE),
                   PAGE_SIZE);
            pt_virt[p] = ((uint64_t)new_p & PAGE_MASK) | pte_flags;
          }
          new_virt[i] = ((uint64_t)pt_phys & PAGE_MASK) | PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_USER;
        }
      }
      continue;
    }

    if (level == 1) {
      uint64_t phys = src_virt[i] & PAGE_MASK;
      if (phys == vmm_get_vsyscall_page_phys()) {
        new_virt[i] = src_virt[i];
        continue;
      }
      // Leaf: allocate a fresh page and copy content.
      void *new_page_phys = pmm_alloc();
      if (!new_page_phys)
        return NULL;

      uint64_t *dst64 = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_page_phys);
      uint64_t *src64 = (uint64_t *)PHYS_TO_VIRT(phys);
      for (size_t w = 0; w < 512; w++)
        dst64[w] = src64[w];

      new_virt[i] =
          ((uint64_t)new_page_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
    } else {
      uint64_t *child_src_phys = (uint64_t *)(src_virt[i] & PAGE_MASK);
      uint64_t *child_new_phys = clone_table(child_src_phys, level - 1, 0, 512);
      if (!child_new_phys)
        return NULL;

      new_virt[i] =
          ((uint64_t)child_new_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
    }
  }

  return (uint64_t *)new_table_phys;
}

// Return the VMA containing `vaddr`, using a one-entry cache.  The page walk
// visits leaf pages in ascending address order, so consecutive pages almost
// always fall in the same VMA; caching the last hit avoids an AVL lookup for
// every single mapped page.
static struct vma *find_vma_cached(struct vma_list *vmas, uint64_t vaddr,
                                   struct vma **cache) {
  if (*cache && vaddr >= (*cache)->start && vaddr < (*cache)->end)
    return *cache;
  struct vma *found = vma_find(vmas, vaddr);
  if (found)
    *cache = found;
  return found;
}

static bool clone_table_vma(uint64_t *src_table_phys, int level,
                            size_t start, size_t end,
                            struct vma_list *vmas, uint64_t base_addr,
                            size_t *cow_count, struct vma **vma_cache,
                            uint64_t **new_table_out) {
  *new_table_out = NULL;
  void *new_table_phys = pmm_alloc();
  if (!new_table_phys)
    return false;

  uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
  *new_table_out = (uint64_t *)new_table_phys;
  uint64_t *src_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_table_phys);

  for (size_t i = 0; i < 512; i++)
    new_virt[i] = 0;

  for (size_t i = start; i < end; i++) {
    if (!(src_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_virt[i] & PAGE_FLAG_USER))
      continue;

    if (src_virt[i] & PAGE_FLAG_PS) {
      if (level == 2) {
        uint64_t child_base = base_addr | ((uint64_t)i << 21);
        struct vma *hv = find_vma_cached(vmas, child_base, vma_cache);
        if (hv && (hv->flags & MAP_SHARED)) {
          new_virt[i] = src_virt[i];
        } else {
          void *new_huge = pmm_alloc_huge_page();
          if (new_huge) {
            uint64_t src_huge_phys = src_virt[i] & PAGE_MASK;
            memcpy(PHYS_TO_VIRT((uint64_t)new_huge),
                   PHYS_TO_VIRT(src_huge_phys),
                   2ULL * 1024 * 1024);
            new_virt[i] = ((uint64_t)new_huge & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
          } else {
            void *pt_phys = pmm_alloc();
            if (!pt_phys)
              return false;
            uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt_phys);
            new_virt[i] = ((uint64_t)pt_phys & PAGE_MASK) |
                          PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_USER;
            uint64_t src_huge_phys = src_virt[i] & PAGE_MASK;
            uint64_t pte_flags = (src_virt[i] & ~PAGE_MASK & ~PAGE_FLAG_PS) | PAGE_FLAG_PRESENT;
            for (size_t p = 0; p < 512; p++) {
              void *new_p = pmm_alloc();
              if (!new_p)
                return false;
              memcpy(PHYS_TO_VIRT((uint64_t)new_p),
                     PHYS_TO_VIRT(src_huge_phys + p * PAGE_SIZE),
                     PAGE_SIZE);
              pt_virt[p] = ((uint64_t)new_p & PAGE_MASK) | pte_flags;
            }
          }
        }
      }
      continue;
    }

    if (level == 1) {
      uint64_t page_vaddr = base_addr | (i << 12);
      uint64_t phys = src_virt[i] & PAGE_MASK;

      struct vma *pvma = find_vma_cached(vmas, page_vaddr, vma_cache);
      bool shared = pvma && (pvma->flags & MAP_SHARED);

      if (phys == vmm_get_vsyscall_page_phys() || shared) {
        // Shared/vDSO: alias the physical frame.  Page-cache-backed shared
        // mappings carry one PMM reference per PTE, so the child's copy needs
        // its own; device-backed frames are owned by their backing object.
        if (shared && (pvma->flags & MAP_PAGECACHE) && pmm_is_managed(phys))
          pmm_incref((void *)phys);
        new_virt[i] = src_virt[i];
      } else {
        // Private: CoW — mark both sides read-only and bump the refcount.
        if (pmm_is_managed(phys)) {
          if (src_virt[i] & PAGE_FLAG_RW) {
            src_virt[i] &= ~PAGE_FLAG_RW;
            src_virt[i] |= PAGE_FLAG_COW;
            if (cow_count)
              (*cow_count)++;
          }
          pmm_incref((void *)phys);
        }
        new_virt[i] = src_virt[i];
      }
    } else {
      int shift = 12 + 9 * (level - 1);
      uint64_t child_base = base_addr | ((uint64_t)i << shift);

      uint64_t *child_src_phys = (uint64_t *)(src_virt[i] & PAGE_MASK);
      uint64_t *child_new_phys = NULL;
      bool child_ok = clone_table_vma(
          child_src_phys, level - 1, 0, 512, vmas, child_base, cow_count,
          vma_cache, &child_new_phys);
      if (child_new_phys)
        new_virt[i] = ((uint64_t)child_new_phys & PAGE_MASK) |
                      (src_virt[i] & ~PAGE_MASK);
      if (!child_ok)
        return false;
    }
  }

  return true;
}

uint64_t vmm_clone_user_mappings(uint64_t *src_pml4_phys) {
  vmm_lock_acquire();

  void *new_pml4_phys = pmm_alloc();
  if (!new_pml4_phys) {
    vmm_lock_release();
    return 0;
  }

  uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
  uint64_t *src_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_pml4_phys);

  for (size_t i = 0; i < 512; i++)
    new_pml4_virt[i] = 0;

  // Deep-copy user half (0-255).
  for (size_t i = 0; i < 256; i++) {
    if (!(src_pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_pml4_virt[i] & PAGE_FLAG_USER))
      continue;

    uint64_t *child_src_phys = (uint64_t *)(src_pml4_virt[i] & PAGE_MASK);
    uint64_t *child_new_phys = clone_table(child_src_phys, 3, 0, 512);
    if (!child_new_phys) {
      vmm_lock_release();
      return 0;
    }

    new_pml4_virt[i] = ((uint64_t)child_new_phys & PAGE_MASK) |
                       (src_pml4_virt[i] & ~PAGE_MASK);
  }

  // Shallow-copy the canonical kernel half (256-511), rather than copying it
  // from the parent.  A parent with a missing higher-half root entry (for
  // example, the HHDM slot used by xAPIC EOI) must not propagate that hole to
  // every child forked from it.
  uint64_t *kernel_pml4_virt =
      (uint64_t *)PHYS_TO_VIRT((uint64_t)vmm_get_kernel_pml4());
  for (size_t i = 256; i < 512; i++)
    new_pml4_virt[i] = kernel_pml4_virt[i];

  vmm_lock_release();
  return (uint64_t)new_pml4_phys;
}

uint64_t vmm_clone_user_mappings_vma(uint64_t *src_pml4_phys,
                                     struct vma_list *vmas) {
  vmm_lock_acquire();

  void *new_pml4_phys = pmm_alloc();
  if (!new_pml4_phys) {
    vmm_lock_release();
    return 0;
  }

  uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
  uint64_t *src_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_pml4_phys);

  for (size_t i = 0; i < 512; i++)
    new_pml4_virt[i] = 0;

  size_t cow_count = 0;
  struct vma *vma_cache = NULL;

  // Clone user half with VMA awareness. Keep partial child tables linked to
  // the new PML4 so an allocation failure can unwind the entire tree.
  bool clone_ok = true;
  for (size_t i = 0; i < 256; i++) {
    if (!(src_pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_pml4_virt[i] & PAGE_FLAG_USER))
      continue;

    uint64_t base_addr = (uint64_t)i << 39;
    // Sign-extend if bit 47 is set (canonical address).
    if (base_addr & (1ULL << 47))
      base_addr |= 0xFFFF000000000000ULL;

    uint64_t *child_src_phys = (uint64_t *)(src_pml4_virt[i] & PAGE_MASK);
    uint64_t *child_new_phys = NULL;
    bool child_ok = clone_table_vma(
        child_src_phys, 3, 0, 512, vmas, base_addr, &cow_count, &vma_cache,
        &child_new_phys);
    if (child_new_phys)
      new_pml4_virt[i] = ((uint64_t)child_new_phys & PAGE_MASK) |
                         (src_pml4_virt[i] & ~PAGE_MASK);
    if (!child_ok) {
      clone_ok = false;
      break;
    }
  }

  if (!clone_ok) {
    /* The partial tree is linked to this PML4, so teardown can release every
     * table, private-page reference, and shared page-cache reference acquired
     * so far. Flush the parent's CoW context before returning as well. */
    if (cow_count > 0)
      tlb_flush_deferred_context((uint64_t)src_pml4_phys);
    vmm_lock_release();
    vmm_free_user_pages_vma((uint64_t)new_pml4_phys, vmas);
    return 0;
  }

  // Keep the kernel half anchored to the canonical kernel page table so a
  // missing parent entry cannot be inherited by the child.
  uint64_t *kernel_pml4_virt =
      (uint64_t *)PHYS_TO_VIRT((uint64_t)vmm_get_kernel_pml4());
  for (size_t i = 256; i < 512; i++)
    new_pml4_virt[i] = kernel_pml4_virt[i];

  // If any pages were converted to CoW, invalidate the parent's address-space
  // context once instead of sending thousands of per-page IPIs or flushing
  // unrelated address spaces on every CPU.
  if (cow_count > 0) {
    /* Requested, not waited for: vmm_lock is still held here, and every target
     * reaches its page fault handler through that lock.  The context flush is
     * issued from vmm_lock_release(), a few instructions later on this core. */
    tlb_flush_deferred_context((uint64_t)src_pml4_phys);
  }

  vmm_lock_release();
  return (uint64_t)new_pml4_phys;
}


uint64_t *vmm_create_pml4(void) {
  vmm_lock_acquire();

  void *new_pml4_phys = pmm_alloc();
  if (!new_pml4_phys) {
    vmm_lock_release();
    return NULL;
  }

  uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
  uint64_t *src_pml4_virt =
      (uint64_t *)PHYS_TO_VIRT((uint64_t)vmm_get_kernel_pml4());

  // Blank user half.
  for (size_t i = 0; i < 256; i++)
    new_pml4_virt[i] = 0;

  // Shared kernel half.
  for (size_t i = 256; i < 512; i++)
    new_pml4_virt[i] = src_pml4_virt[i];

  vmm_lock_release();

  return (uint64_t *)new_pml4_phys;
}
