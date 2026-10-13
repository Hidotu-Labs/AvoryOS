// Memory Management Syscalls: mmap, munmap, brk
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/numa.h"
#include "../mm/mempolicy.h"
#include "../mm/numa_migrate.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../mm/vmm_internal.h"
#include "../mm/tlb_shootdown.h"
#include "../sched/sched.h"
#include "syscall.h"
#include "sys_io_shared.h"
#include "arch/uaccess.h"
#include "../console/klog.h"
#include <stdint.h>

// LinuxKPI's native preemption pin keeps this per-CPU deferred TLB queue
// drain on the CPU that recorded it while interrupts are enabled.
extern void __kpi_preempt_disable(void);
extern void __kpi_preempt_enable(void);

// Linux mmap constants
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000 /* Linux: fail with EEXIST if range busy */
#define MAP_HUGETLB 0x40000         /* Linux user-space: request huge pages */
/* Linux user-space pthread stack hint, stored verbatim in vma.flags. */
#define MAP_STACK 0x20000
/* Kernel-internal MAP_HUGEPAGE — must match vma.h */
#define MAP_HUGEPAGE 0x200000000ULL /* enable 2MB demand paging for this VMA */

#define MAP_FAILED ((uint64_t)-1)

// Errno constants
#define E_INVAL ((uint64_t)-22)
#define E_NOMEM ((uint64_t)-12)
#define E_BADF ((uint64_t)-9)
#define E_EXIST ((uint64_t)-17)

// Helpers

#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define HHDM_OFFSET pmm_get_hhdm_offset()
#define PML4_PHYS_TO_VIRT(p) ((uint64_t *)(((uint64_t)(p)) + HHDM_OFFSET))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// User-space pointer validation: reject anything above canonical user range.
static inline bool is_user_pointer(uint64_t addr) {
  return addr <= USER_ADDR_MAX;
}

static uint64_t build_page_flags(uint64_t prot) {
  // PROT_NONE → no flags at all (page must stay non-present).
  // On x86-64 there is no "read disable" bit; PRESENT alone grants reads.
  if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == PROT_NONE)
    return 0;

  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & PROT_WRITE)
    flags |= PAGE_FLAG_RW;
  if (!(prot & PROT_EXEC))
    flags |= PAGE_FLAG_NX;
  return flags;
}

// Anonymous mapping
static bool mm_compat32(void) {
  struct thread *t = sched_get_current();
  return t && t->mm && t->mm->compat32;
}
#define MMAP_REGION_BASE (mm_compat32() ? 0x40000000ULL : 0x7F0000000000ULL)
#define MMAP_REGION_LIMIT (mm_compat32() ? 0xB0000000ULL : 0x7FF000000000ULL)

// INTERNAL HELPERS

static void safe_unmap_and_free(uint64_t *pml4, uint64_t va, uint64_t phys,
                                bool free_phys, const char *ctx) {
  (void)ctx;
  // Step 1: remove the PTE.  After this the frame is unreachable via this VA.
  vmm_unmap_page(pml4, va);

  /* Step 2: wait for every CPU that could have cached the old translation
   * before recycling the frame.  vmm_unmap_page() queues its shootdown while
   * interrupts are masked; its lock-release drain can therefore do only the
   * local invalidation and leave remote acknowledgements pending.  In that
   * case keep the frame allocated.  Leaking it is safer than letting another
   * CPU continue writing through a stale TLB entry after the PMM reuses it. */
  bool shootdown_complete = tlb_flush_deferred_drain();
  if (free_phys && phys != 0) {
    if (vmm_virt_to_phys(pml4, va) == 0) {
      if (shootdown_complete)
        pmm_free((void *)phys);
      else
        vmm_defer_frame_free((void *)phys, 1);
    }
  }
}

/* Does this process own the frame at @va (anonymous private mapping)?
 *
 * @mm_locked says whether the caller already holds mm->lock.  The VMA tree is
 * walked under that lock because another thread's mmap()/mprotect() can free
 * the very node we are reading; callers that hold the lock must not have it
 * taken again (these locks are not recursive), and callers that do not hold it
 * must not have it skipped. */
static bool range_page_is_ours(struct thread *t, uint64_t va, bool mm_locked) {
  if (!t || !t->mm)
    return false;

  bool ours;
  if (mm_locked) {
    struct vma *v = vma_find(&t->mm->vmas, va);
    ours = (v && v->fd == -1 && (v->flags & MAP_PRIVATE) &&
            (v->flags & MAP_ANONYMOUS));
  } else {
    spinlock_acquire(&t->mm->lock);
    struct vma *v = vma_find(&t->mm->vmas, va);
    ours = (v && v->fd == -1 && (v->flags & MAP_PRIVATE) &&
            (v->flags & MAP_ANONYMOUS));
    spinlock_release(&t->mm->lock);
  }
  return ours;
}

/* Generic cache views and private memfd views own a reference per PTE.
 * Shared device views retain the backing object's ownership convention. */
static bool vma_has_file_page_refs(struct vma *v) {
  if (!v)
    return false;
  vfs_node_t *node = (vfs_node_t *)v->file_node;
  return (v->flags & MAP_PAGECACHE) ||
         ((v->flags & MAP_PRIVATE) && node && node->mmap_private_pmm_refs);
}

static bool range_page_is_pagecache(struct thread *t, uint64_t va,
                                    bool mm_locked) {
  if (!t || !t->mm)
    return false;

  bool ours;
  if (mm_locked) {
    struct vma *v = vma_find(&t->mm->vmas, va);
    ours = vma_has_file_page_refs(v);
  } else {
    spinlock_acquire(&t->mm->lock);
    struct vma *v = vma_find(&t->mm->vmas, va);
    ours = vma_has_file_page_refs(v);
    spinlock_release(&t->mm->lock);
  }
  return ours;
}

// teardown_range:
//   Unmap [base, base+len) and free anonymous private frames.
//   Used by both MAP_FIXED pre-teardown and munmap proper.
//   Does NOT touch the VMA list — callers manage that themselves.
//
//   @mm_locked: true when the caller holds t->mm->lock. The locked path is
//   batched (one vmm_lock hold, one deferred drain and one empty-table sweep
//   per 64-page chunk, VMA ownership hoisted per interval); the unlocked path
//   keeps the old per-page behavior for callers that cannot hold mm->lock.

// Frames collected under vmm_lock; released after the deferred drain.
#define TEARDOWN_COLLECT 64

static void teardown_release_collected(uint64_t *phys, uint16_t *npages,
                                       int n) {
  bool complete = tlb_flush_deferred_drain();
  for (int i = 0; i < n; i++) {
    if (complete) {
      if (npages[i] == 1)
        pmm_free_page((void *)phys[i]);
      else
        pmm_free_pages((void *)phys[i], npages[i]);
    } else {
      vmm_defer_frame_free((void *)phys[i], npages[i]);
    }
  }
}

// Locked, batched teardown. Caller holds t->mm->lock (VMA tree stable).
static void teardown_range_locked(uint64_t *pml4, struct thread *t,
                                  uint64_t base, uint64_t len) {
  uint64_t end = base + len;
  uint64_t va = base;

  uint64_t fphys[TEARDOWN_COLLECT];
  uint16_t fnpages[TEARDOWN_COLLECT];
  int nf = 0;

  // Empty-table sweep ledger: at most one sweep per touched PT.
  uint64_t sweep_pt = ~(uint64_t)0; // va>>21 key of the open PT
  bool pt_dirty = false;
  uint64_t sweep_rep = 0; // representative VA inside the open PT

  // VMA ownership cache: one lookup per interval instead of two per page.
  struct vma *cv = NULL;
  uint64_t cv_end = base;
  bool ours = false, pagecache = false;

  vmm_lock_acquire();
  // Flush collected frames + sweep the open PT. Drops and re-takes vmm_lock
  // around the drain (the drain must run off-lock).
  while (va < end) {
    if (va >= cv_end) {
      cv = vma_find(&t->mm->vmas, va);
      if (cv) {
        cv_end = cv->end;
        ours = (cv->fd == -1 && (cv->flags & MAP_PRIVATE) &&
                (cv->flags & MAP_ANONYMOUS));
        pagecache = vma_has_file_page_refs(cv);
      } else {
        // Gap: unmap without freeing; re-evaluate on the next page. Gaps
        // are rare in teardown ranges, so one lookup per gap page is fine.
        cv_end = va + PAGE_SIZE;
        ours = false;
        pagecache = false;
      }
    }

    uint64_t frame = 0;
    vmm_unmap_kind_t kind = vmm_unmap_page_locked(pml4, va, &frame);
    if (kind == VMM_UNMAP_4K) {
      uint64_t ptk = va >> 21;
      if (ptk != sweep_pt) {
        if (pt_dirty)
          vmm_free_empty_tables(pml4, sweep_rep);
        sweep_pt = ptk;
        pt_dirty = false;
      }
      pt_dirty = true;
      sweep_rep = va;
      if ((ours || pagecache) && frame != 0) {
        fphys[nf] = frame;
        fnpages[nf] = 1;
        nf++;
      }
      va += PAGE_SIZE;
    } else if (kind == VMM_UNMAP_HOLE) {
      va += PAGE_SIZE;
    } else {
      // 2 MB PDE. Bulk-release only the whole-block-owned case inline (the
      // same rule as the old per-page path); anything else is rare enough
      // for the slow path below.
#define HUGE_2MB (2ULL * 1024 * 1024)
      uint64_t block = va & ~(HUGE_2MB - 1);
      uint64_t block_end = block + HUGE_2MB;
#undef HUGE_2MB
      if (va == block && block_end <= end && ours) {
        uint64_t huge_base = 0;
        vmm_unmap_huge_locked(pml4, va, &huge_base);
        if (pt_dirty) {
          vmm_free_empty_tables(pml4, sweep_rep);
          pt_dirty = false;
        }
        sweep_pt = ~(uint64_t)0;
        if (huge_base != 0) {
          fphys[nf] = huge_base;
          fnpages[nf] = 512;
          nf++;
        }
        va = block_end;
        // Fall through to the capacity check below.
      } else {
        // Partial/shared huge mapping: split it on the slow path, then
        // resume the batch on 4 KB granularity.
        if (pt_dirty) {
          vmm_free_empty_tables(pml4, sweep_rep);
          pt_dirty = false;
        }
        sweep_pt = ~(uint64_t)0;
        vmm_lock_release();
        teardown_release_collected(fphys, fnpages, nf);
        nf = 0;
        vmm_drain_deferred_work();
        if (!vmm_split_huge_page(pml4, va)) {
          // OOM: keep the mapping intact rather than free live frames.
          va += PAGE_SIZE;
        }
        // Re-resolve the VMA interval (split does not touch VMAs, but the
        // lock was dropped and another thread may have edited the tree).
        cv_end = va;
        vmm_lock_acquire();
        continue;
      }
    }

    if (nf >= TEARDOWN_COLLECT - 1) {
      if (pt_dirty) {
        vmm_free_empty_tables(pml4, sweep_rep);
        pt_dirty = false;
      }
      sweep_pt = ~(uint64_t)0;
      vmm_lock_release();
      teardown_release_collected(fphys, fnpages, nf);
      nf = 0;
      vmm_drain_deferred_work();
      // VMA tree may have changed while mm->lock was... (it wasn't: the
      // caller holds mm->lock throughout. Only vmm_lock was dropped, so
      // the interval cache stays valid.)
      vmm_lock_acquire();
    }
  }
  if (pt_dirty)
    vmm_free_empty_tables(pml4, sweep_rep);
  vmm_lock_release();

  teardown_release_collected(fphys, fnpages, nf);
  // Release page-table frames parked by the sweep above.
  vmm_drain_deferred_work();
}
static void teardown_range(uint64_t *pml4, struct thread *t, uint64_t base,
                           uint64_t len, bool mm_locked, const char *ctx) {
  (void)ctx;
  if (len == 0)
    return;
  if (mm_locked) {
    teardown_range_locked(pml4, t, base, len);
    return;
  }
  uint64_t end = base + len;

  for (uint64_t va = base; va < end; va += PAGE_SIZE) {
    uint64_t phys = vmm_virt_to_phys(pml4, va);
    if (phys == 0)
      continue; // Already unmapped, nothing to do.

    phys = PAGE_ALIGN_DOWN(phys); // Strip low flag bits VMM may leave set.

    // Detect a 2 MB PS-bit huge page: the VA is 2MB-aligned and phys is
    // the base of the huge block (no intra-page offset).
#define HUGE_2MB (2ULL * 1024 * 1024)
    if (vmm_is_huge_page(pml4, va)) {
      uint64_t block = va & ~(HUGE_2MB - 1);
      uint64_t block_end = block + HUGE_2MB;

      /* Bulk release only when the whole 2 MB block is going away *and* we own
       * the frames in it.  vmm_unmap_page() releases all 512 frames without
       * asking anybody, so a file-backed or MAP_SHARED huge mapping has to go
       * down the split path below and be released page by page with
       * free_phys == false. */
      if (va == block && block_end <= end &&
          range_page_is_ours(t, va, mm_locked)) {
        // vmm_unmap_page clears the PDE and frees all 512 constituent frames
        // via pmm_free_pages in one shot.  Skip forward past the sub-page VAs
        // we just released.
        vmm_unmap_page(pml4, va);
        va = block_end - PAGE_SIZE;
        continue;
      }

      /* Only part of the huge mapping is inside [base, end).  Clearing the PDE
       * here would release frames the caller still owns - munmap(base + 0x1000,
       * 0x1000) used to hand 2 MB back to the PMM while the process kept using
       * it, and those frames were then handed to somebody else.  Split the
       * block into 4 KB pages and let the per-page path below free just the
       * requested ones. */
      if (!vmm_split_huge_page(pml4, va))
        continue; // OOM: keep the mapping intact rather than free live frames
    }
#undef HUGE_2MB


    // Only free frames we own: anonymous private mappings, plus page-cache
    // file frames (pmm_free_page() is a decref for those).
    bool free_phys = range_page_is_ours(t, va, mm_locked) ||
                     range_page_is_pagecache(t, va, mm_locked);

    safe_unmap_and_free(pml4, va, phys, free_phys, ctx);
  }
}


// sys_mmap
// Linux ABI: mmap(addr, length, prot, flags, fd, offset)
//   rdi=addr  rsi=length  rdx=prot  r10=flags  r8=fd  r9=offset
uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags,
                  uint64_t fd, uint64_t offset) {
  (void)offset;

  /*
    klog_puts("[MMAP] addr=");
    klog_uint64(addr);
    klog_puts(" len=");
    klog_uint64(length);
    klog_puts(" prot=0x");
    klog_uint64(prot);
    klog_puts(" flags=0x");
    klog_uint64(flags);
    klog_puts("\n");
  */

  // Validate prot and flags
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE | 0x01000000 | 0x02000000)) != 0) {
    return E_INVAL;
  }

  uint32_t map_type = (uint32_t)(flags & 0x0F);
  if (map_type != MAP_SHARED && map_type != MAP_PRIVATE && map_type != 0x03 /* MAP_SHARED_VALIDATE */) {
    return E_INVAL;
  }

  /* Kernel-internal VMA bits are never accepted from user space. */
  flags &= ~MAP_PAGECACHE;

  if (length == 0) {
    return E_INVAL;
  }

  // Determine virtual address
  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  if (aligned_len == 0 || aligned_len - 1 > USER_ADDR_MAX)
    return E_NOMEM;
  struct thread *current_thread = sched_get_current();

  if (mm_compat32() &&
      (addr >= 0xC0000000ULL || aligned_len > 0xC0000000ULL - addr))
    return E_NOMEM;

  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t vaddr = 0;

  bool fixed_noreplace = (flags & MAP_FIXED_NOREPLACE) != 0;
  bool fixed = (flags & MAP_FIXED) != 0;
  if (fixed && fixed_noreplace)
    return E_INVAL;

  if (fixed || fixed_noreplace) {
    if (addr == 0 || (addr & (PAGE_SIZE - 1))) {
      return E_INVAL;
    }
    if (!is_user_pointer(addr) || aligned_len - 1 > USER_ADDR_MAX - addr) {
      return E_NOMEM;
    }
    vaddr = addr;

    if (current_thread && current_thread->mm) {
      spinlock_acquire(&current_thread->mm->lock);
      if (fixed_noreplace &&
          vma_find_overlap(&current_thread->mm->vmas, vaddr,
                           vaddr + aligned_len)) {
        spinlock_release(&current_thread->mm->lock);
        return E_EXIST;
      }
      /* Serialize the PTE teardown with demand faults and remove the old VMA
       * before dropping mm->lock.  TLB waits are deferred while this lock
       * masks interrupts, then drained below after faults can run again. */
      teardown_range(pml4, current_thread, vaddr, aligned_len, true,
                     "MAP_FIXED teardown");
      vma_remove(&current_thread->mm->vmas, vaddr, vaddr + aligned_len);
      if (flags & MAP_ANONYMOUS) {
        /* Install fixed anonymous mappings atomically with the overlap check
         * and teardown.  The VMA itself reserves the range; moving the normal
         * mmap cursor here would discard every free gap below a high fixed
         * reservation (Wine reserves the top of user space this way). */
        uint64_t vma_flags = flags;
        if (flags & MAP_HUGETLB)
          vma_flags |= MAP_HUGEPAGE;
        int result = vma_add(&current_thread->mm->vmas, vaddr,
                             vaddr + aligned_len, prot, vma_flags,
                             -1, 0, NULL, 0);
        spinlock_release(&current_thread->mm->lock);
        vmm_drain_deferred_work();
        if (result < 0)
          return E_NOMEM;
        return vaddr;
      }
      /* Keep non-fixed mmap reservations above this range while a
       * file-backed MAP_FIXED operation finishes outside mm->lock. */
      if (vaddr < MMAP_REGION_LIMIT &&
          vaddr + aligned_len > MMAP_REGION_BASE) {
        uint64_t reservation_end = vaddr + aligned_len;
        if (reservation_end > MMAP_REGION_LIMIT)
          reservation_end = MMAP_REGION_LIMIT;
        current_thread->mm->mmap_next_addr =
            MAX(current_thread->mm->mmap_next_addr,
                reservation_end);
      }
      spinlock_release(&current_thread->mm->lock);
      vmm_drain_deferred_work();
    } else {
      teardown_range(pml4, current_thread, vaddr, aligned_len, false,
                     "MAP_FIXED teardown");
    }
  } else {
    // Non-fixed: allocate dynamically utilizing AVL Interval Gap Finding
    if (current_thread && current_thread->mm) {
      spinlock_acquire(&current_thread->mm->lock);

      /* Non-fixed mappings are reserved by advancing mmap_next_addr while
       * holding mm->lock.  File-backed mmap handlers run after that lock is
       * dropped, before their VMA is installed; searching below the high-water
       * mark could hand the same gap to another thread during that window. */
      uint64_t search_base = MAX(MMAP_REGION_BASE,
                                 current_thread->mm->mmap_next_addr);

      // Try addr as a hint if provided and aligned
      if (addr != 0 && (addr & (PAGE_SIZE - 1)) == 0 &&
          is_user_pointer(addr) && is_user_pointer(addr + aligned_len - 1) &&
          addr >= search_base && addr + aligned_len <= MMAP_REGION_LIMIT) {
        if (!vma_find_overlap(&current_thread->mm->vmas, addr, addr + aligned_len)) {
          vaddr = addr;
        }
      }

      if (vaddr == 0) {
        vaddr = vma_find_gap(&current_thread->mm->vmas, aligned_len,
                             search_base, MMAP_REGION_LIMIT);
      }

      if (vaddr && vaddr + aligned_len <= MMAP_REGION_LIMIT &&
          (flags & MAP_ANONYMOUS)) {
        uint64_t vma_flags = flags;
        if (flags & MAP_HUGETLB)
          vma_flags |= MAP_HUGEPAGE;
        int vma_idx = vma_add(&current_thread->mm->vmas, vaddr, vaddr + aligned_len,
                              prot, vma_flags, -1, 0, NULL, 0);
        if (vma_idx < 0) {
          spinlock_release(&current_thread->mm->lock);
          return E_NOMEM;
        }
        current_thread->mm->mmap_next_addr =
            MAX(current_thread->mm->mmap_next_addr, vaddr + aligned_len);
        spinlock_release(&current_thread->mm->lock);
        return vaddr;
      }

      if (!vaddr || vaddr + aligned_len > MMAP_REGION_LIMIT)
        vaddr = 0;
      else
        /* Reserve file-backed ranges before releasing mm->lock.  A failed
         * mmap may leave a harmless hole in this monotonic allocation cursor. */
        current_thread->mm->mmap_next_addr =
            MAX(current_thread->mm->mmap_next_addr, vaddr + aligned_len);
      spinlock_release(&current_thread->mm->lock);
    }

    if (vaddr == 0) {
      // Fallback to legacy allocator if AVL gap finding fails or thread context missing
      vaddr = mm_alloc_mmap_region(aligned_len);
    }

    if (vaddr == 0 || vaddr + aligned_len > MMAP_REGION_LIMIT) {
      return E_NOMEM;
    }
  }

  // File-backed mapping
  if (!(flags & MAP_ANONYMOUS) && (int64_t)fd != -1) {
    vfs_node_t *node __attribute__((cleanup(fd_node_put))) =
        fd_node_get(current_thread, fd);
    if (!node)
      return E_BADF;

    if (node->mmap &&
        !(node->mmap_shared_only && ((flags & 0x3) == MAP_PRIVATE))) {
      // Pass MAP_FIXED to internal handler to ensure it respects our vaddr
      uint64_t result =
          node->mmap(node, vaddr, length, prot, flags | MAP_FIXED, offset);
      if (result >= (uint64_t)-4095) {
        /* A failed Linux f_op->mmap must not leave a pending bridge wrapper. */
        extern void *linuxkpi_vma_take_pending(void) __attribute__((weak));
        extern void linuxkpi_vma_unref(void *) __attribute__((weak));
        if (linuxkpi_vma_take_pending && linuxkpi_vma_unref) {
          void *stale = linuxkpi_vma_take_pending();
          if (stale)
            linuxkpi_vma_unref(stale);
        }
        return result == MAP_FAILED ? E_NOMEM : result;
      }

      if (current_thread && current_thread->mm) {
        spinlock_acquire(&current_thread->mm->lock);
        int vma_idx =
            vma_add(&current_thread->mm->vmas, result, result + aligned_len, prot,
                    flags, (int)fd, offset, node, 0);
        spinlock_release(&current_thread->mm->lock);
        if (vma_idx < 0) {
          extern void *linuxkpi_vma_take_pending(void) __attribute__((weak));
          extern void linuxkpi_vma_unref(void *) __attribute__((weak));
          if (linuxkpi_vma_take_pending && linuxkpi_vma_unref) {
            void *stale = linuxkpi_vma_take_pending();
            if (stale)
              linuxkpi_vma_unref(stale);
          }
          return E_NOMEM;
        }

        /* If this was a Linux f_op->mmap() (imported driver), bind the
         * Linux-facing vm_area_struct to the native VMA so vm_ops->close()
         * runs when the last reference goes away.  vma_attach_linux() takes
         * its own reference; the bridge's pending creator reference is
         * released here so a full munmap drops the last one and closes the
         * wrapper exactly once. */
        extern void *linuxkpi_vma_take_pending(void) __attribute__((weak));
        extern void linuxkpi_vma_unref(void *) __attribute__((weak));
        if (linuxkpi_vma_take_pending) {
          void *lvma = linuxkpi_vma_take_pending();
          if (lvma) {
            spinlock_acquire(&current_thread->mm->lock);
            vma_attach_linux(&current_thread->mm->vmas, result, lvma);
            spinlock_release(&current_thread->mm->lock);
            if (linuxkpi_vma_unref)
              linuxkpi_vma_unref(lvma);
          }
        }
      }

      return result;
    }

    /* Regular file without a driver mmap handler: demand-page it through the
     * VMA page-cache path.  The fault handler already serves these (shared and
     * private, read-only and writable), so this is what makes file mmap work
     * for SQLite WAL, QFile::map, and friends. */
    if ((node->flags & FS_TYPE_MASK) != FS_FILE)
      return E_INVAL;
    if (offset & (PAGE_SIZE - 1))
      return E_INVAL;
    if (!current_thread->mm)
      return E_NOMEM;

    spinlock_acquire(&current_thread->mm->lock);
    int vma_idx = vma_add(&current_thread->mm->vmas, vaddr,
                          vaddr + aligned_len, prot,
                          flags | MAP_PAGECACHE, (int)fd, offset, node, 0);
    spinlock_release(&current_thread->mm->lock);
    if (vma_idx < 0)
      return E_NOMEM;
    return vaddr;
  }

  // Reject non-anonymous mappings with no fd
  if (!(flags & MAP_ANONYMOUS)) {
    return E_BADF;
  }

  // Anonymous mapping (Fixed path or legacy fallback)
  if (current_thread && current_thread->mm) {
    spinlock_acquire(&current_thread->mm->lock);
    uint64_t vma_flags = flags;
    if (flags & MAP_HUGETLB)
      vma_flags |= MAP_HUGEPAGE;
    int vma_idx = vma_add(&current_thread->mm->vmas, vaddr, vaddr + aligned_len,
                          prot, vma_flags, -1, 0, NULL, 0);
    if (vma_idx < 0) {
      spinlock_release(&current_thread->mm->lock);
      return E_NOMEM;
    }

    current_thread->mm->mmap_next_addr =
        MAX(current_thread->mm->mmap_next_addr, vaddr + aligned_len);
    spinlock_release(&current_thread->mm->lock);
  }

  return vaddr;
}

// sys_munmap
// Linux ABI: munmap(addr, length)
//   rdi=addr  rsi=length
#define MUNMAP_BATCH_PAGES 32
uint64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  // Validate arguments
  if (addr == 0 || length == 0)
    return E_INVAL;
  if (addr & (PAGE_SIZE - 1))
    return E_INVAL; // addr must be page-aligned (POSIX).
  if (!is_user_pointer(addr) || !is_user_pointer(addr + length - 1))
    return E_INVAL;

  struct thread *current = sched_get_current();
  if (!current)
    return E_INVAL;


  // Linux munmap is permissive: it unmaps whatever is in the range.
  // It does not require a VMA to exist at 'addr'.

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t end = addr + aligned_len;

  spinlock_acquire(&current->mm->lock);
  /* Reserve the range as inaccessible before dropping mm->lock.  That stops
   * page faults from reinstalling PTEs and keeps mmap's gap search from reusing
   * the address while teardown runs in bounded batches. */
  vma_mprotect(&current->mm->vmas, addr, end, PROT_NONE);
  spinlock_release(&current->mm->lock);

  uint64_t batch_bytes = (uint64_t)MUNMAP_BATCH_PAGES * PAGE_SIZE;
  for (uint64_t batch_start = addr; batch_start < end;) {
    uint64_t batch_end = batch_start + batch_bytes;
    if (batch_end < batch_start || batch_end > end)
      batch_end = end;

    spinlock_acquire(&current->mm->lock);
    teardown_range(pml4, current, batch_start, batch_end - batch_start, true,
                   "sys_munmap");
    spinlock_release(&current->mm->lock);
    /* The per-CPU deferred queues are deliberately bounded.  Drain before
     * starting another batch so no page frames are stranded on overflow and
     * remote CPUs can acknowledge between short mm critical sections. */
    vmm_drain_deferred_work();
    batch_start = batch_end;
  }

  spinlock_acquire(&current->mm->lock);
  vma_remove(&current->mm->vmas, addr, end);
  vma_merge_adjacent(&current->mm->vmas);
  spinlock_release(&current->mm->lock);
  vmm_drain_deferred_work();

  return 0;
}

// sys_brk  (unchanged from original — included for completeness)
// Linux ABI: brk(addr)  —  rdi=addr
// Returns the current/new program break.
static uint64_t sys_brk(uint64_t addr, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return 0;


  // Linux ABI: brk(0) returns current break.
  spinlock_acquire(&current->mm->lock);
  if (addr == 0) {
    uint64_t ret = current->mm->brk_current;
    spinlock_release(&current->mm->lock);
    return ret;
  }

  // Check bounds.
  if (!is_user_pointer(addr) ||
      (current->mm->compat32 && addr >= 0xC0000000ULL)) {
    uint64_t ret = current->mm->brk_current;
    spinlock_release(&current->mm->lock);
    return ret;
  }

  uint64_t *pml4 = vmm_get_active_pml4();

  if (addr > current->mm->brk_current) {
    uint64_t old_end = PAGE_ALIGN_UP(current->mm->brk_current);
    uint64_t new_end = PAGE_ALIGN_UP(addr);

    if (new_end > old_end) {
      if (vma_find_overlap(&current->mm->vmas, old_end, new_end)) {
        spinlock_release(&current->mm->lock);
        return current->mm->brk_current; // Overlap with existing VMA
      }
      if (vma_add(&current->mm->vmas, old_end, new_end, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, NULL, 0) < 0) {
        spinlock_release(&current->mm->lock);
        return current->mm->brk_current;
      }
    }

    current->mm->brk_current = addr;

  } else if (addr < current->mm->brk_current && addr >= current->mm->brk_base) {
    uint64_t old_end = PAGE_ALIGN_UP(current->mm->brk_current);
    uint64_t new_end = PAGE_ALIGN_UP(addr);

    for (uint64_t page = new_end; page < old_end; page += PAGE_SIZE) {
      uint64_t phys = vmm_virt_to_phys(pml4, page);
      if (phys != 0) {
        phys = PAGE_ALIGN_DOWN(phys);
        safe_unmap_and_free(pml4, page, phys, true, "sys_brk shrink");
      }
    }

    vma_remove(&current->mm->vmas, new_end, old_end);
    current->mm->brk_current = addr;
  }

  uint64_t ret = current->mm->brk_current;
  spinlock_release(&current->mm->lock);

  return ret;
}

// sys_mprotect
// Linux ABI: mprotect(addr, len, prot)
#define MPROTECT_BATCH_PAGES 64
static uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (addr == 0 || (addr & (PAGE_SIZE - 1)))
    return (addr == 0) ? E_NOMEM : E_INVAL;
  if (len == 0)
    return 0;

  uint64_t aligned_len = PAGE_ALIGN_UP(len);
  if (addr + aligned_len < addr || !is_user_pointer(addr) ||
      !is_user_pointer(addr + aligned_len - 1))
    return E_NOMEM;

  uint64_t *pml4 = vmm_get_active_pml4();

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return E_INVAL;



  uint64_t end = addr + aligned_len;
  for (uint64_t batch_start = addr; batch_start < end;) {
    uint64_t batch_end = batch_start +
        (uint64_t)MPROTECT_BATCH_PAGES * PAGE_SIZE;
    if (batch_end < batch_start || batch_end > end)
      batch_end = end;

    spinlock_acquire(&current->mm->lock);
    vmm_lock_acquire();
    bool ok = true;
    bool touched = false;

    for (uint64_t va = batch_start; va < batch_end; va += PAGE_SIZE) {
      uint64_t phys = vmm_virt_to_phys(pml4, va);
      if (phys == 0)
        continue;

      uint64_t new_flags = build_page_flags(prot);
      if (prot == PROT_NONE) {
        // Keep the PTE present so teardown_range / vmm_virt_to_phys can still
        // find this physical frame at munmap time. Omitting PAGE_FLAG_USER
        // gives ring 3 the expected protection fault.
        new_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_NX;
      } else if (prot & PROT_WRITE) {
        struct vma *v = vma_find(&current->mm->vmas, va);
        if (PAGE_ALIGN_DOWN(phys) == pmm_get_zero_page_phys() ||
            (v && (v->flags & MAP_PRIVATE) && v->file_node != NULL)) {
          // Shared page-cache and zero-page frames stay read-only until COW.
          new_flags = (new_flags & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
        }
      }
      bool changed = false;
      if (!vmm_map_page_locked(pml4, va, PAGE_ALIGN_DOWN(phys), new_flags,
                               &changed)) {
        ok = false;
        break;
      }
      touched |= changed;
    }

    if (ok) {
      // Publish each bounded range's VMA permissions while the same mm lock
      // still excludes concurrent faults and mapping changes.
      vma_mprotect(&current->mm->vmas, batch_start, batch_end, prot);
      vma_merge_adjacent(&current->mm->vmas);
    }

    // Deferred requests live in a per-CPU queue. Pin this thread across the
    // unlock-to-drain window so a timer tick cannot migrate it and strand the
    // queue on the CPU that performed the page-table writes.
    __kpi_preempt_disable();
    if (touched)
      tlb_flush_deferred_context((uint64_t)pml4);
    vmm_lock_release();
    spinlock_release(&current->mm->lock);

    // The VMM lock's automatic drain sees IF=0 while mm->lock is held and
    // deliberately defers remote waits. Drain now that peers can take IPIs;
    // one address-space flush covers the whole batch's changed PTEs.
    if (touched)
      (void)tlb_flush_deferred_drain();
    __kpi_preempt_enable();
    if (!ok)
      return E_NOMEM;

    batch_start = batch_end;
  }

  return 0;
}

// sys_mremap
// Linux ABI: mremap(old_addr, old_size, new_size, flags, [new_addr])
//   rdi=old_addr  rsi=old_size  rdx=new_size  r10=flags  r8=new_addr
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

static uint64_t sys_mremap(uint64_t old_addr, uint64_t old_size,
                           uint64_t new_size, uint64_t flags,
                           uint64_t new_addr_hint, uint64_t a5) {
  (void)new_addr_hint;
  (void)a5;

  if (old_addr == 0 || old_size == 0 || new_size == 0)
    return E_INVAL;
  if (old_addr & (PAGE_SIZE - 1))
    return E_INVAL;
  if (!is_user_pointer(old_addr))
    return E_INVAL;

  // We only support MREMAP_MAYMOVE for now (the common musl realloc path).
  // MREMAP_FIXED is rarely used and complex to implement.
  if (flags & MREMAP_FIXED)
    return E_INVAL;

  struct thread *current = sched_get_current();
  if (!current)
    return E_INVAL;

  uint64_t aligned_old = PAGE_ALIGN_UP(old_size);
  uint64_t aligned_new = PAGE_ALIGN_UP(new_size);
  uint64_t *pml4 = vmm_get_active_pml4();

  spinlock_acquire(&current->mm->lock);

  /* Upstream rejects any mremap of a VM_DONTEXPAND VMA (GEM and dma-buf
   * mappings set it).  The native tree has no such flag, so ask the bridge
   * wrapper; this also keeps those mappings out of the remove/re-add paths
   * below, exactly like upstream. */
  struct vma *orig_vma = vma_find(&current->mm->vmas, old_addr);
  if (orig_vma && orig_vma->linux_vma) {
    extern bool linuxkpi_vma_no_expand(void *) __attribute__((weak));
    if (linuxkpi_vma_no_expand && linuxkpi_vma_no_expand(orig_vma->linux_vma)) {
      spinlock_release(&current->mm->lock);
      return E_INVAL;
    }
  }

  // Shrink: just unmap the tail pages and update the VMA.
  if (aligned_new <= aligned_old) {
    if (aligned_new < aligned_old) {
      uint64_t trim_base = old_addr + aligned_new;
      uint64_t trim_len = aligned_old - aligned_new;
      teardown_range(pml4, current, trim_base, trim_len, true,
                     "mremap shrink");
      vma_remove(&current->mm->vmas, trim_base, trim_base + trim_len);
    }
    spinlock_release(&current->mm->lock);
    return old_addr;
  }

  // Grow: try in-place first — check if pages right after old region are free.
  uint64_t grow_base = old_addr + aligned_old;
  uint64_t grow_len = aligned_new - aligned_old;
  bool can_grow_inplace = true;

  // Check that the expansion area doesn't overlap any existing VMA.
  if (vma_find_overlap(&current->mm->vmas, grow_base, grow_base + grow_len)) {
    can_grow_inplace = false;
  }

  // Also verify no existing page mappings in the expansion zone.
  if (can_grow_inplace) {
    for (uint64_t va = grow_base; va < grow_base + grow_len; va += PAGE_SIZE) {
      if (vmm_virt_to_phys(pml4, va) != 0) {
        can_grow_inplace = false;
        break;
      }
    }
  }

  // The original VMA was looked up above for the VM_DONTEXPAND check; the
  // shrink path returned already, so it is still valid here.
  if (!orig_vma) {
    spinlock_release(&current->mm->lock);
    return E_INVAL;
  }
  uint64_t prot = orig_vma->prot;
  uint64_t vma_flags = orig_vma->flags;
  int vma_fd = orig_vma->fd;
  uint64_t vma_offset = orig_vma->offset;
  vfs_node_t *vma_file = (vfs_node_t *)orig_vma->file_node;
  uint64_t page_flags = build_page_flags(prot);

  /* A Linux f_op->mmap() call leaves its bridge wrapper pending on this
   * thread; the remove/re-add paths below must attach it to the new native
   * VMA, or vm_ops->close() is never tied to this mapping (P2 gap). */
  extern void *linuxkpi_vma_take_pending(void) __attribute__((weak));
  extern void linuxkpi_vma_unref(void *) __attribute__((weak));
  extern void linuxkpi_vma_rebase(void *, unsigned long, unsigned long,
                                  unsigned long) __attribute__((weak));

  if (can_grow_inplace) {
    /* Preserve the backing object when a shared file mapping grows.
     * wl_shm_pool.resize() relies on the new pages viewing the same memfd;
     * anonymous zero pages here make every buffer beyond the old size blank. */
    if ((vma_flags & MAP_SHARED) && vma_file && vma_file->mmap) {
      uint64_t mapped =
          vma_file->mmap(vma_file, grow_base, grow_len, prot,
                         vma_flags | MAP_FIXED, vma_offset + aligned_old);
      if (mapped != grow_base) {
        spinlock_release(&current->mm->lock);
        return E_NOMEM;
      }

      /* The f_op->mmap() above created a wrapper covering only the new
       * pages; make the re-added node own it and rebase it onto the whole
       * mapping so fault-time pgoff stays at vma_offset. */
      void *new_lvma =
          linuxkpi_vma_take_pending ? linuxkpi_vma_take_pending() : NULL;

      /* Keep the file alive while replacing the old VMA reference. */
      vfs_node_ref(vma_file);
      vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);
      int add_ret =
          vma_add(&current->mm->vmas, old_addr, old_addr + aligned_new, prot,
                  vma_flags, vma_fd, vma_offset, vma_file, 0);
      if (add_ret == 0 && new_lvma) {
        if (linuxkpi_vma_rebase)
          linuxkpi_vma_rebase(new_lvma, old_addr, old_addr + aligned_new,
                              vma_offset);
        vma_attach_linux(&current->mm->vmas, old_addr, new_lvma);
      }
      if (new_lvma && linuxkpi_vma_unref)
        linuxkpi_vma_unref(new_lvma);
      vfs_close(vma_file);
      if (add_ret < 0) {
        spinlock_release(&current->mm->lock);
        return E_NOMEM;
      }

      spinlock_release(&current->mm->lock);
      return old_addr;
    }

    // Allocate and map the new pages.
    for (uint64_t va = grow_base; va < grow_base + grow_len; va += PAGE_SIZE) {
      void *phys = pmm_alloc();
      if (!phys) {
        spinlock_release(&current->mm->lock);
        return E_NOMEM; // OOM — leave partial state; not ideal but safe.
      }
      if (!vmm_map_page(pml4, va, (uint64_t)phys, page_flags)) {
        pmm_free(phys);
        spinlock_release(&current->mm->lock);
        return E_NOMEM;
      }
      void *kva = (void *)((uint64_t)phys + HHDM_OFFSET);
      memset(kva, 0, PAGE_SIZE);
    }
    // Extend the VMA to cover the new range, keeping any bridge wrapper.
    void *keep_lvma = orig_vma->linux_vma;
    vma_linux_get(keep_lvma);
    vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);
    vma_add(&current->mm->vmas, old_addr, old_addr + aligned_new, prot,
            vma_flags, -1, 0, NULL, 0);
    if (keep_lvma && linuxkpi_vma_rebase)
      linuxkpi_vma_rebase(keep_lvma, old_addr, old_addr + aligned_new,
                          vma_offset);
    vma_attach_linux(&current->mm->vmas, old_addr, keep_lvma);
    vma_linux_put(keep_lvma);
    spinlock_release(&current->mm->lock);
    return old_addr;
  }

  // Cannot grow in-place.  If MAYMOVE is allowed, allocate a new region,
  // copy old data, and unmap the old region.
  if (!(flags & MREMAP_MAYMOVE)) {
    spinlock_release(&current->mm->lock);
    return E_NOMEM; // Actually Linux returns ENOMEM here if it can't grow in-place and MAYMOVE not set.
  }

  uint64_t new_addr = vma_find_gap(&current->mm->vmas, aligned_new,
                                   MMAP_REGION_BASE, MMAP_REGION_LIMIT);
  if (new_addr == 0 || new_addr + aligned_new > MMAP_REGION_LIMIT) {
    spinlock_release(&current->mm->lock);
    return E_NOMEM;
  }

  /* A moved shared file mapping must remain a view of the same file, not
   * become an anonymous copy. This is the relocation path for a Wayland SHM
   * pool which cannot expand at its current virtual address. */
  if ((vma_flags & MAP_SHARED) && vma_file && vma_file->mmap) {
    uint64_t mapped =
        vma_file->mmap(vma_file, new_addr, aligned_new, prot,
                       vma_flags | MAP_FIXED, vma_offset);
    if (mapped != new_addr) {
      spinlock_release(&current->mm->lock);
      return E_NOMEM;
    }

    /* The f_op->mmap() above created the wrapper for the new address; the
     * re-added node owns it (the old wrapper closes with the removed node). */
    void *new_lvma =
        linuxkpi_vma_take_pending ? linuxkpi_vma_take_pending() : NULL;

    vfs_node_ref(vma_file);
    teardown_range(pml4, current, old_addr, aligned_old, true,
                   "mremap file move");
    vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);
    int add_ret =
        vma_add(&current->mm->vmas, new_addr, new_addr + aligned_new, prot,
                vma_flags, vma_fd, vma_offset, vma_file, 0);
    if (add_ret == 0 && new_lvma) {
      if (linuxkpi_vma_rebase)
        linuxkpi_vma_rebase(new_lvma, new_addr, new_addr + aligned_new,
                            vma_offset);
      vma_attach_linux(&current->mm->vmas, new_addr, new_lvma);
    }
    if (new_lvma && linuxkpi_vma_unref)
      linuxkpi_vma_unref(new_lvma);
    vfs_close(vma_file);
    if (add_ret < 0) {
      spinlock_release(&current->mm->lock);
      return E_NOMEM;
    }

    current->mm->mmap_next_addr =
        MAX(current->mm->mmap_next_addr, new_addr + aligned_new);
    spinlock_release(&current->mm->lock);
    return new_addr;
  }

  // Allocate and map new pages.
  for (uint64_t i = 0; i < aligned_new / PAGE_SIZE; i++) {
    void *phys = pmm_alloc();
    if (!phys) {
      spinlock_release(&current->mm->lock);
      return E_NOMEM;
    }
    if (!vmm_map_page(pml4, new_addr + i * PAGE_SIZE, (uint64_t)phys,
                      page_flags)) {
      pmm_free(phys);
      spinlock_release(&current->mm->lock);
      return E_NOMEM;
    }
    void *kva = (void *)((uint64_t)phys + HHDM_OFFSET);
    memset(kva, 0, PAGE_SIZE);
  }

  // Copy old data to new location (page by page through HHDM).
  for (uint64_t off = 0; off < aligned_old; off += PAGE_SIZE) {
    uint64_t old_phys = vmm_virt_to_phys(pml4, old_addr + off);
    uint64_t new_phys = vmm_virt_to_phys(pml4, new_addr + off);
    if (old_phys && new_phys) {
      void *src = (void *)(PAGE_ALIGN_DOWN(old_phys) + HHDM_OFFSET);
      void *dst = (void *)(PAGE_ALIGN_DOWN(new_phys) + HHDM_OFFSET);
      memcpy(dst, src, PAGE_SIZE);
    }
  }

  // Tear down old mapping, keeping any bridge wrapper for the new node.
  void *keep_lvma = orig_vma->linux_vma;
  vma_linux_get(keep_lvma);
  teardown_range(pml4, current, old_addr, aligned_old, true, "mremap move");
  vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);

  // Register new VMA and hand it the preserved wrapper.
  vma_add(&current->mm->vmas, new_addr, new_addr + aligned_new, prot, vma_flags,
          -1, 0, NULL, 0);
  if (keep_lvma && linuxkpi_vma_rebase)
    linuxkpi_vma_rebase(keep_lvma, new_addr, new_addr + aligned_new,
                        vma_offset);
  vma_attach_linux(&current->mm->vmas, new_addr, keep_lvma);
  vma_linux_put(keep_lvma);

  current->mm->mmap_next_addr =
      MAX(current->mm->mmap_next_addr, new_addr + aligned_new);

  spinlock_release(&current->mm->lock);
  return new_addr;
}

// Linux madvise advice values
#define MADV_NORMAL      0
#define MADV_RANDOM      1
#define MADV_SEQUENTIAL  2
#define MADV_WILLNEED    3
#define MADV_DONTNEED    4
#define MADV_FREE        8
#define MADV_REMOVE      9
#define MADV_DONTFORK    10
#define MADV_DOFORK      11
#define MADV_MERGEABLE   12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE    14
#define MADV_NOHUGEPAGE  15
#define MADV_DONTDUMP    16
#define MADV_DODUMP      17
#define MADV_WIPEONFORK  18
#define MADV_KEEPONFORK  19
#define MADV_COLD        20
#define MADV_PAGEOUT     21

static uint64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;

  if (addr & (PAGE_SIZE - 1))
    return (uint64_t)-22; // EINVAL — must be page-aligned

  if (len == 0)
    return 0;

  uint64_t aligned_len = PAGE_ALIGN_UP(len);
  uint64_t end = addr + aligned_len;

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return 0;

  spinlock_acquire(&current->mm->lock);

  // MADV_DONTNEED / MADV_REMOVE:
  // Discard physical pages in the address range so that subsequent accesses
  // yield fresh zero-filled pages for anonymous mappings, as required by POSIX/Linux.
  // Note: MADV_FREE is an advisory hint that pages CAN be reclaimed under memory
  // pressure, but must NOT be immediately discarded if no pressure exists.
  if (advice == MADV_DONTNEED || advice == MADV_REMOVE) {
    uint64_t *pml4 = (uint64_t *)current->cr3;
    if (!pml4)
      pml4 = vmm_get_active_pml4();
    spinlock_release(&current->mm->lock);
    /* Batched exactly like sys_munmap: each 32-page chunk runs under mm->lock
     * so the VMA classification is stable, and the deferred shootdown queue
     * fills enough to coalesce into one context flush per batch. Holding
     * mm->lock across the whole range would stall faulting peers for the
     * full span; 32 pages keeps every critical section bounded. */
#define MADVISE_BATCH_PAGES 32
    for (uint64_t b = addr; b < end;) {
      uint64_t be = b + (uint64_t)MADVISE_BATCH_PAGES * PAGE_SIZE;
      if (be < b || be > end)
        be = end;
      spinlock_acquire(&current->mm->lock);
      teardown_range(pml4, current, b, be - b, true,
                     "madvise DONTNEED");
      spinlock_release(&current->mm->lock);
      /* Drain before the next batch so no frames are stranded on deferred
       * queue overflow and remote CPUs can acknowledge between short mm
       * critical sections. */
      vmm_drain_deferred_work();
      b = be;
    }
#undef MADVISE_BATCH_PAGES
    return 0;
  }

  if (advice != MADV_HUGEPAGE && advice != MADV_NOHUGEPAGE) {
    spinlock_release(&current->mm->lock);
    return 0; // Silently succeed for other advisory hints
  }

  // Walk the range page-by-page, jumping by VMA end boundaries to avoid
  // redundant tree lookups inside the same VMA.
  uint64_t page = addr & ~(PAGE_SIZE - 1ULL);
  while (page < end) {
    struct vma *v = vma_find(&current->mm->vmas, page);
    if (!v) {
      // Gap in the mapping — skip to the next page and try again.
      page += PAGE_SIZE;
      continue;
    }

    // Only apply to anonymous private mappings.
    if ((v->flags & MAP_ANONYMOUS) && (v->flags & MAP_PRIVATE)) {
      if (advice == MADV_HUGEPAGE)
        v->flags |= MAP_HUGEPAGE;
      else
        v->flags &= ~MAP_HUGEPAGE;
    }

    // Jump to the end of this VMA, clamped to the requested range.
    page = (v->end < end) ? v->end : end;
  }

  spinlock_release(&current->mm->lock);
  return 0;
}

// sys_mincore: determine whether pages are resident in memory
static uint64_t sys_mincore(uint64_t addr, uint64_t len, uint64_t vec_ptr,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a4; (void)a5; (void)a6;
  if (addr & (PAGE_SIZE - 1))
    return (uint64_t)-22; // EINVAL
  if (len == 0)
    return 0;
  if (!vec_ptr)
    return (uint64_t)-14; // EFAULT

  size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
  if (!vmm_is_user_addr_range_writable(vec_ptr, pages))
    return (uint64_t)-14; // EFAULT

  // In AvoryOS all allocated user pages are in physical memory (no swap)
  memset((void *)vec_ptr, 1, pages);
  return 0;
}

static uint64_t sys_msync(uint64_t addr, uint64_t len, uint64_t flags,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a4; (void)a5; (void)a6;
  if (addr & (PAGE_SIZE - 1))
    return (uint64_t)-22; // EINVAL: addr must be page-aligned

  #define MS_ASYNC 1
  #define MS_INVALIDATE 2
  #define MS_SYNC 4

  // Flags must contain either MS_ASYNC or MS_SYNC, but not both
  uint64_t sync_mode = flags & (MS_ASYNC | MS_SYNC);
  if (sync_mode == 0 || sync_mode == (MS_ASYNC | MS_SYNC))
    return (uint64_t)-22; // EINVAL

  // Reject unsupported flag bits
  if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
    return (uint64_t)-22; // EINVAL

  if (len == 0)
    return 0;

  if (!is_user_pointer(addr) || addr + len < addr || !is_user_pointer(addr + len))
    return (uint64_t)-14; // EFAULT

  // In AvoryOS all allocated user pages reside in-core physical memory
  // with a unified page cache. Synchronous flush is a no-op.
  return 0;
}

// Linux NUMA memory policy constants
#define MPOL_DEFAULT    0
#define MPOL_PREFERRED  1
#define MPOL_BIND       2
#define MPOL_INTERLEAVE 3
#define MPOL_LOCAL      4
#define MPOL_MAX        5

#define MPOL_F_NODE         (1 << 0)
#define MPOL_F_ADDR         (1 << 1)
#define MPOL_F_MEMS_ALLOWED (1 << 2)

#define MPOL_F_STATIC_NODES   (1 << 15)
#define MPOL_F_RELATIVE_NODES (1 << 14)
#define MPOL_MODE_FLAGS       (MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES)

// sys_get_mempolicy(int *mode, unsigned long *nodemask, unsigned long maxnode,
//                   unsigned long addr, unsigned long flags)
// Retrieves the NUMA memory policy for the calling thread or for a specific address.
// AvoryOS is a single-node system (node 0).
static uint64_t sys_get_mempolicy(uint64_t mode_ptr, uint64_t nodemask_ptr,
                                  uint64_t maxnode, uint64_t addr,
                                  uint64_t flags, uint64_t a5) {
  (void)a5;

  if (flags & ~(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED))
    return (uint64_t)-22; // EINVAL

  if ((flags & MPOL_F_MEMS_ALLOWED) && (flags & (MPOL_F_ADDR | MPOL_F_NODE)))
    return (uint64_t)-22; // EINVAL

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  uint32_t active_nodes = numa_node_count();
  uint64_t online_mask = 0;
  for (uint32_t i = 0; i < active_nodes && i < MAX_NUMA_NODES; i++) {
    struct numa_node *n = numa_get_node(i);
    if (n && n->online)
      online_mask |= (1ULL << i);
  }
  if (online_mask == 0) online_mask = 1ULL;

  if (flags & MPOL_F_MEMS_ALLOWED) {
    if (nodemask_ptr) {
      if (maxnode < 1) return (uint64_t)-22;
      unsigned long mask = (unsigned long)online_mask;
      if (copy_to_user((void *)nodemask_ptr, &mask, sizeof(mask)) != 0)
        return (uint64_t)-14;
    }
    return 0;
  }

  if (flags & MPOL_F_ADDR) {
    if (!addr || !is_user_pointer(addr))
      return (uint64_t)-14; // EFAULT

    if (flags & MPOL_F_NODE) {
      uint64_t cr3 = (current->cr3 != 0) ? current->cr3 : (uint64_t)vmm_get_active_pml4();
      uint64_t *pte_ptr = vmm_get_pte_ptr((uint64_t *)cr3, addr);
      int node = 0;
      if (pte_ptr && (*pte_ptr & PAGE_FLAG_PRESENT)) {
        node = numa_phys_to_node(*pte_ptr & PAGE_MASK);
      }
      if (mode_ptr) {
        if (copy_to_user((void *)mode_ptr, &node, sizeof(node)) != 0)
          return (uint64_t)-14;
      }
      return 0;
    }

    spinlock_acquire(&current->mm->lock);
    struct vma *v = vma_find(&current->mm->vmas, addr);
    int pol = (v && v->numa_policy != 0) ? v->numa_policy : current->numa_policy.mode;
    unsigned long mask = (v && v->numa_nodemask != 0) ? (unsigned long)v->numa_nodemask : (unsigned long)current->numa_policy.nodemask;
    spinlock_release(&current->mm->lock);

    if (mode_ptr) {
      if (copy_to_user((void *)mode_ptr, &pol, sizeof(pol)) != 0)
        return (uint64_t)-14;
    }
    if (nodemask_ptr && maxnode > 0) {
      if (copy_to_user((void *)nodemask_ptr, &mask, sizeof(mask)) != 0)
        return (uint64_t)-14;
    }
    return 0;
  }

  // Default query: current thread's policy and nodemask
  if (mode_ptr) {
    int pol = current->numa_policy.mode;
    if (copy_to_user((void *)mode_ptr, &pol, sizeof(pol)) != 0)
      return (uint64_t)-14;
  }
  if (nodemask_ptr && maxnode > 0) {
    unsigned long mask = (unsigned long)current->numa_policy.nodemask;
    if (copy_to_user((void *)nodemask_ptr, &mask, sizeof(mask)) != 0)
      return (uint64_t)-14;
  }
  return 0;
}

static uint64_t sys_set_mempolicy(uint64_t mode_raw, uint64_t nodemask_ptr,
                                  uint64_t maxnode, uint64_t a3,
                                  uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  int mode = (int)(mode_raw & ~MPOL_MODE_FLAGS);
  if (mode < 0 || mode >= MPOL_MAX)
    return (uint64_t)-22; // EINVAL

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  unsigned long mask = 0;
  if (nodemask_ptr && maxnode > 0) {
    if (copy_from_user(&mask, (const void *)nodemask_ptr, sizeof(mask)) != 0)
      return (uint64_t)-14; // EFAULT
  }

  if (mode == MPOL_DEFAULT) {
    current->numa_policy.mode = MPOL_DEFAULT;
    current->numa_policy.nodemask = 1ULL;
    current->numa_policy.preferred_node = 0;
    return 0;
  }

  uint32_t active_nodes = numa_node_count();
  uint64_t online_mask = 0;
  for (uint32_t i = 0; i < active_nodes && i < MAX_NUMA_NODES; i++) {
    struct numa_node *n = numa_get_node(i);
    if (n && n->online)
      online_mask |= (1ULL << i);
  }

  if (mode == MPOL_BIND || mode == MPOL_INTERLEAVE) {
    if ((mask & online_mask) == 0)
      return (uint64_t)-22; // EINVAL
  }

  current->numa_policy.mode = mode;
  current->numa_policy.nodemask = mask & online_mask;
  if (mode == MPOL_PREFERRED) {
    for (int i = 0; i < MAX_NUMA_NODES; i++) {
      if (mask & (1ULL << i)) {
        current->numa_policy.preferred_node = i;
        break;
      }
    }
  }
  return 0;
}

static uint64_t sys_mbind(uint64_t addr, uint64_t len, uint64_t mode_raw,
                          uint64_t nodemask_ptr, uint64_t maxnode,
                          uint64_t flags) {
  if (!addr || !is_user_pointer(addr) || (addr & (PAGE_SIZE - 1)))
    return (uint64_t)-22; // EINVAL
  if (len == 0)
    return 0;

  int mode = (int)(mode_raw & ~MPOL_MODE_FLAGS);
  if (mode < 0 || mode >= MPOL_MAX)
    return (uint64_t)-22; // EINVAL

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return (uint64_t)-1;

  unsigned long mask = 0;
  if (nodemask_ptr && maxnode > 0) {
    if (copy_from_user(&mask, (const void *)nodemask_ptr, sizeof(mask)) != 0)
      return (uint64_t)-14; // EFAULT
  }

  uint64_t end = (addr + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  spinlock_acquire(&current->mm->lock);
  for (uint64_t va = addr; va < end; va += PAGE_SIZE) {
    struct vma *v = vma_find(&current->mm->vmas, va);
    if (v) {
      v->numa_policy = mode;
      v->numa_nodemask = mask;
      if (mode == MPOL_PREFERRED) {
        for (int i = 0; i < MAX_NUMA_NODES; i++) {
          if (mask & (1ULL << i)) {
            v->numa_preferred_node = i;
            break;
          }
        }
      }
      if (v->end > va)
        va = v->end - PAGE_SIZE;
    }
  }
  spinlock_release(&current->mm->lock);

  if (flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)) {
    int target_node = 0;
    for (int i = 0; i < MAX_NUMA_NODES; i++) {
      if (mask & (1ULL << i)) {
        target_node = i;
        break;
      }
    }
    for (uint64_t va = addr; va < end; va += PAGE_SIZE) {
      numa_migrate_page(current, va, target_node);
    }
  }

  return 0;
}

// Public API

/* Write-watch via userfaultfd needs a fault-event fd and UFFDIO operations.
 * Keep the optional probe explicitly unsupported until those exist; Wine
 * uses its normal memory protection path when this returns ENOSYS. */
static uint64_t sys_userfaultfd(uint64_t flags, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  return (uint64_t)-38;
}

void syscall_register_mm(void) {
  syscall_register(SYS_USERFAULTFD, sys_userfaultfd);
  syscall_register(SYS_MMAP, sys_mmap);
  syscall_register(SYS_MUNMAP, sys_munmap);
  syscall_register(SYS_MREMAP, sys_mremap);
  syscall_register(SYS_MSYNC, sys_msync);
  syscall_register(SYS_MINCORE, sys_mincore);
  syscall_register(SYS_MADVISE, sys_madvise);
  syscall_register(SYS_BRK, sys_brk);
  syscall_register(SYS_MPROTECT, sys_mprotect);
  syscall_register(SYS_MBIND, sys_mbind);
  syscall_register(SYS_SET_MEMPOLICY, sys_set_mempolicy);
  syscall_register(SYS_GET_MEMPOLICY, sys_get_mempolicy);
  syscall_register(SYS_MOVE_PAGES, sys_move_pages);
  syscall_register(SYS_MIGRATE_PAGES, sys_migrate_pages);
}
// Called from process_exec when loading a new process image.
void mm_reset_mmap_state(struct thread *t) {
  if (!t || !t->mm)
    return;
  t->mm->mmap_next_addr = MMAP_REGION_BASE;
  /* A new program inherits the address space bookkeeping but not the identity
   * of the old one: until build_user_stack() records the new argv, /proc must
   * report no command line rather than the previous program's. */
  t->mm->cmdline_len = 0;
  t->mm->arg_start   = 0;
  t->mm->arg_end     = 0;
}

// Allocate a virtual address range from the mmap region for device mmap
// handlers (e.g. framebuffer, DMA).  Returns the base VA, or 0 on failure.
uint64_t mm_alloc_mmap_region(uint64_t length) {
  if (length == 0)
    return 0;

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return 0;

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  spinlock_acquire(&current->mm->lock);
  if (current->mm->mmap_next_addr + aligned_len > MMAP_REGION_LIMIT) {
    spinlock_release(&current->mm->lock);
    return 0;
  }

  uint64_t vaddr = current->mm->mmap_next_addr;
  current->mm->mmap_next_addr += aligned_len;
  spinlock_release(&current->mm->lock);
  return vaddr;
}
