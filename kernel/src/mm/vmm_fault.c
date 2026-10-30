#include "../fs/tmpfs.h"
#include "mm/zram.h"

#include "../apic/lapic_timer.h"
#include "../arch/x86_64/extable.h"
#include "../console/klog.h"
#include "../cpu/kpf_dump.h"
#include "../cpu/msr.h"
#include "../fs/vfs.h"
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "pmm.h"
#include "numa.h"
#include "mempolicy.h"
#include "page_ops.h"
#include "tlb_shootdown.h"
#include "vma.h"
#include "vmm.h"
#include "vmm_internal.h"
#include "shm.h"
#include <stddef.h>
#include <stdint.h>

static void pf_shmem_put(vfs_node_t **node) { if (*node) vfs_close(*node); }

#ifndef PROT_NONE
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#endif

/* Bad user pointers can fault on several worker threads at once.  Printing an
 * entire VMA tree for each one floods and interleaves the serial log; keep a
 * few representative records and retain a suppression count for the rest. */
static volatile uint32_t no_vma_user_log_count;

/* Page faults enter through an interrupt gate, so IF is normally clear even
 * for user tasks. File-cache fills and imported driver faults may read, wait,
 * or yield; let IPIs and timer/device IRQs run around those calls, but never
 * open interrupts from a nested hard-IRQ fault. */
static bool pf_enable_irqs_for_wait(void) {
  bool were_enabled = hal_irq_enabled();
  if (!were_enabled) {
    extern int linuxkpi_irq_depth(void) __attribute__((weak));
    if (!linuxkpi_irq_depth || linuxkpi_irq_depth() == 0)
      hal_irq_enable();
  }
  return were_enabled;
}

static void pf_restore_irqs_after_wait(bool were_enabled) {
  if (!were_enabled)
    hal_irq_disable();
}

static uint64_t dp_build_flags(uint64_t prot) {
  if (prot == PROT_NONE)
    return 0;

  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & PROT_WRITE)
    flags |= PAGE_FLAG_RW;
  if (!(prot & PROT_EXEC))
    flags |= PAGE_FLAG_NX;
  return flags;
}

/* True when a pre-zeroed pool page may back this fault: no NUMA bind policy
 * on the VMA and none on the current thread (pool pages are node-agnostic,
 * refilled from the local node). */
static inline bool vmm_zero_pool_usable(int vma_policy) {
  if (vma_policy != MPOL_DEFAULT)
    return false;
  struct thread *cur = sched_get_current();
  return !cur || cur->numa_policy.mode == MPOL_DEFAULT;
}

static inline void *vmm_alloc_page_numa(int vma_numa_policy, uint64_t vma_numa_nodemask,
                                        int vma_numa_preferred_node, uint64_t vaddr) {
  struct thread *current = sched_get_current();
  if (__builtin_expect(vma_numa_policy == MPOL_DEFAULT &&
                       (!current || current->numa_policy.mode == MPOL_DEFAULT), 1)) {
    return pmm_alloc_page();
  }

  struct mempolicy pol;

  if (vma_numa_policy != MPOL_DEFAULT) {
    pol.mode = vma_numa_policy;
    pol.nodemask = vma_numa_nodemask;
    pol.preferred_node = vma_numa_preferred_node;
    pol.interleave_idx = 0;
  } else if (current) {
    mempolicy_copy(&pol, &current->numa_policy);
  } else {
    mempolicy_init(&pol);
  }

  int target_node = mempolicy_select_node(&pol, vaddr);
  struct cpu_info *cpu = cpu_get_current();
  int local_node = (cpu && cpu->cpu_id < MAX_CPUS) ? numa_cpu_to_node(cpu->cpu_id) : 0;

  if (target_node == local_node &&
      (pol.mode != MPOL_BIND || (pol.nodemask & (1ULL << (local_node % 64))))) {
    void *page = pmm_pcp_alloc_page();
    if (page)
      return page;
  }

  uint64_t mask = (pol.mode == MPOL_BIND) ? pol.nodemask : 0;
  return pmm_alloc_pages_nodemask(1, target_node, mask);
}

static inline void *vmm_alloc_huge_page_numa(int vma_numa_policy, uint64_t vma_numa_nodemask,
                                             int vma_numa_preferred_node, uint64_t vaddr) {
  struct thread *current = sched_get_current();
  if (__builtin_expect(vma_numa_policy == MPOL_DEFAULT &&
                       (!current || current->numa_policy.mode == MPOL_DEFAULT), 1)) {
    return pmm_alloc_huge_page();
  }

  struct mempolicy pol;

  if (vma_numa_policy != MPOL_DEFAULT) {
    pol.mode = vma_numa_policy;
    pol.nodemask = vma_numa_nodemask;
    pol.preferred_node = vma_numa_preferred_node;
    pol.interleave_idx = 0;
  } else if (current) {
    mempolicy_copy(&pol, &current->numa_policy);
  } else {
    mempolicy_init(&pol);
  }

  int target_node = mempolicy_select_node(&pol, vaddr);
  uint64_t mask = (pol.mode == MPOL_BIND) ? pol.nodemask : 0;
  void *phys = pmm_alloc_pages_nodemask(512, target_node, mask);
  if (!phys)
    return NULL;
  if ((uint64_t)phys & 0x1FFFFFULL) {
    pmm_free_pages(phys, 512);
    return NULL;
  }
  return phys;
}

/* ---- Demand-fault low watermark ------------------------------------------
 *
 * Reclaim used to start only after a fault allocation had already failed,
 * so a burst could drive the buddy allocator to exactly zero and the fault
 * that arrived with nothing free killed a process even though megabytes of
 * clean cache were still droppable (2026-10-16 capture: two [VMM] OOM
 * demand-page kills with free_pages=0 and free_pages=40).  When free pages
 * dip below this watermark the next user fault drops clean cache first, so
 * the allocator keeps a small reclaimable headroom while a Firefox/KDE
 * burst runs instead of first hitting zero.
 *
 * Rate-limited: one reclaim pass every 250 ms is plenty to track a
 * demand-fault burst. Parked PCP
 * and zero-pool frames are returned unconditionally - cache reclaim cannot
 * see them, and merging them restores the high-order blocks that a TTM or
 * huge-page allocation needs. */
#define VMM_FAULT_WATERMARK_PAGES 2048u /* 8 MB */
#define VMM_FAULT_RECLAIM_INTERVAL_MS 250u
#define VMM_FAULT_RECLAIM_MAX_PAGES 4096u

/* Escalated-pressure hooks.  The generic heap and the LinuxKPI shrinker
 * registry are linked into this image; weak declarations keep the mm layer
 * independently linkable (same pattern as heap_slab.c's file-cache hook). */
extern void heap_oom_reclaim(void) __attribute__((weak));
extern unsigned long linuxkpi_shrink_reclaim(unsigned int gfp,
                                             unsigned long needed)
    __attribute__((weak));
extern size_t thread_stack_cache_reclaim(size_t max_reclaim)
    __attribute__((weak));
extern size_t ext2_buffer_cache_shrink(size_t max_buffers)
    __attribute__((weak));
extern size_t pty_shrink_unused_buffers(void)
    __attribute__((weak));

static volatile uint64_t vmm_fault_reclaim_next_ms;

static void vmm_fault_watermark_reclaim(void) {
  uint64_t now = lapic_timer_get_ms();
  uint64_t next = __atomic_load_n(&vmm_fault_reclaim_next_ms, __ATOMIC_RELAXED);

  if (now < next)
    return;
  if (!__atomic_compare_exchange_n(&vmm_fault_reclaim_next_ms, &next,
                                   now + VMM_FAULT_RECLAIM_INTERVAL_MS, false,
                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    return;

  size_t free_pages = pmm_get_free_pages();
  if (free_pages >= VMM_FAULT_WATERMARK_PAGES)
    return;

  size_t target = VMM_FAULT_WATERMARK_PAGES - free_pages;
  if (target > VMM_FAULT_RECLAIM_MAX_PAGES)
    target = VMM_FAULT_RECLAIM_MAX_PAGES;
  pmm_pcp_drain_all();
  if (thread_stack_cache_reclaim)
    thread_stack_cache_reclaim(8);
  if (ext2_buffer_cache_shrink)
    ext2_buffer_cache_shrink(16);
  if (pty_shrink_unused_buffers)
    pty_shrink_unused_buffers();
  mm_file_cache_reclaim(target);
}

/* Map already-cached neighbours of a freshly faulted private file page in one
 * pass, the way the shared/read-only path clusters its 128 KB window.  Every
 * page is installed read-only with the CoW bit, so the first write still
 * copies in the write-fault path.  Each frame gets its own reference; pages
 * whose VMA changed, disappeared, or no longer describes the same file offset
 * are dropped instead of being mapped. */
static void vmm_map_cow_cluster(uint64_t *pml4, uint64_t cr2,
                                vfs_node_t *node, struct mm_struct *mm,
                                uint64_t vma_start, uint64_t vma_end,
                                uint64_t vma_offset, uint64_t eff_file_size) {
  if (!pml4 || !node || !mm)
    return;

  uint64_t fault_vpage = cr2 & ~0xFFFULL;
  uint64_t cluster_vstart = cr2 & ~0x1FFFFULL;

  struct {
    uint64_t vpage;
    uint64_t phys;
  } batch[32];
  uint32_t batch_count = 0;

  spinlock_acquire(&node->pages_lock);
  for (int ci = 0; ci < 32; ci++) {
    uint64_t vpage = cluster_vstart + (uint64_t)ci * PAGE_SIZE;
    if (vpage == fault_vpage || vpage < vma_start || vpage >= vma_end)
      continue;
    uint64_t page_off = vpage - vma_start;
    if (page_off + PAGE_SIZE > eff_file_size)
      continue; // partial tail: zeros come from the copy-now path
    uint32_t file_off = (uint32_t)(vma_offset + page_off);
    vfs_page_t *p = asc_radix_tree_lookup(&node->pages, (uint64_t)(file_off >> 12));
    if (p && vfs_cache_page_valid(p) && !p->loading && p->uptodate &&
        p->frame_phys && !vfs_page_is_evicted(p)) {
      batch[batch_count].vpage = vpage;
      batch[batch_count].phys = p->frame_phys;
      pmm_incref((void *)p->frame_phys);
      batch_count++;
    }
  }
  spinlock_release(&node->pages_lock);

  if (batch_count == 0)
    return;

  /* Hold mm->lock while installing: mm->lock -> vmm_lock is the established
   * order, and it keeps a concurrent munmap/mprotect from leaving a phantom
   * mapping behind.  Each page is revalidated against the current VMA so a
   * split, merge or remap cannot alias a frame that belongs elsewhere. */
  spinlock_acquire(&mm->lock);
  vmm_lock_acquire_for(pml4, 0);
  for (uint32_t i = 0; i < batch_count; i++) {
    struct vma *nv = vma_find(&mm->vmas, batch[i].vpage);
    uint64_t nv_file_size =
        nv ? ((nv->file_size > 0 || (nv->flags & MAP_ELF_LOAD)) ? nv->file_size : (nv->end - nv->start)) : 0;
    bool ok = nv && (nv->flags & MAP_PRIVATE) && (nv->prot & PROT_READ) &&
              batch[i].vpage >= nv->start &&
              batch[i].vpage + PAGE_SIZE <= nv->end &&
              (batch[i].vpage - nv->start) + PAGE_SIZE <= nv_file_size &&
              (void *)nv->file_node == (void *)node &&
              (uint32_t)(nv->offset + (batch[i].vpage - nv->start)) ==
                  (uint32_t)(vma_offset + (batch[i].vpage - vma_start));
    if (!ok) {
      pmm_decref((void *)batch[i].phys);
      continue;
    }

    uint64_t flags = (dp_build_flags(nv->prot) & ~PAGE_FLAG_RW) |
                     PAGE_FLAG_COW | PAGE_FLAG_REFAULT;
    bool changed = false;
    if (vmm_page_present_nolock(pml4, batch[i].vpage) ||
        !vmm_map_page_locked(pml4, batch[i].vpage, batch[i].phys, flags,
                             &changed))
      pmm_decref((void *)batch[i].phys);
  }
  vmm_lock_release();
  spinlock_release(&mm->lock);
}

/* ---- Why the paging engine refused a fault --------------------------------
 *
 * A -1 is a death sentence for a kernel-mode fault, so every bail-out in
 * vmm_handle_page_fault() annotates itself and page_fault_handler() prints the
 * record through kpf_dump.c while the machine can still talk.
 *
 * Implemented in vmm_fault_check.c.
 */

/* Transparent huge pages. "madvise" (default) only backs VMAs that asked for
 * them (MAP_HUGETLB / MADV_HUGEPAGE). "always" also backs large anonymous
 * private VMAs on a write fault; read faults keep using the shared zero page
 * so an untouched mapping never costs 2 MB. Stacks and MADV_NOHUGEPAGE VMAs
 * are excluded. fork still deep-copies huge pages, so "always" trades fork
 * latency of big processes for fewer TLB misses and page faults. */
static int thp_mode = THP_MODE_MADVISE;

int vmm_thp_mode(void) { return __atomic_load_n(&thp_mode, __ATOMIC_RELAXED); }

void vmm_set_thp_mode(int mode) {
  if (mode == THP_MODE_MADVISE || mode == THP_MODE_ALWAYS)
    __atomic_store_n(&thp_mode, mode, __ATOMIC_RELAXED);
}

static inline bool vma_wants_huge(uint64_t flags, bool write_fault) {
  if (!(flags & MAP_ANONYMOUS))
    return false;
  if (flags & MAP_HUGEPAGE)
    return true;
  return vmm_thp_mode() == THP_MODE_ALWAYS && write_fault &&
         (flags & MAP_PRIVATE) &&
         !(flags & (MAP_NOHUGEPAGE | MAP_STACK | MAP_GROWSDOWN | MAP_SYSV_SHM));
}

int vmm_handle_page_fault(uint64_t cr2, uint64_t error_code,
                          struct registers *regs) {
  bool user_mode = (error_code & 0x4) != 0;
  bool write_fault = (error_code & 0x2) != 0;
  bool present_bit = (error_code & 0x1) != 0;
  bool exec_fault = (error_code & 0x10) != 0;


  struct thread *current = sched_get_current();
  if (!current)
    return PF_REJECT("no current thread - fault outside any thread context", 0,
                     0);

  /* A kernel thread has no address space, so there is nothing to demand page
   * and no VMA to recover permissions from. Checked here because every path
   * below reaches through current->mm->lock, which would fault again inside
   * the fault handler and turn this reportable #PF into a double fault. */
  if (!current->mm)
    return PF_REJECT("thread has no address space (kernel thread)", 0, 0);

  uint64_t target_cr3 = current->cr3;
  if (target_cr3 == 0) {
    __asm__ volatile("mov %%cr3, %0" : "=r"(target_cr3));
    target_cr3 &= 0xFFFFFFFFFFFFF000ULL;
  }

  /* User demand paging keeps its own headroom: drop clean cache before the
   * allocator reaches zero (see vmm_fault_watermark_reclaim()). */
  if (user_mode)
    vmm_fault_watermark_reclaim();

  /* Swap faults must precede anonymous zero-fill/fault-around: a
   * compressed non-present PTE is data, never an empty mapping. */
  if (user_mode && zram_under_pressure()) {
    bool irq_state = pf_enable_irqs_for_wait();
    zram_balance();
    pf_restore_irqs_after_wait(irq_state);
  }
  if (!present_bit) {
    bool irq_state = pf_enable_irqs_for_wait();
    int swapped = zram_fault(current->mm, target_cr3, cr2, write_fault, exec_fault);
    pf_restore_irqs_after_wait(irq_state);
    if (swapped > 0) return 0;
    if (swapped < 0) return PF_REJECT("compressed swap fault failed", -swapped, cr2);
  }

  if (present_bit && write_fault) {
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(target_cr3);
    uint64_t virt = cr2 & PAGE_MASK;

    /* Lock order in this kernel is mm->lock -> vmm_lock.  sys_mmap(),
     * sys_mprotect(), sys_madvise() and the teardown paths all hold mm->lock
     * and then reach vmm_map_page()/vmm_unmap_page(), which take vmm_lock.
     * This path used to take them the other way round (mm->lock from inside
     * the vmm_lock section below), so a CoW fault on one CPU could deadlock
     * against an mprotect()/mmap() on another, with neither able to back off. */
    spinlock_acquire(&current->mm->lock);
    vmm_lock_acquire_for((uint64_t *)target_cr3, virt);

    /* Every bail-out from the walk below has to unwind the pair again. */
#define WALK_UNLOCK()                                                          \
  do {                                                                         \
    vmm_lock_release();                                       \
    spinlock_release(&current->mm->lock);                                      \
  } while (0)
#define WALK_REJECT(reason, d1, d2)                                             \
  do {                                                                         \
    WALK_UNLOCK();                                                             \
    return PF_REJECT((reason), (d1), (d2));                                    \
  } while (0)

    uint64_t pml4e = pml4[(virt >> 39) & 511];
    if (!(pml4e & PAGE_FLAG_PRESENT))
      WALK_REJECT("write walk: PML4E is not present", pml4e, virt);
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4e & PAGE_MASK);

    uint64_t pdpte = pdpt[(virt >> 30) & 511];
    if (!(pdpte & PAGE_FLAG_PRESENT) || (pdpte & PAGE_FLAG_PS))
      WALK_REJECT("write walk: PDPT entry missing or a 1GB page", pdpte, virt);

    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpte & PAGE_MASK);
    uint64_t pde = pd[(virt >> 21) & 511];
    if (!(pde & PAGE_FLAG_PRESENT) || (pde & PAGE_FLAG_PS))
      WALK_REJECT("write walk: PD entry missing or a 2MB huge page", pde, virt);

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pde & PAGE_MASK);
    uint64_t *pte = &pt[(virt >> 12) & 511];
    if (!(*pte & PAGE_FLAG_PRESENT))
      WALK_REJECT("error code says P=1 but the PTE is not present", *pte, virt);

    /* The frame the PTE stopped pointing at, and whether a flush is owed.
     * Both are acted on only after the locks are dropped - see below. */
    uint64_t cow_old_phys = 0;
    bool cow_broken = false;

    if (*pte & PAGE_FLAG_RW) {
      // Stale TLB: Another thread in this process already broke CoW on this
      // page.
      WALK_UNLOCK();
      tlb_shootdown_page(virt);
      return 0;
    }

    if (!(*pte & PAGE_FLAG_COW)) {
      // Not marked COW yet. Is it a MAP_PRIVATE VMA that's now writable?
      // (This can happen if it was first mapped read-only and then mprotected).
      // mm->lock is already held, so the VMA tree may be walked directly.
      int v_numa_policy = 0;
      uint64_t v_numa_nodemask = 0;
      int v_numa_preferred_node = 0;
      struct vma *v = vma_find(&current->mm->vmas, cr2);
      if (v) {
        v_numa_policy = v->numa_policy;
        v_numa_nodemask = v->numa_nodemask;
        v_numa_preferred_node = v->numa_preferred_node;
        if ((v->flags & MAP_PRIVATE) && (v->prot & PROT_WRITE)) {
          // Writable private mappings may have been remapped read-only for
          // CoW. Read-only executable/file mappings must stay protected.
          *pte |= PAGE_FLAG_COW;
        }
      }
    }

    if (*pte & PAGE_FLAG_COW) {
      uint64_t old_phys = *pte & PAGE_MASK;

      if (old_phys == pmm_get_zero_page_phys()) {
        // Prefer an idle-zeroed frame; fall back to alloc+zero inline.
        void *new_phys = vmm_zero_pool_usable(MPOL_DEFAULT)
                             ? pmm_alloc_zeroed_page()
                             : NULL;
        if (!new_phys) {
          new_phys = vmm_alloc_page_numa(0, 0, 0, virt);
          if (!new_phys)
            WALK_REJECT("CoW break of the shared zero page: PMM out of "
                        "memory", old_phys, 0);

          page_zero(PHYS_TO_VIRT((uint64_t)new_phys));
        }

        *pte = ((uint64_t)new_phys & PAGE_MASK) |
               (*pte & ~PAGE_MASK & ~(PAGE_FLAG_COW | PAGE_FLAG_REFAULT)) |
               PAGE_FLAG_RW;
        pml4[(virt >> 39) & 511] |= (PAGE_FLAG_RW | PAGE_FLAG_USER);
        pdpt[(virt >> 30) & 511] |= (PAGE_FLAG_RW | PAGE_FLAG_USER);
        pd[(virt >> 21) & 511] |= (PAGE_FLAG_RW | PAGE_FLAG_USER);
        WALK_UNLOCK();
        tlb_shootdown_page(virt);
        return 0; // zero page CoW broken with fresh zeroed page
      }

      uint16_t refs = pmm_get_ref((void *)old_phys);

      if (refs > 1) {
        // Multiple owners — make a private copy.
        void *new_phys = vmm_alloc_page_numa(0, 0, 0, virt);
        if (!new_phys)
          WALK_REJECT("CoW copy: PMM out of memory", old_phys, refs);

        page_copy(PHYS_TO_VIRT((uint64_t)new_phys), PHYS_TO_VIRT(old_phys));

        *pte = ((uint64_t)new_phys & PAGE_MASK) |
               (*pte & ~PAGE_MASK & ~(PAGE_FLAG_COW | PAGE_FLAG_REFAULT)) |
               PAGE_FLAG_RW;
        // Released after the flush below, not here.
        cow_old_phys = old_phys;
      } else {
        // Sole owner — just make it writable in place.
        *pte &= ~(PAGE_FLAG_COW | PAGE_FLAG_REFAULT);
        *pte |= PAGE_FLAG_RW;
      }

      pml4[(virt >> 39) & 511] |= (PAGE_FLAG_RW | PAGE_FLAG_USER);
      pdpt[(virt >> 30) & 511] |= (PAGE_FLAG_RW | PAGE_FLAG_USER);
      pd[(virt >> 21) & 511] |= (PAGE_FLAG_RW | PAGE_FLAG_USER);
      cow_broken = true;
    }

    WALK_UNLOCK();
#undef WALK_UNLOCK
#undef WALK_REJECT

    if (cow_broken) {
      /* Flushing and releasing the old frame happen outside both locks on
       * purpose.  A shootdown IPIs every CPU and waits for them, which must
       * never be done while holding a lock that other CPUs are waiting for -
       * the initiator would be waiting for CPUs that are waiting for it.
       * Dropping the frame only after the flush keeps the window closed: until
       * every CPU has thrown away its stale writable translation, the frame
       * cannot be handed to anybody else. */
      tlb_shootdown_page(virt);
      if (cow_old_phys)
        pmm_decref((void *)cow_old_phys);
      return 0; // fault handled
    }

    // Present page, write fault, no COW flag → genuine write protection
    // violation.
    if (user_mode) {
      klog_puts("[VMM] Write-protect fault at CR2=");
      klog_hex64(cr2);
      klog_puts(" RIP=");
      klog_hex64(regs->rip);
      klog_puts(" tid=");
      klog_uint64(current->tid);
      klog_puts(" RSP=");
      klog_hex64(regs->rsp);
      klog_puts("\n");
      return PF_REJECT("write-protect violation: present page is read-only and "
                       "not CoW", *pte, *pte & PAGE_FLAG_COW);
    }
  }

  // If we reach here for a PRESENT page, check if the VMA permissions allow this
  // access (e.g. if the page was previously PROT_NONE or missing USER/RW bits).
  if (present_bit) {
    const char *why = user_mode
                          ? "present page: no VMA covers it, permissions could "
                            "not be recovered"
                          : "present page: ring-0 fault outside any VMA "
                            "(protection violation)";
    uint64_t vma_prot = 0;
    uint64_t vma_start = 0;
    spinlock_acquire(&current->mm->lock);
    struct vma *v = vma_find(&current->mm->vmas, cr2);
    if (v && v->prot != PROT_NONE) {
      vma_prot = v->prot;
      vma_start = v->start;
      bool allowed = true;
      if (write_fault && !(v->prot & PROT_WRITE)) allowed = false;
      if (exec_fault && !(v->prot & PROT_EXEC)) allowed = false;
      if (!write_fault && !exec_fault && !(v->prot & PROT_READ)) allowed = false;
      if (!allowed)
        why = "present page: VMA permissions forbid this access";

      if (allowed) {
        uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(target_cr3);
        uint64_t virt = cr2 & PAGE_MASK;
        if (pml4[(virt >> 39) & 511] & PAGE_FLAG_PRESENT) {
          uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[(virt >> 39) & 511] & PAGE_MASK);
          if ((pdpt[(virt >> 30) & 511] & PAGE_FLAG_PRESENT) && !(pdpt[(virt >> 30) & 511] & PAGE_FLAG_PS)) {
            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[(virt >> 30) & 511] & PAGE_MASK);
            if ((pd[(virt >> 21) & 511] & PAGE_FLAG_PRESENT) && !(pd[(virt >> 21) & 511] & PAGE_FLAG_PS)) {
              uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[(virt >> 21) & 511] & PAGE_MASK);
              uint64_t *pte = &pt[(virt >> 12) & 511];
              if (*pte & PAGE_FLAG_PRESENT) {
                uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
                if (v->prot & PROT_WRITE) flags |= PAGE_FLAG_RW;
                if (!(v->prot & PROT_EXEC)) flags |= PAGE_FLAG_NX;
                *pte = (*pte & PAGE_MASK) | flags;
                pml4[(virt >> 39) & 511] |= (PAGE_FLAG_USER | (flags & PAGE_FLAG_RW));
                pdpt[(virt >> 30) & 511] |= (PAGE_FLAG_USER | (flags & PAGE_FLAG_RW));
                pd[(virt >> 21) & 511] |= (PAGE_FLAG_USER | (flags & PAGE_FLAG_RW));
                vmm_flush_tlb(virt);
                spinlock_release(&current->mm->lock);
                return 0; // Recovered PTE permissions from VMA
              }
              why = "present page: PTE vanished underneath the permission "
                    "recovery walk";
            } else {
              why = "present page: walk stopped at a missing PD or a huge page";
            }
          } else {
            why = "present page: walk stopped at a missing PDPT or a huge page";
          }
        } else {
          why = "present page: walk stopped at an absent PML4E";
        }
      }
    } else if (v) {
      vma_start = v->start;
      why = "present page: VMA is PROT_NONE";
    }
    spinlock_release(&current->mm->lock);

    if (user_mode) {
      klog_puts("[VMM] Protection fault on present page at CR2=");
      klog_hex64(cr2);
      klog_puts(" RIP=");
      klog_hex64(regs->rip);
      klog_puts(" err=");
      klog_hex64(error_code);
      klog_puts("\n");
    }
    return PF_REJECT(why, vma_prot, vma_start);
  }

  // ---- VMA-based demand paging --------------------------------------------

  spinlock_acquire(&current->mm->lock);

  struct vma *vma = vma_find(&current->mm->vmas, cr2);

  if (!vma) {
    // Try automatic stack growth (GROWSDOWN VMA within 8 MB).
    vma = vma_find_growdown(&current->mm->vmas, cr2, 8 * 1024 * 1024);
    if (vma) {
      uint64_t old_start = vma->start;
      uint64_t old_end = vma->end;
      uint64_t new_start = cr2 & ~0xFFFULL;
      uint64_t prot = vma->prot;
      uint64_t flags = vma->flags;
      int fd = vma->fd;
      uint64_t offset = vma->offset;
      /* Stack VMAs are anonymous, but a MAP_GROWSDOWN file mapping can carry
       * a bridge wrapper; keep it across the remove/re-add (P2 gap). */
      void *keep_lvma = vma->linux_vma;

      vma_linux_get(keep_lvma);
      vma_remove(&current->mm->vmas, old_start, old_end);
      if (vma_add(&current->mm->vmas, new_start, old_end, prot, flags, fd,
                  offset, NULL, 0) != 0) {
        klog_puts("[VMM] Stack expansion failed (overlap?) for CR2=");
        klog_hex64(cr2);
        klog_puts("\n");
        if (vma_add(&current->mm->vmas, old_start, old_end, prot, flags, fd,
                    offset, NULL, 0) == 0)
          vma_attach_linux(&current->mm->vmas, old_start, keep_lvma);
        vma = NULL;
      } else {
        vma_attach_linux(&current->mm->vmas, new_start, keep_lvma);
        vma = vma_find(&current->mm->vmas, cr2);
      }
      vma_linux_put(keep_lvma);
    } else {
      struct vma *potential =
          vma_find_growdown(&current->mm->vmas, cr2, 1024ULL * 1024 * 1024);
      if (potential) {
        klog_puts("[VMM] Rejected stack growth: CR2=");
        klog_hex64(cr2);
        klog_puts(" is too far below VMA ");
        klog_hex64(potential->start);
        klog_puts("\n");
      }
    }
  }

  uint64_t vma_prot = 0;
  uint64_t vma_flags = 0;
  int vma_fd = -1;
  uint64_t vma_offset = 0;
  uint64_t vma_file_size = 0;
  uint64_t vma_start = 0;
  uint64_t vma_end = 0;
  vfs_node_t *shmem_pin __attribute__((cleanup(pf_shmem_put))) = NULL;
  void *vma_file_node = NULL;
  void *vma_linux = NULL;
  int vma_numa_policy = 0;
  uint64_t vma_numa_nodemask = 0;
  int vma_numa_preferred_node = 0;
  if (vma) {
    vma_prot = vma->prot;
    vma_flags = vma->flags;
    vma_fd = vma->fd;
    (void)vma_fd; // captured for future use (e.g. close-on-exec logic)
    vma_offset = vma->offset;
    vma_file_size = vma->file_size;
    vma_start = vma->start;
    vma_end = vma->end;
    vma_file_node = vma->file_node;
    if (tmpfs_is_shmem((vfs_node_t *)vma_file_node)) {
      shmem_pin = vma_file_node;
      vfs_node_ref(shmem_pin);
    }
    vma_numa_policy = vma->numa_policy;
    vma_numa_nodemask = vma->numa_nodemask;
    vma_numa_preferred_node = vma->numa_preferred_node;

    /* The Linux-facing bridge wrapper can go away the moment a concurrent
     * munmap removes this native VMA, and the imported driver's ->close
     * callback (TTM's clears vma->vm_private_data) would then run while a
     * fault on this CPU is still in flight - the fault reloads
     * vm_private_data and dereferences NULL.  The fault can also sleep
     * (TTM waits on a dma-fence), so mm->lock cannot be held across it
     * either.  Take a reference while the VMA is still pinned by mm->lock
     * and hold it until the fault returns: ->close only runs when the last
     * reference drops. */
    vma_linux = vma->linux_vma;
    if (vma_linux)
      vma_linux_get(vma_linux);
  }
  spinlock_release(&current->mm->lock);

  /* Imported Linux drivers can handle the fault themselves through
   * vm_ops->fault(); the bridge builds a struct vm_fault and installs PTEs
   * on their behalf if they ask for it. */
  if (vma_linux) {
    extern int linuxkpi_vma_fault(void *, unsigned long, unsigned long)
        __attribute__((weak));

    bool irq_state = pf_enable_irqs_for_wait();
    int driver_fault_result =
        linuxkpi_vma_fault ? linuxkpi_vma_fault(vma_linux, cr2, error_code)
                           : -1;
    pf_restore_irqs_after_wait(irq_state);
    if (linuxkpi_vma_fault && driver_fault_result == 0) {
      /* The driver fault can sleep (TTM waits on a dma-fence) and another
       * thread may have unmapped this range meanwhile.  A PTE it installed
       * then outlives the buffer object whose mapping is gone, so validate
       * the VMA again under mm->lock and drop the translation if it is no
       * longer the same mapping.  mm->lock -> vmm_lock is the established
       * order (see vmm_map_cow_cluster()). */
      spinlock_acquire(&current->mm->lock);
      struct vma *still = vma_find(&current->mm->vmas, cr2);
      bool same_mapping = still && still->linux_vma == vma_linux;
      spinlock_release(&current->mm->lock);
      /* Tear the translation down before dropping the last reference: the
       * put may run ->close and free the buffer object behind it. */
      if (!same_mapping)
        vmm_unmap_page((uint64_t *)target_cr3, cr2 & ~0xFFFULL);
      vma_linux_put(vma_linux);
      if (!same_mapping)
        return PF_REJECT("driver mapping disappeared while the fault was in "
                         "flight (racing munmap)", cr2, 0);
      return 0;
    }
    vma_linux_put(vma_linux);
    /* A driver-owned VMA has no generic backing. Falling through here can
     * map fresh anonymous RAM after a GEM/TTM fault failed, so CPU uploads
     * and GPU accesses silently refer to different physical pages. */
    return PF_REJECT("driver rejected the fault in its own mapping", cr2,
                     (uint64_t)(int64_t)driver_fault_result);
  }

  if (!vma) {
    // No VMA covers this address — genuine segfault.
    if (user_mode) {
      void *handler = (void *)current->signal_handlers[10].sa_handler;
      bool has_custom_handler = (handler != NULL && handler != (void *)1);
      if (!has_custom_handler) {
        uint32_t seen = __atomic_add_fetch(&no_vma_user_log_count, 1,
                                           __ATOMIC_RELAXED);
        if (seen <= 8 || (seen & 0xFFu) == 0) {
          klog_puts("[VMM] user fault outside VMA cr2=");
          klog_hex64(cr2);
          klog_puts(" rip=");
          klog_hex64(regs->rip);
          klog_puts(" err=");
          klog_hex64(error_code);
          klog_puts(" tid=");
          klog_uint64(current->tid);
          klog_puts(" comm='");
          klog_puts(current->comm);
          klog_puts("' count=");
          klog_uint64(seen);
          klog_puts("\n");
          /* Preserve the user register state at the fault. In particular,
           * null reads inside user code need the base/index registers to
           * distinguish a bad pointer from a bad syscall or VMM mapping. */
          klog_puts("[VMM] user fault regs rax="); klog_hex64(regs->rax);
          klog_puts(" rbx="); klog_hex64(regs->rbx);
          klog_puts(" rcx="); klog_hex64(regs->rcx);
          klog_puts(" rdx="); klog_hex64(regs->rdx);
          klog_puts(" rsi="); klog_hex64(regs->rsi);
          klog_puts(" rdi="); klog_hex64(regs->rdi);
          klog_puts(" rbp="); klog_hex64(regs->rbp);
          klog_puts(" rsp="); klog_hex64(regs->rsp);
          klog_puts("\n[VMM] user fault regs r8="); klog_hex64(regs->r8);
          klog_puts(" r9="); klog_hex64(regs->r9);
          klog_puts(" r10="); klog_hex64(regs->r10);
          klog_puts(" r11="); klog_hex64(regs->r11);
          klog_puts(" r12="); klog_hex64(regs->r12);
          klog_puts(" r13="); klog_hex64(regs->r13);
          klog_puts(" r14="); klog_hex64(regs->r14);
          klog_puts(" r15="); klog_hex64(regs->r15);
          klog_puts("\n[VMM] user TLS fs=");
          klog_hex64(rdmsr(0xC0000100));
          klog_puts(" gs=");
          /* isr_common_stub already executed SWAPGS for this user fault. */
          klog_hex64(rdmsr(0xC0000102));
          klog_puts(" tracked_fs="); klog_hex64(current->fs_base);
          klog_puts(" tracked_gs="); klog_hex64(current->gs_base);
          klog_puts("\n");
        } else if (seen == 9) {
          klog_puts("[VMM] repeated user no-VMA faults suppressed; sampling "
                    "every 256th (counter is global)\n");
        }
      }
      return PF_REJECT("no VMA covers the faulting address", cr2, 0);
    }

    if (!user_mode && cr2 <= USER_SPACE_LIMIT) {
      if (regs && extable_has_entry(regs->rip)) {
        return PF_REJECT("kernel read of a user address at a guarded (extable) "
                         "instruction", cr2, regs->rip);
      }
      /* Capture the unguarded fault before ordinary logging can take locks. */
      kpf_dump_thread_fault(regs, cr2);
      klog_puts("[VMM] kernel access faulted on user address cr2=");
      klog_hex64(cr2);
      klog_puts(" rip=");
      klog_hex64(regs->rip);
      klog_puts(" tid=");
      klog_uint64(current->tid);
      klog_puts(" comm='");
      klog_puts((current->comm[0]) ? current->comm : "?");
      klog_puts("' err=");
      klog_hex64(regs->err_code);
      klog_puts("\n");

      if (current->state == THREAD_DEAD || current->state == THREAD_ZOMBIE) {
        while (1) {
          sched_yield();
        }
      }

      process_do_exit(11); // SIGSEGV
    }
    klog_puts("[VMM] Segmentation fault at CR2=");
    klog_hex64(cr2);
    klog_puts(" RIP=");
    klog_hex64(regs->rip);
    klog_puts(" tid=");
    klog_uint64(current->tid);
    klog_puts(" comm='");
    klog_puts(current->comm);
    klog_puts("'\n");
    return PF_REJECT(user_mode
                         ? "no VMA covers the faulting address"
                         : "kernel address is not covered by a VMA, so there is "
                           "nothing to demand page (higher-half mapping missing?)",
                     cr2, 0);
  }

  // PROT_NONE enforcement — reserves address space but forbids all access.
  if (vma_prot == PROT_NONE) {
    return PF_REJECT("VMA covering the address is PROT_NONE", vma_start,
                     vma_end);
  }

  // Write to read-only VMA.
  if (write_fault && !(vma_prot & PROT_WRITE)) {
    return PF_REJECT("write to a read-only VMA", vma_prot, vma_start);
  }

  // SysV Shared Memory demand-paging
  if (vma_flags & MAP_SYSV_SHM) {
    uint32_t shmid = (uint32_t)vma_offset;
    uint64_t fault_page = cr2 & ~0xFFFULL;
    uint32_t page_idx = (uint32_t)((fault_page - vma_start) / PAGE_SIZE);

    uint64_t phys = shm_get_or_alloc_page(shmid, page_idx);
    if (!phys) {
      vma_linux_put(vma_linux);
      return PF_REJECT("SHM failed to get or allocate page", vma_start, vma_end);
    }

    spinlock_acquire(&current->mm->lock);
    struct vma *recheck = vma_find(&current->mm->vmas, fault_page);
    if (!recheck || !(recheck->flags & MAP_SYSV_SHM) ||
        (uint32_t)recheck->offset != shmid ||
        fault_page < recheck->start || fault_page >= recheck->end) {
      spinlock_release(&current->mm->lock);
      pmm_decref((void *)phys);
      vma_linux_put(vma_linux);
      return 0; // raced with munmap/shmdt, retry
    }

    uint64_t page_flags = dp_build_flags(recheck->prot);

    bool installed = vmm_map_page_if_unmapped((uint64_t *)target_cr3,
                                              fault_page, phys, page_flags);
    if (!installed) {
      pmm_decref((void *)phys);
    }
    spinlock_release(&current->mm->lock);
    vma_linux_put(vma_linux);
    return 0;
  }

  void *frame = NULL;
  bool cow_mapping = false;
  bool refault_mapping = false;
  vfs_node_t *node = (vfs_node_t *)vma_file_node;
  if (shmem_pin) {
    uint64_t page_offset = (cr2 & ~0xFFFULL) - vma_start;
    uint64_t file_offset = vma_offset + page_offset;
    uint64_t file_bytes = vma_file_size;
    bool private_tail = (vma_flags & MAP_ELF_LOAD) &&
                        (vma_flags & 3) == MAP_PRIVATE &&
                        (page_offset >= file_bytes ||
                         file_bytes - page_offset < PAGE_SIZE);
    if (private_tail) {
      /* ELF p_filesz can end inside a tmpfs storage page, before section
       * headers or other file contents. The remainder belongs to BSS, not
       * to the file: copying the whole storage page poisons global pointers.
       * Pure BSS pages must also work beyond the file's EOF. Never zero the
       * storage page itself, since other mappings still expose those bytes. */
      frame = vmm_alloc_page_numa(vma_numa_policy, vma_numa_nodemask,
                                 vma_numa_preferred_node, cr2);
      if (!frame)
        return PF_REJECT("PMM out of memory for private shmem tail", page_offset, file_bytes);
      page_zero(PHYS_TO_VIRT((uint64_t)frame));
      if (page_offset < file_bytes) {
        void *storage = (void *)tmpfs_fault_page(node, file_offset);
        if (!storage) {
          pmm_decref(frame);
          return PF_REJECT("shmem tail unavailable or past EOF", file_offset, node->length);
        }
        memcpy(PHYS_TO_VIRT((uint64_t)frame),
               PHYS_TO_VIRT((uint64_t)storage), file_bytes - page_offset);
        pmm_decref(storage);
      }
    } else {
      frame = (void *)tmpfs_fault_page(node, file_offset);
      if (!frame)
        return PF_REJECT("shmem page unavailable or past EOF", file_offset, node->length);
      cow_mapping = (vma_flags & 3) == MAP_PRIVATE;
    }
  } else if (node && (node->flags & FS_TYPE_MASK) == FS_FILE) {
    uint64_t page_offset = (cr2 & ~0xFFFULL) - vma_start;
    uint64_t eff_file_size = (vma_file_size > 0 || (vma_flags & MAP_ELF_LOAD))
                                 ? vma_file_size : (vma_end - vma_start);

    if (page_offset >= eff_file_size) {
      // 1. Pure BSS page (past eff_file_size) — share the global zero page on
      // read (CoW) instead of allocating a dedicated physical frame.
      if (!write_fault && (vma_flags & MAP_PRIVATE)) {
        uint64_t zp = pmm_get_zero_page_phys();
        if (zp) {
          uint64_t fault_page = cr2 & ~0xFFFULL;
          spinlock_acquire(&current->mm->lock);
          struct vma *w = vma_find(&current->mm->vmas, fault_page);
          if (w && (w->flags & MAP_PRIVATE) && w->file_node == node &&
              w->prot == vma_prot && w->flags == vma_flags) {
            uint64_t flags =
                (dp_build_flags(w->prot) & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
            uint64_t win_end = fault_page + 16 * PAGE_SIZE;
            if (win_end < fault_page || win_end > w->end)
              win_end = w->end;
            vmm_lock_acquire_for((uint64_t *)target_cr3, fault_page);
            for (uint64_t va2 = fault_page; va2 < win_end; va2 += PAGE_SIZE) {
              uint64_t po = va2 - w->start;
              if (po >= eff_file_size &&
                  !vmm_page_present_nolock((uint64_t *)target_cr3, va2)) {
                bool changed = false;
                vmm_map_page_locked((uint64_t *)target_cr3, va2, zp, flags,
                                    &changed);
              }
            }
            vmm_lock_release();
            spinlock_release(&current->mm->lock);
            vma_linux_put(vma_linux);
            return 0;
          }
          spinlock_release(&current->mm->lock);
          vma_linux_put(vma_linux);
          return 0;
        }
      }

      // Write fault: allocate a dedicated zero-filled page.
      frame = vmm_zero_pool_usable(vma_numa_policy) ? pmm_alloc_zeroed_page()
                                                    : NULL;
      if (!frame) {
        frame = vmm_alloc_page_numa(vma_numa_policy, vma_numa_nodemask, vma_numa_preferred_node, cr2);
        if (!frame)
          return PF_REJECT("PMM out of memory for a BSS page", page_offset,
                           eff_file_size);
        page_zero(PHYS_TO_VIRT((uint64_t)frame));
      }
    } else if ((vma_flags & MAP_PRIVATE) && (vma_prot & PROT_WRITE)) {
      // 2. Private Writable file mapping (e.g. ELF .data / .got / partial BSS)
      //
      //    A read fault maps the shared page-cache frame read-only with the CoW
      //    bit set instead of copying it.  The frame is then shared by every
      //    process mapping the same library, and the existing write-fault CoW
      //    path copies it on the first actual write.  A write fault takes the
      //    private copy immediately so the page is not faulted twice.
      //
      //    The page must lie entirely inside the segment's file-backed range:
      //    on a partial last page the cache frame holds the bytes that follow
      //    the segment in the file, while the mapping must expose zeros there,
      //    so those keep the copy-now path.
      uint32_t file_offset = (uint32_t)(vma_offset + page_offset);
      vfs_page_t *cached = vfs_cache_lookup(node, file_offset);
      bool irq_state = pf_enable_irqs_for_wait();
      if (cached) {
        // Fast path: verify page is ready without redundant lookup cycle
        bool ready = (!__atomic_load_n(&cached->loading, __ATOMIC_ACQUIRE) &&
                      cached->uptodate && cached->frame_phys);
        if (__builtin_expect(!ready, 0)) {
          spinlock_acquire(&node->pages_lock);
          ready = (!cached->loading && cached->uptodate && cached->frame_phys);
          spinlock_release(&node->pages_lock);
        }

        if (!ready) {
          vfs_cache_put(node, cached);
          cached = vfs_cache_get_or_create(node, file_offset);
        }
      } else {
        /* On-demand readahead: prefetch 128 KB window ahead of cold write/read fault */
        if (node && node->read && (node->flags & FS_TYPE_MASK) == FS_FILE) {
          (void)vfs_cache_readahead(node, file_offset, 128 * 1024);
        }
        cached = vfs_cache_get_or_create(node, file_offset);
      }
      pf_restore_irqs_after_wait(irq_state);

      if (!cached || !cached->frame_phys) {
        if (cached)
          vfs_cache_put(node, cached);
        klogf("[VMM] File cache read failure for VMA [%016llx-%016llx] off=%u\n",
              (unsigned long long)vma_start, (unsigned long long)vma_end,
              file_offset);
        return PF_REJECT("private file mapping: page cache could not supply the "
                         "page", file_offset, page_offset);
      }

      bool whole_page_in_file = (page_offset + PAGE_SIZE) <= eff_file_size;
      if (!write_fault && whole_page_in_file) {
        // Share the cached frame: read-only + CoW, no private frame, no memcpy.
        frame = (void *)cached->frame_phys;
        pmm_incref(frame);
        cow_mapping = true;
        refault_mapping = true;
      } else {
        // The page is about to be dirtied (or is the partial tail of the
        // segment): take the private copy now without redundant zeroing.
        frame = vmm_alloc_page_numa(vma_numa_policy, vma_numa_nodemask, vma_numa_preferred_node, cr2);
        if (!frame) {
          vfs_cache_put(node, cached);
          return PF_REJECT("PMM out of memory for a private file page",
                           page_offset, eff_file_size);
        }
        void *priv_virt = PHYS_TO_VIRT((uint64_t)frame);
        uint32_t valid_bytes = 4096;
        if (page_offset + 4096 > eff_file_size)
          valid_bytes = (uint32_t)(eff_file_size - page_offset);

        if (valid_bytes == PAGE_SIZE) {
          page_copy(priv_virt, PHYS_TO_VIRT(cached->frame_phys));
        } else {
          memcpy(priv_virt, PHYS_TO_VIRT(cached->frame_phys), valid_bytes);
          memset((uint8_t *)priv_virt + valid_bytes, 0, PAGE_SIZE - valid_bytes);
        }
      }
      vfs_cache_put(node, cached);

      // Map already-cached neighbours read-only + CoW so adjacent .data/.got
      // pages do not each take a separate fault (even on write faults).
      vmm_map_cow_cluster((uint64_t *)target_cr3, cr2, node, current->mm,
                          vma_start, vma_end, vma_offset, eff_file_size);
    } else {
      // 3. Shared or Read-Only file mapping (e.g. ELF .text / .rodata)
      //    Use shared VFS page-cache frame directly.
      uint32_t file_offset = (uint32_t)(vma_offset + page_offset);
      vfs_page_t *cached = vfs_cache_lookup(node, file_offset);
      bool irq_state = pf_enable_irqs_for_wait();
      if (cached) {
        // Fast path: verify page is ready without redundant lookup cycle
        bool ready = (!__atomic_load_n(&cached->loading, __ATOMIC_ACQUIRE) &&
                      cached->uptodate && cached->frame_phys);
        if (__builtin_expect(!ready, 0)) {
          spinlock_acquire(&node->pages_lock);
          ready = (!cached->loading && cached->uptodate && cached->frame_phys);
          spinlock_release(&node->pages_lock);
        }

        if (!ready) {
          vfs_cache_put(node, cached);
          cached = vfs_cache_get_or_create(node, file_offset);
        }
      } else {
        /* On-demand readahead: prefetch 128 KB window ahead of cold execution/read fault */
        if (node && node->read && (node->flags & FS_TYPE_MASK) == FS_FILE) {
          (void)vfs_cache_readahead(node, file_offset, 128 * 1024);
        }
        cached = vfs_cache_get_or_create(node, file_offset);
      }
      pf_restore_irqs_after_wait(irq_state);

      if (cached && cached->frame_phys) {
        frame = (void *)cached->frame_phys;
        pmm_incref(frame);
        refault_mapping = (vma_flags & MAP_PRIVATE) &&
                          page_offset + PAGE_SIZE <= eff_file_size;
        vfs_cache_put(node, cached);
      } else {
        if (cached)
          vfs_cache_put(node, cached);
        klogf("[VMM] File cache read failure for ro VMA [%016llx-%016llx] off=%u page_off=%llu eff_size=%llu vma_off=%llu file='%s' comm='%s' tid=%u rip=%016llx\n",
              (unsigned long long)vma_start, (unsigned long long)vma_end, file_offset,
              (unsigned long long)page_offset, (unsigned long long)eff_file_size,
              (unsigned long long)vma_offset,
              (node && node->name[0]) ? node->name : "?",
              (current && current->comm[0]) ? current->comm : "?",
              current ? current->tid : 0,
              (unsigned long long)(regs ? regs->rip : 0));
        return PF_REJECT("shared/read-only file mapping: page cache could not "
                         "supply the page", file_offset, page_offset);
      }

      // Proactive cluster mapping: map adjacent cached pages in a 128 KB window (32 pages)
      // to avoid dozens of redundant page faults during library / binary startup.
      // The window is revalidated against the current VMA under mm->lock so a
      // racing munmap/mprotect/mmap cannot alias a file frame into a different
      // (e.g. anonymous) mapping. This mirrors vmm_map_cow_cluster(): SMP
      // exposes the race that single-CPU execution hid.
      uint64_t cluster_vstart = cr2 & ~0x1FFFFULL;
      struct {
        uint64_t vpage;
        uint64_t phys;
      } batch[32];
      uint32_t batch_count = 0;

      spinlock_acquire(&node->pages_lock);
      for (int ci = 0; ci < 32; ci++) {
        uint64_t vpage = cluster_vstart + (uint64_t)(ci * 4096);
        if (vpage == (cr2 & ~0xFFFULL))
          continue;
        if (vpage < vma_start || vpage >= vma_end)
          continue;

        uint32_t foff = (uint32_t)(vma_offset + (vpage - vma_start));
        vfs_page_t *p = asc_radix_tree_lookup(&node->pages, (uint64_t)(foff >> 12));
        if (p && vfs_cache_page_valid(p) && !p->loading && p->uptodate &&
        p->frame_phys && !vfs_page_is_evicted(p)) {
          batch[batch_count].vpage = vpage;
          batch[batch_count].phys = p->frame_phys;
          pmm_incref((void *)p->frame_phys);
          batch_count++;
        }
      }
      spinlock_release(&node->pages_lock);

      if (batch_count > 0) {
        spinlock_acquire(&current->mm->lock);
        vmm_lock_acquire_for((uint64_t *)target_cr3, 0);
        for (uint32_t bi = 0; bi < batch_count; bi++) {
          struct vma *nv = vma_find(&current->mm->vmas, batch[bi].vpage);
          uint64_t nv_file_size =
              nv ? ((nv->file_size > 0 || (nv->flags & MAP_ELF_LOAD)) ? nv->file_size : (nv->end - nv->start)) : 0;
          uint64_t nv_page_off = (nv && batch[bi].vpage >= nv->start)
                                     ? (batch[bi].vpage - nv->start)
                                     : UINT64_MAX;
          bool ok =
              nv && (nv->prot & PROT_READ) && batch[bi].vpage >= nv->start &&
              batch[bi].vpage + PAGE_SIZE <= nv->end &&
              nv_page_off + PAGE_SIZE <= nv_file_size &&
              (void *)nv->file_node == (void *)node &&
              (uint32_t)(nv->offset + nv_page_off) ==
                  (uint32_t)(vma_offset + (batch[bi].vpage - vma_start));
          uint64_t page_flags = ok ? dp_build_flags(nv->prot) : 0;
          /* The fault started in a read-only VMA, but mprotect may have made
           * this neighbour writable while the file cache was being filled.
           * Private mappings must never expose a writable cache frame: retain
           * CoW using the permissions revalidated under mm->lock. */
          if (ok && (nv->flags & MAP_PRIVATE))
            page_flags = (page_flags & ~PAGE_FLAG_RW) |
                         PAGE_FLAG_COW | PAGE_FLAG_REFAULT;
          if (!ok) {
            pmm_decref((void *)batch[bi].phys);
            continue;
          }
          bool changed = false;
          bool installed =
              !vmm_page_present_nolock((uint64_t *)target_cr3, batch[bi].vpage) &&
              vmm_map_page_locked((uint64_t *)target_cr3, batch[bi].vpage,
                                  batch[bi].phys, page_flags, &changed);
          if (!installed)
            pmm_decref((void *)batch[bi].phys);
        }
        vmm_lock_release();
        spinlock_release(&current->mm->lock);
      }
    }

  } else {
    // ---- Anonymous zero-fill-on-demand -------------------------------------

    // Try to satisfy the fault with a 2 MB huge page when the VMA requests it.
    // All four conditions must hold for a huge page to be installed:
    //   1. VMA is flagged MAP_HUGEPAGE (set by madvise or MAP_HUGETLB)
    //   2. The 2 MB-aligned base of the fault address falls inside the VMA
    //   3. The VMA covers the entire 2 MB region starting at that base
    //   4. PMM can hand us a 2 MB-aligned physical block (order-9 buddy)
    //
    // If any condition fails we fall through to the ordinary 4 KB path.
    if (vma_wants_huge(vma_flags, write_fault)) {
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)
      uint64_t hp_base = cr2 & ~(HUGE_PAGE_SIZE - 1ULL); // 2 MB-align down
      /* Skip the 2 MB allocate+zero when a page table already covers the
       * range (earlier 4 KB faults, a split): vmm_map_huge_page() would
       * refuse it and every later fault here would repeat the waste. */
      if (hp_base >= vma_start && hp_base + HUGE_PAGE_SIZE <= vma_end &&
          vmm_huge_slot_free((uint64_t *)target_cr3, hp_base)) {
        void *huge_phys = vmm_alloc_huge_page_numa(vma_numa_policy, vma_numa_nodemask, vma_numa_preferred_node, cr2);
        if (huge_phys) {
          // Zero the entire 2 MB frame.
          page_zero_huge((void *)((uint64_t)huge_phys + pmm_get_hhdm_offset()));

          /* Allocation/zeroing ran without mm->lock. Revalidate before
           * publishing, just like the ordinary 4 KB fault path: a peer may
           * have replaced this range with a file or driver mapping. */
          spinlock_acquire(&current->mm->lock);
          struct vma *w = vma_find(&current->mm->vmas, cr2);
          bool valid = w && vma_wants_huge(w->flags, write_fault) &&
                       !w->file_node &&
                       !w->linux_vma && w->prot == vma_prot &&
                       w->flags == vma_flags && hp_base >= w->start &&
                       hp_base + HUGE_PAGE_SIZE <= w->end;
          bool installed = valid &&
              vmm_map_huge_page((uint64_t *)target_cr3, hp_base,
                                (uint64_t)huge_phys, dp_build_flags(w->prot));
          spinlock_release(&current->mm->lock);
          if (installed) {
            // Success — the whole 2 MB region is now mapped.
            return 0;
          }
          // Map failed (e.g. OOM for intermediate page table) — free and
          // fall through to the 4 KB path below.
          pmm_free_pages(huge_phys, 512);
          if (!valid)
            return 0; // Retry against the replacement VMA.
        }
        // PMM OOM for 2 MB block — fall through to 4 KB allocation.
      }
#undef HUGE_PAGE_SIZE
    }

    // Zero-Page Sharing: a read fault on a private anonymous mapping shares
    // the global zero page (read-only + COW) instead of allocating a frame.
    // The first write to the page breaks CoW via the dedicated zero-page path
    // above (fresh zeroed frame, never a write to the shared page). This is
    // the standard demand-paging tradeoff: read-mostly pages cost one light
    // fault with no allocation or zeroing, while read-then-write pages pay a
    // second (CoW-break) fault. Write faults skip this and allocate directly.
    //
    // Fault-around: the faulting page is ~370 ns of fixed cost (entry/exit,
    // two mm->lock sections, two VMA lookups) even with nothing to allocate,
    // so mapping one page per fault caps a 4 MB scan at ~380 us. Like Linux
    // (fault_around_bytes = 64K), one fault maps up to the next 16 pages in
    // the same VMA. Zero pages consume no frames, so over-mapping is free —
    // and every eliminated fault saves the whole fixed cost.
#define FAULT_AROUND_PAGES 16
    if (!write_fault && (vma_flags & MAP_PRIVATE) &&
        (vma_flags & MAP_ANONYMOUS)) {
      uint64_t zp = pmm_get_zero_page_phys();
      if (zp) {
        uint64_t fault_page = cr2 & ~0xFFFULL;
        /* One lock hold per 16 pages, not one per page: the per-page
         * vmm_lock round-trip (CLI/STI + MFENCE + drain attempt) was
         * ~130 ns x 16, most of the fault-around cost. Lock order
         * mm->lock -> vmm_lock matches the CoW path above, and the window
         * is clamped to the VMA found under the same hold, so a racing
         * munmap/mprotect cannot leave zero pages outside the VMA. */
        spinlock_acquire(&current->mm->lock);
        struct vma *w = vma_find(&current->mm->vmas, fault_page);
        if (w && (w->flags & MAP_PRIVATE) &&
            (w->flags & MAP_ANONYMOUS) && !w->file_node && !w->linux_vma &&
            w->prot == vma_prot && w->flags == vma_flags) {
          uint64_t flags =
              (dp_build_flags(w->prot) & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
          uint64_t win_end = fault_page + (uint64_t)FAULT_AROUND_PAGES * PAGE_SIZE;
          if (win_end < fault_page || win_end > w->end)
            win_end = w->end;
          vmm_lock_acquire_for((uint64_t *)target_cr3, fault_page);
          for (uint64_t va2 = fault_page; va2 < win_end;
               va2 += PAGE_SIZE) {
            if (!vmm_page_present_nolock((uint64_t *)target_cr3, va2)) {
              bool changed = false;
              vmm_map_page_locked((uint64_t *)target_cr3, va2, zp, flags,
                                  &changed);
            }
          }
          vmm_lock_release();
          spinlock_release(&current->mm->lock);
          return 0;
        }
        spinlock_release(&current->mm->lock);
        /* Never fall back to an unlocked install using the old VMA's
         * permissions. Retry the instruction against its current mapping. */
        return 0;
      }
    }
#undef FAULT_AROUND_PAGES

    // 4 KB fallback (always correct, also handles non-huge VMAs).
    // Prefer an idle-zeroed frame; fall back to alloc+zero inline.
    bool pre_zeroed = false;
    frame = vmm_zero_pool_usable(vma_numa_policy) ? pmm_alloc_zeroed_page()
                                                  : NULL;
    if (frame) {
      pre_zeroed = true;
    } else {
      frame = vmm_alloc_page_numa(vma_numa_policy, vma_numa_nodemask, vma_numa_preferred_node, cr2);
    }

    if (!frame) {
      // Reclaim before giving up: deferred frees may be parked on the drain
      // queue and clean file-cache pages are droppable. pmm_pcp_drain_all()
      // returns the zero pool plus every CPU's parked frames to the buddy
      // lists — a fault allocation can otherwise fail while those frames
      // still count as free. Failing here kills a userspace thread, so one
      // bounded recovery pass is worth it.
      vmm_drain_deferred_work();
      pmm_pcp_drain_all();
      mm_file_cache_reclaim(64);
      frame = vmm_alloc_page_numa(vma_numa_policy, vma_numa_nodemask, vma_numa_preferred_node, cr2);
    }

    if (!frame) {
      /* Escalated pass: the first pass only returns parked frames and the
       * current mm's file cache. Under desktop pressure the bulk of the
       * reclaimable memory sits elsewhere (TTM/driver pools, empty kernel
       * slabs, files no longer mapped in this mm), so ask every subsystem
       * that can hand pages back before declaring OOM. IRQs are opened
       * around the reclaim: none of it runs from a nested hard-IRQ fault,
       * and shrinkers may wait on locks held by interrupt handlers. */
      bool irq_state = pf_enable_irqs_for_wait();
      if (heap_oom_reclaim)
        heap_oom_reclaim();
      if (linuxkpi_shrink_reclaim)
        linuxkpi_shrink_reclaim(0, 512);
      mm_file_cache_reclaim(2048);
      pmm_pcp_drain_all();
      pf_restore_irqs_after_wait(irq_state);
      frame = vmm_alloc_page_numa(vma_numa_policy, vma_numa_nodemask, vma_numa_preferred_node, cr2);
    }

    if (!frame) {
      size_t free_buddy = pmm_get_free_pages();
      size_t free_all = pmm_get_free_pages_including_pcp();
      klogf("[VMM] OOM demand-page alloc=4KB cr2=%016llx page=%016llx "
            "rip=%016llx rsp=%016llx err=%016llx vma=[%016llx-%016llx) "
            "prot=%llx flags=%llx huge=%u free_pages=%llu free_pages_pcp=%llu "
            "tid=%u comm='%s' mode=%s\n",
            (unsigned long long)cr2,
            (unsigned long long)(cr2 & ~0xFFFULL),
            (unsigned long long)(regs ? regs->rip : 0),
            (unsigned long long)(regs ? regs->rsp : 0),
            (unsigned long long)error_code,
            (unsigned long long)vma_start,
            (unsigned long long)vma_end,
            (unsigned long long)vma_prot,
            (unsigned long long)vma_flags,
            (vma_flags & MAP_HUGEPAGE) ? 1U : 0U,
            (unsigned long long)free_buddy,
            (unsigned long long)free_all,
            current ? current->tid : 0,
            (current && current->comm[0]) ? current->comm : "?",
            user_mode ? "user" : "kernel");
      if (user_mode) {
        /* Kill the whole process, not just the faulting thread. A lone dead
         * thread leaves every lock it held and every sibling in an
         * undefined state (2026-10-16: a dead KDE TaskController thread and
         * a Firefox content process kept running around the corpse), and
         * the address space's memory is not released until a thread that
         * does not exist finishes exiting. This matches the fatal-signal
         * default action and Linux's fault-path OOM handling. */
        sched_terminate_thread_group(current);
        sched_terminate_thread(current->tid);
        return 0;
      }
      return PF_REJECT("PMM out of memory while demand paging a 4KB page",
                       vma_start, vma_end);
    }
    if (!pre_zeroed)
      page_zero(PHYS_TO_VIRT((uint64_t)frame));
  }


  // ---- Map the faulting page ----------------------------------------------

  // The frame was prepared without mm->lock (file I/O is slow). Another
  // thread may have munmapped and reused this range meanwhile. Merely checking
  // that *some* VMA still covers the address is insufficient: it could now be
  // a different anonymous/file mapping, and installing this frame would
  // silently corrupt the new mapping (including allocator metadata).
  spinlock_acquire(&current->mm->lock);
  struct vma *recheck = vma_find(&current->mm->vmas, cr2);
  uint64_t page_vaddr = cr2 & PAGE_MASK;
  uint64_t original_page_offset =
      vma_offset + (page_vaddr >= vma_start ? page_vaddr - vma_start : 0);
  uint64_t current_page_offset =
      recheck && page_vaddr >= recheck->start
          ? recheck->offset + (page_vaddr - recheck->start)
          : UINT64_MAX;
  bool same_mapping =
      recheck && recheck->prot == vma_prot && recheck->flags == vma_flags &&
      recheck->fd == vma_fd && current_page_offset == original_page_offset &&
      recheck->file_size == vma_file_size &&
      recheck->file_node == vma_file_node && recheck->linux_vma == vma_linux;
  if (!same_mapping) {
    spinlock_release(&current->mm->lock);
    if (!node) {
      pmm_free_page(frame);
    } else {
      pmm_decref((void *)frame);
    }
    /* The fault raced with munmap/mmap/mprotect while the frame was being
     * prepared.  Do not turn that transient race into SIGSEGV: retry the
     * instruction so the page-fault path resolves the address against the
     * current VMA (or rejects it if it is really unmapped/protected). */
    vma_linux_put(vma_linux);
    return 0;
  }

  uint64_t flags = dp_build_flags(vma_prot);
  if (cow_mapping)
    flags = (flags & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
  if (refault_mapping)
    flags |= PAGE_FLAG_REFAULT;
  uint64_t vpage = cr2 & ~0xFFFULL;
  bool mapped = vmm_map_page_if_unmapped((uint64_t *)target_cr3, vpage,
                                         (uint64_t)frame, flags);
  uint64_t existing_phys =
      mapped ? 0 : vmm_virt_to_phys((uint64_t *)target_cr3, vpage);
  spinlock_release(&current->mm->lock);
  if (!mapped) {
    // If another thread already mapped this page while we prepared the frame:
    if (existing_phys != 0) {
      if (!node) {
        pmm_free_page(frame);
      } else {
        pmm_decref((void *)frame);
      }
      tlb_shootdown_page(vpage);
      return 0; // Handled concurrently by another thread!
    }

    if (!node) {
      pmm_free_page(frame);
    } else {
      pmm_decref((void *)frame);
    }
    klog_puts("[VMM] Fatal PT alloc failure in paging engine\n");
    if (user_mode) {
      sched_terminate_thread(current->tid);
      return 0;
    }
    return PF_REJECT("no memory left for an intermediate page table", vpage,
                     flags);
  }

  return 0;
}

void vmm_map_signal_trampoline(uint64_t *pml4) {
  extern uint64_t signal_trampoline_phys;
  if (signal_trampoline_phys == 0)
    return;
  vmm_map_page(pml4, 0x00007FFFFFFFF000ULL, signal_trampoline_phys,
               PAGE_FLAG_USER);
}

// ─────────────────────────────────────────────────────────────────────────────
// vsyscall page — mapped at 0xFFFFFFFFFF600000 in every user process.
// Linux maps this fixed page so glibc/JVM can call gettimeofday, time, and
// getcpu without a full syscall.  HotSpot specifically hard-codes a read/call
// to 0xFFFFFFFFFF600800 (getcpu) to determine the CPU number for TLAB
// allocation.  Without this mapping the JVM crashes with SIGSEGV.
// ─────────────────────────────────────────────────────────────────────────────
#define VSYSCALL_BASE 0xFFFFFFFFFF600000ULL

static uint64_t vsyscall_page_phys = 0;

extern uint8_t vdso_blob_start[];
extern uint8_t vdso_blob_end[];

void vmm_update_vdso_data(void) {
  if (vsyscall_page_phys == 0)
    return;

  extern uint64_t tsc_get_freq_khz(void);
  extern uint64_t lapic_timer_get_boot_tsc(void);
  extern uint64_t rtc_get_boot_timestamp(void);

  uint64_t khz = tsc_get_freq_khz();
  uint64_t boot_tsc = lapic_timer_get_boot_tsc();
  uint64_t boot_sec = rtc_get_boot_timestamp();
  uint64_t tsc_hz = khz * 1000ULL;

  uint32_t sec_shift = 20;
  uint64_t sec_mult = 0;
  uint64_t mult_rem_ns = 0;
  uint64_t mult_rem_us = 0;

  if (tsc_hz > 0) {
    __uint128_t num = ((__uint128_t)1 << (64 + sec_shift));
    sec_mult = (uint64_t)(num / tsc_hz) + 1;
    mult_rem_ns = (1000000000ULL << 32) / tsc_hz;
    mult_rem_us = (1000000ULL << 32) / tsc_hz;
  }

  extern bool cpu_has_rdpid(void);
  extern bool cpu_has_rdtscp(void);
  uint64_t getcpu_mode = cpu_has_rdpid() ? 2ULL : (cpu_has_rdtscp() ? 1ULL : 0ULL);

  uint64_t *data = (uint64_t *)(PHYS_TO_VIRT(vsyscall_page_phys) + 0xE00);
  data[0] = boot_tsc;
  data[1] = khz;
  data[2] = boot_sec;
  data[3] = tsc_hz;
  data[4] = sec_mult;
  data[5] = (uint64_t)sec_shift;
  data[6] = mult_rem_ns;
  data[7] = mult_rem_us;
  data[8] = getcpu_mode;
}


void vmm_init_vsyscall_page(void) {
  if (vsyscall_page_phys != 0)
    return; // already done

  void *page = pmm_alloc_page();
  if (!page)
    return;

  vsyscall_page_phys = (uint64_t)page;
  uint8_t *v = (uint8_t *)PHYS_TO_VIRT(page);

  // Copy compiled vDSO assembly blob into the page
  size_t blob_len = (size_t)(vdso_blob_end - vdso_blob_start);
  if (blob_len > 0xE00)
    blob_len = 0xE00;
  memcpy(v, vdso_blob_start, blob_len);

  /* i386 fallback restorers live on this read-only executable shared page,
   * never on an executable user stack. Legacy sigreturn pops the signal
   * argument before entering the kernel; rt_sigreturn does not. */
  static const uint8_t compat_return[] = {0x58, 0xb8, 119, 0, 0, 0, 0xcd, 0x80};
  static const uint8_t compat_rt_return[] = {0xb8, 173, 0, 0, 0, 0xcd, 0x80};
  memcpy(v + 0xF00, compat_return, sizeof(compat_return));
  memcpy(v + 0xF10, compat_rt_return, sizeof(compat_rt_return));

  vmm_update_vdso_data();
}


uint64_t vmm_get_vsyscall_page_phys(void) {
  return vsyscall_page_phys;
}

void vmm_map_vsyscall_page(uint64_t *pml4) {
  if (vsyscall_page_phys == 0)
    return;
  // Map legacy vsyscall page (0xFFFFFFFFFF600000) for HotSpot getcpu stub
  vmm_map_page(pml4, VSYSCALL_BASE, vsyscall_page_phys, PAGE_FLAG_USER);
  // Map standard ELF vDSO page (0x700000000000) for musl/glibc dynamic linker
  vmm_map_page(pml4, VDSO_USER_BASE, vsyscall_page_phys, PAGE_FLAG_USER);
}
