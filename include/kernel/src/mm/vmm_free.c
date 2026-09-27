#include "vmm.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "pmm.h"
#include "vma.h"
#include <stddef.h>
#include <stdint.h>

static void reclaim_unmapped_file_cache(struct vma *v) {
  if (!v)
    return;
  reclaim_unmapped_file_cache(v->left);
  reclaim_unmapped_file_cache(v->right);
  if (v->file_node) {
    vfs_node_t *node = (vfs_node_t *)v->file_node;
    if (vfs_node_is_alive(node) && (node->flags & FS_PAGE_CACHE))
      vfs_cache_clear_unused(node);
  }
}

void vmm_free_user_pages(uint64_t cr3) {
  if (cr3 == 0)
    return;

  // Safety: never free the active PML4.
  uint64_t active_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
  if (cr3 == (active_cr3 & PAGE_MASK)) {
    klog_puts("[VMM] WARNING: refusing to free active CR3!\n");
    return;
  }

  uint64_t  hhdm     = pmm_get_hhdm_offset();
  uint64_t *pml4_virt = (uint64_t *)(hhdm + cr3);

  for (size_t i = 0; i < 256; i++) {
    if (!(pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;

    uint64_t  pdpt_phys = pml4_virt[i] & PAGE_MASK;
    uint64_t *pdpt_virt = (uint64_t *)(hhdm + pdpt_phys);

    for (size_t j = 0; j < 512; j++) {
      if (!(pdpt_virt[j] & PAGE_FLAG_PRESENT))
        continue;
      if (pdpt_virt[j] & PAGE_FLAG_PS) // 1GB huge page — skip
        continue;

      uint64_t  pd_phys = pdpt_virt[j] & PAGE_MASK;
      uint64_t *pd_virt = (uint64_t *)(hhdm + pd_phys);

      for (size_t k = 0; k < 512; k++) {
        if (!(pd_virt[k] & PAGE_FLAG_PRESENT))
          continue;

        if (pd_virt[k] & PAGE_FLAG_PS) {
          // 2MB huge page: free all 512 constituent 4KB frames.
          uint64_t huge_phys = pd_virt[k] & PAGE_MASK;
          for (size_t p = 0; p < 512; p++)
            pmm_free_page((void *)(huge_phys + p * 4096));
          pd_virt[k] = 0;
          continue;
        }

        uint64_t  pt_phys = pd_virt[k] & PAGE_MASK;
        uint64_t *pt_virt = (uint64_t *)(hhdm + pt_phys);

        uint64_t vdso_phys = vmm_get_vsyscall_page_phys();
        for (size_t l = 0; l < 512; l++) {
          if (pt_virt[l] & PAGE_FLAG_PRESENT) {
            uint64_t frame = pt_virt[l] & PAGE_MASK;
            if (frame != vdso_phys)
              pmm_free_page((void *)frame);
          }
        }
        pmm_free_page((void *)pt_phys);
      }
      pmm_free_page((void *)pd_phys);
    }
    pmm_free_page((void *)pdpt_phys);
  }

  pmm_free_page((void *)cr3);
}

void vmm_free_user_pages_vma(uint64_t cr3, struct vma_list *vmas) {
  if (cr3 == 0)
    return;

  if (!vmas) {
    vmm_free_user_pages(cr3);
    return;
  }

  uint64_t active_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
  if (cr3 == (active_cr3 & PAGE_MASK)) {
    klog_puts("[VMM] WARNING: refusing to free active CR3!\n");
    return;
  }

  uint64_t  hhdm     = pmm_get_hhdm_offset();
  uint64_t *pml4_virt = (uint64_t *)(hhdm + cr3);
  uint64_t  vdso_phys = vmm_get_vsyscall_page_phys();

  for (size_t i = 0; i < 256; i++) {
    if (!(pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;

    uint64_t  pdpt_phys = pml4_virt[i] & PAGE_MASK;
    uint64_t *pdpt_virt = (uint64_t *)(hhdm + pdpt_phys);

    for (size_t j = 0; j < 512; j++) {
      if (!(pdpt_virt[j] & PAGE_FLAG_PRESENT))
        continue;
      if (pdpt_virt[j] & PAGE_FLAG_PS) // 1GB huge page — skip
        continue;

      uint64_t  pd_phys = pdpt_virt[j] & PAGE_MASK;
      uint64_t *pd_virt = (uint64_t *)(hhdm + pd_phys);

      for (size_t k = 0; k < 512; k++) {
        if (!(pd_virt[k] & PAGE_FLAG_PRESENT))
          continue;

        if (pd_virt[k] & PAGE_FLAG_PS) {
          // 2MB huge page — compute VA and check VMA.
          uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                        ((uint64_t)k << 21);
          struct vma *v = vma_find(vmas, va);
          if (v && (v->flags & MAP_SHARED)) {
            pd_virt[k] = 0;
            continue; // shared device page — do NOT free
          }
          uint64_t huge_phys = pd_virt[k] & PAGE_MASK;
          for (size_t p = 0; p < 512; p++)
            pmm_free_page((void *)(huge_phys + p * 4096));
          pd_virt[k] = 0;
          continue;
        }

        uint64_t  pt_phys = pd_virt[k] & PAGE_MASK;
        uint64_t *pt_virt = (uint64_t *)(hhdm + pt_phys);

        for (size_t l = 0; l < 512; l++) {
          if (!(pt_virt[l] & PAGE_FLAG_PRESENT))
            continue;

          uint64_t frame = pt_virt[l] & PAGE_MASK;
          if (frame == vdso_phys)
            continue;

          uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                        ((uint64_t)k << 21) | ((uint64_t)l << 12);
          struct vma *v = vma_find(vmas, va);

          if (v && (v->flags & MAP_PAGECACHE)) {
            /* Generic file mapping: this PTE holds a PMM reference to its
             * page-cache frame, which teardown must drop.  A shared write goes
             * straight into that frame, so flag it for writeback while the
             * dirty bit is still visible. */
            vfs_node_t *file = (vfs_node_t *)v->file_node;
            if ((v->flags & MAP_SHARED) && (pt_virt[l] & PAGE_FLAG_D) &&
                vfs_node_is_alive(file)) {
              uint32_t file_offset = (uint32_t)(v->offset + va - v->start);
              vfs_cache_mark_dirty(file, file_offset);
            }
            pmm_free_page((void *)frame);
            continue;
          }

          if (v && (v->flags & MAP_SHARED)) {
            /* Device-backed shared mappings (memfd/GEM/framebuffer) do not take
             * a per-PTE PMM reference; their backing object owns the frame. */
            continue;
          }

          pmm_free_page((void *)frame);
        }
        pmm_free_page((void *)pt_phys);
      }
      pmm_free_page((void *)pd_phys);
    }
    pmm_free_page((void *)pdpt_phys);
  }

  /* PTE teardown above dropped this process's references to file-backed
   * pages. Return cache pages that now have no mappings to the PMM; cache
   * pages still mapped by another process retain their extra references. */
  reclaim_unmapped_file_cache(vmas->root);
  pmm_free_page((void *)cr3);
}
