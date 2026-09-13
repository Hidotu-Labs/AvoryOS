/* unmap_mapping_range() backend (called from linuxkpi/src/mmap.c).
 *
 * Linux's unmap_mapping_range() walks an address_space's interval tree of
 * mappings and zaps the page-table entries covering a file range.  The native
 * model keeps those mappings in the per-process VMA trees plus the LinuxKPI
 * bridge wrappers, so this walks every address space that has a bridged VMA
 * mapping the requested address_space and unmaps the PTE range each VMA
 * contributes.  The VMAs themselves stay in place: the next access refaults
 * through the bridge (or faults, for mappings with no fault handler), which is
 * exactly what TTM/amdgpu rely on after a BO move or free.
 *
 * Pages are deliberately left to their owner (BO/page cache), matching the
 * native munmap path, which only frees anonymous-private and MAP_PAGECACHE
 * frames.
 *
 * Locks: the thread list is read under tid_lock while taking a lifetime
 * reference on each mm, so a concurrent thread reap cannot free the mm (or its
 * page tables) under us.  The VMA ranges are collected under mm->lock and the
 * PTEs are zapped after dropping it - a TLB shootdown waits for other CPUs, and
 * a CPU faulting into this address space waits on mm->lock with interrupts
 * masked, so zapping under that lock can deadlock.  vmm_unmap_page() runs the
 * deferred shootdowns itself when it releases vmm_lock.
 */

#include "../console/klog.h"
#include "../sched/sched.h"

#include "heap.h"
#include "pcid.h"
#include "pmm.h"
#include "vma.h"
#include "vmm.h"

/* Matched VA ranges per batch; a bigger batch means fewer tree walks. */
#define VMA_UNMAP_BATCH 16
/* Distinct address spaces handled per call; every thread sharing an mm counts
 * once.  More than this many live processes is not plausible on this system,
 * and the overflow path is reported rather than silent. */
#define VMA_UNMAP_MM_MAX 128

static void mm_put(struct mm_struct *mm, uint64_t cr3) {
  int refs;

  refs = __atomic_sub_fetch(&mm->ref_count, 1, __ATOMIC_ACQ_REL);
  if (refs != 0)
    return;

  /* Last reference: mirror the scheduler reaper's teardown for a shared mm
   * (kernel/src/sched/sched_reap.c).  The page tables must be freed before the
   * VMA list they describe. */
  if (cr3)
    vmm_free_user_pages_vma(cr3, &mm->vmas);
  if (mm->pcid) {
    pcid_free(mm->pcid);
    mm->pcid = 0;
  }
  vma_list_destroy(&mm->vmas);
  kfree(mm);
}

void vma_unmap_mapping_range(void *mapping, uint64_t file_begin,
                             uint64_t file_end, bool even_cows) {
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  struct mm_struct *done[VMA_UNMAP_MM_MAX];
  int ndone = 0;
  bool overflow = false;

  if (!mapping)
    return;

  for (;;) {
    struct mm_struct *mm = NULL;
    uint64_t cr3 = 0;
    struct thread *t;
    int i;

    spinlock_acquire(&tid_lock);
    for (t = global_thread_list; t; t = t->global_next) {
      bool seen = false;

      if (!t->mm || !t->cr3)
        continue;
      for (i = 0; i < ndone; i++) {
        if (done[i] == t->mm) {
          seen = true;
          break;
        }
      }
      if (seen)
        continue;
      if (ndone >= VMA_UNMAP_MM_MAX) {
        overflow = true;
        break;
      }
      __atomic_add_fetch(&t->mm->ref_count, 1, __ATOMIC_ACQ_REL);
      mm = t->mm;
      cr3 = t->cr3;
      break;
    }
    spinlock_release(&tid_lock);

    if (!mm)
      break;

    uint64_t from_start = 0;
    for (;;) {
      uint64_t starts[VMA_UNMAP_BATCH], ends[VMA_UNMAP_BATCH];
      int n, k;

      spinlock_acquire(&mm->lock);
      n = vma_collect_mapping(&mm->vmas, mapping, file_begin, file_end,
                              even_cows, from_start, starts, ends,
                              VMA_UNMAP_BATCH);
      spinlock_release(&mm->lock);

      for (k = 0; k < n; k++) {
        for (uint64_t va = starts[k]; va < ends[k]; va += PAGE_SIZE)
          vmm_unmap_page((uint64_t *)cr3, va);
      }

      if (n < VMA_UNMAP_BATCH)
        break;
      from_start = starts[VMA_UNMAP_BATCH - 1];
    }

    done[ndone++] = mm;
    mm_put(mm, cr3);
  }

  if (overflow) {
    klog_puts("[WARN] unmap_mapping_range: more than 128 address spaces; "
              "remaining mappings were not invalidated\n");
  }
}
