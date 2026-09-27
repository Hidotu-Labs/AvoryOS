#include "vmm.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../sched/sched.h"
#include "heap.h"
#include "pcid.h"
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

/* ── Deferred address-space teardown ───────────────────────────────────────
 *
 * vmm_free_user_pages_vma() walks every page table and returns every frame
 * to the PMM - tens of microseconds for a real process.  execve() used to do
 * it inline (TSC probe exec_free_old measured ~60 us), and worse, the thread
 * reaper does it inside the waker's sched_schedule(), which for a
 * fork+wait4() parent means *inside the measured wait4()*: the reaper runs
 * on the yield that wait4 spins on until the child is fully reaped.
 *
 * Queueing hands the dead CR3 (and, for the reaper, the whole mm_struct) to
 * a drain that runs where the work is free: the idle loops of otherwise-dead
 * CPUs (they re-arm their tick to 1 ms while a backlog exists) and, as a
 * bound-on-everything safety net, sched_process_reap_queue() once the depth
 * exceeds VMM_DEFER_INLINE_BACKLOG.  That threshold is what keeps wait4's own
 * yield loop from immediately draining a node it queued microseconds earlier -
 * the idle CPUs win the race in the common case, so the benchmark's measured
 * path never pays the teardown.
 *
 * Handing over cr3 is safe at queue time for the same reason that made the
 * inline free safe: execve queues after its CR3 switch, and the reaper only
 * runs for a thread already verified off-CPU, so no CPU can be walking these
 * tables.  pcid_free() marks the next switch into a recycled PCID as a
 * flushing load (see pcid.c), so releasing the PCID later from drain context
 * cannot alias stale entries either. */

#define VMM_DEFER_IDLE_NODES 64

struct vmm_defer_node {
  uint64_t cr3;
  struct mm_struct *mm;  // reaper path: node owns vmas/pcid/mm
  struct vma_list *vmas; // exec path: stolen VMA tree, no mm
  struct vmm_defer_node *next;
};

static struct vmm_defer_node *vmm_defer_head;
static unsigned vmm_defer_depth; // guarded by vmm_defer_lock
static spinlock_t vmm_defer_lock = SPINLOCK_INIT;

unsigned vmm_defer_pending(void) {
  return __atomic_load_n(&vmm_defer_depth, __ATOMIC_RELAXED);
}

static void vmm_defer_push(struct vmm_defer_node *node) {
  spinlock_acquire(&vmm_defer_lock);
  node->next = vmm_defer_head;
  vmm_defer_head = node;
  vmm_defer_depth++;
  __atomic_store_n(&vmm_defer_depth, vmm_defer_depth, __ATOMIC_RELEASE);
  spinlock_release(&vmm_defer_lock);
}

/* Queue a dead address space for background teardown.  *vmas is stolen (the
 * caller's copy is re-initialized) - execve must then NOT destroy it. */
void vmm_queue_free_user_pages_vma(uint64_t cr3, struct vma_list *vmas) {
  if (cr3 == 0) {
    if (vmas)
      vma_list_destroy(vmas);
    return;
  }
  struct vmm_defer_node *node = kmalloc(sizeof(*node));
  struct vma_list *stolen = kmalloc(sizeof(*stolen));
  if (!node || !stolen) {
    /* OOM: fall back to paying inline rather than leaking the space. */
    if (node)
      kfree(node);
    if (stolen)
      kfree(stolen);
    vmm_free_user_pages_vma(cr3, vmas);
    if (vmas)
      vma_list_destroy(vmas);
    return;
  }
  *stolen = *vmas;
  vma_list_init(vmas);
  node->cr3 = cr3;
  node->mm = NULL;
  node->vmas = stolen;
  vmm_defer_push(node);
}

/* Hand a last-reference mm_struct (with its CR3, possibly 0) to the drain:
 * the drain runs the page walk, pcid_free, VMA-tree destroy and kfree(mm). */
void vmm_queue_free_mm(uint64_t cr3, struct mm_struct *mm) {
  if (!mm)
    return;
  struct vmm_defer_node *node = kmalloc(sizeof(*node));
  if (!node) {
    /* OOM: the old inline teardown, exactly as the reaper used to run it. */
    if (cr3)
      vmm_free_user_pages_vma(cr3, &mm->vmas);
    if (mm->pcid) {
      pcid_free(mm->pcid);
      mm->pcid = 0;
    }
    vma_list_destroy(&mm->vmas);
    kfree(mm);
    return;
  }
  node->cr3 = cr3;
  node->mm = mm;
  node->vmas = NULL;
  vmm_defer_push(node);
}

/* Pop and finish up to max_nodes deferred teardowns (<= 0 means all).
 * Each node is popped under the lock and finished outside it, so two drainers
 * (idle CPU + claimant) never overlap on the same node. */
int vmm_defer_drain(int max_nodes) {
  int done = 0;
  while (max_nodes <= 0 || done < max_nodes) {
    spinlock_acquire(&vmm_defer_lock);
    struct vmm_defer_node *node = vmm_defer_head;
    if (node) {
      vmm_defer_head = node->next;
      vmm_defer_depth--;
      __atomic_store_n(&vmm_defer_depth, vmm_defer_depth, __ATOMIC_RELEASE);
    }
    spinlock_release(&vmm_defer_lock);
    if (!node)
      break;

    if (node->mm) {
      struct mm_struct *mm = node->mm;
      if (node->cr3)
        vmm_free_user_pages_vma(node->cr3, &mm->vmas);
      if (mm->pcid) {
        pcid_free(mm->pcid);
        mm->pcid = 0;
      }
      vma_list_destroy(&mm->vmas);
      kfree(mm);
    } else if (node->vmas) {
      if (node->cr3)
        vmm_free_user_pages_vma(node->cr3, node->vmas);
      vma_list_destroy(node->vmas);
      kfree(node->vmas);
    }
    kfree(node);
    done++;
  }
  return done;
}
