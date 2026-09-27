
#include "../arch/x86_64/extable.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "pmm.h"
#include "tlb_shootdown.h"
#include "vma.h"
#include "vmm.h"
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

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
    vfs_page_t *p = radix_tree_lookup(&node->pages, (uint64_t)(file_off >> 12));
    if (p && !p->loading && p->uptodate && p->frame_phys && !p->evicted) {
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
  for (uint32_t i = 0; i < batch_count; i++) {
    struct vma *nv = vma_find(&mm->vmas, batch[i].vpage);
    uint64_t nv_file_size =
        nv ? ((nv->file_size > 0) ? nv->file_size : (nv->end - nv->start)) : 0;
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

    uint64_t flags = (dp_build_flags(nv->prot) & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
    if (!vmm_map_page_if_unmapped(pml4, batch[i].vpage, batch[i].phys, flags))
      pmm_decref((void *)batch[i].phys);
  }
  spinlock_release(&mm->lock);
}

/* ---- Why the paging engine refused a fault --------------------------------
 *
 * A -1 is a death sentence for a kernel-mode fault, so every bail-out in
 * vmm_handle_page_fault() annotates itself and page_fault_handler() prints the
 * record through kpf_dump.c while the machine can still talk.
 *
 * Plain stores only: this runs on the fault path, where taking a lock or
 * allocating can fault again or spin forever on a ticket lock this CPU already
 * holds. A record torn by a rejection on another CPU only garbles one line on a
 * machine that is about to panic anyway.
 */
static struct vmm_fault_reject pf_reject;
static volatile uint32_t pf_reject_seq;

static int pf_reject_record(uint64_t cr2, uint64_t error_code,
                            const struct registers *regs,
                            const struct thread *current, const char *reason,
                            const char *file, uint32_t line, uint64_t detail,
                            uint64_t detail2) {
  pf_reject.reason = reason;
  pf_reject.file = file;
  pf_reject.line = line;
  pf_reject.cr2 = cr2;
  pf_reject.err_code = error_code;
  pf_reject.rip = regs ? regs->rip : 0;
  pf_reject.detail = detail;
  pf_reject.detail2 = detail2;
  pf_reject.tid = current ? current->tid : 0;
  pf_reject.seq = pf_reject_seq + 1;
  pf_reject_seq = pf_reject.seq;
  return -1;
}

/* Only usable inside vmm_handle_page_fault(), whose locals it borrows. */
#define PF_REJECT(reason, detail, detail2)                                     \
  pf_reject_record(cr2, error_code, regs, current, (reason), __FILE__,         \
                   __LINE__, (uint64_t)(detail), (uint64_t)(detail2))

bool vmm_get_last_fault_reject(struct vmm_fault_reject *out) {
  if (!out)
    return false;
  uint32_t seq = pf_reject_seq;
  if (seq == 0)
    return false;
  *out = pf_reject;
  return out->seq == seq;
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
    vmm_lock_acquire();

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
      struct vma *v = vma_find(&current->mm->vmas, cr2);
      if (v) {
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
        void *new_phys = pmm_alloc_page();
        if (!new_phys)
          WALK_REJECT("CoW break of the shared zero page: PMM out of "
                      "memory", old_phys, 0);

        memset(PHYS_TO_VIRT((uint64_t)new_phys), 0, PAGE_SIZE);

        *pte = ((uint64_t)new_phys & PAGE_MASK) |
               (*pte & ~PAGE_MASK & ~PAGE_FLAG_COW) | PAGE_FLAG_RW;
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
        void *new_phys = pmm_alloc_page();
        if (!new_phys)
          WALK_REJECT("CoW copy: PMM out of memory", old_phys, refs);

        memcpy(PHYS_TO_VIRT((uint64_t)new_phys), PHYS_TO_VIRT(old_phys),
               PAGE_SIZE);

        *pte = ((uint64_t)new_phys & PAGE_MASK) |
               (*pte & ~PAGE_MASK & ~PAGE_FLAG_COW) | PAGE_FLAG_RW;
        // Released after the flush below, not here.
        cow_old_phys = old_phys;
      } else {
        // Sole owner — just make it writable in place.
        *pte &= ~PAGE_FLAG_COW;
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

      vma_remove(&current->mm->vmas, old_start, old_end);
      if (vma_add(&current->mm->vmas, new_start, old_end, prot, flags, fd,
                  offset, NULL, 0) != 0) {
        klog_puts("[VMM] Stack expansion failed (overlap?) for CR2=");
        klog_hex64(cr2);
        klog_puts("\n");
        vma_add(&current->mm->vmas, old_start, old_end, prot, flags, fd, offset,
                NULL, 0);
        vma = NULL;
      } else {
        vma = vma_find(&current->mm->vmas, cr2);
      }
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
  void *vma_file_node = NULL;
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
  }
  spinlock_release(&current->mm->lock);

  if (!vma) {
    // If cr2 is in page 0 (0x0 .. 0xFFF) and it is a user-mode read fault in butterscotch,
    // map the global shared zero page (read-only, non-executable) so reading null strings
    // in writeEscapedString reads zeroes without crashing.
    if (!write_fault && !exec_fault && user_mode && (cr2 < PAGE_SIZE)) {
      if (current->comm && strstr(current->comm, "butterscotch")) {
        uint64_t zp = pmm_get_zero_page_phys();
        if (zp) {
          uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_NX;
          if (vmm_map_page((uint64_t *)target_cr3, 0, zp, flags)) {
            return 0; // Handled butterscotch null-pointer read fault
          }
        }
      }
    }

    // No VMA covers this address — genuine segfault.
    if (user_mode) {
      void *handler = (void *)current->signal_handlers[10].sa_handler;
      bool has_custom_handler = (handler != NULL && handler != (void *)1);
      if (!has_custom_handler) {
        klog_puts("[VMM] No VMA for CR2=");
        klog_hex64(cr2);
        klog_puts(" RIP=");
        klog_hex64(regs->rip);
        klog_puts(" process '");
        klog_puts(current->comm);
        klog_puts("' (tid=");
        klog_uint64(current->tid);
        klog_puts(")\n[VMM] Active VMAs:\n");
        vma_dump(&current->mm->vmas);
      }
      return PF_REJECT("no VMA covers the faulting address", cr2, 0);
    }

    if (!user_mode && cr2 <= USER_SPACE_LIMIT) {
      if (regs && extable_has_entry(regs->rip)) {
        return PF_REJECT("kernel read of a user address at a guarded (extable) "
                         "instruction", cr2, regs->rip);
      }
      klog_puts("\n" KLOG_CLR_RED "[ FATAL ]" KLOG_CLR_RESET
                " KERNEL-MODE FAULT on user address\n");
      klog_puts("          CR2:  ");
      klog_hex64(cr2);
      klog_puts("\n");
      klog_puts("          RIP:  ");
      klog_hex64(regs->rip);
      klog_puts("\n");
      klog_puts("          TID:  ");
      klog_uint64(current->tid);
      klog_puts(" COMM: '");
      klog_puts((current->comm[0]) ? current->comm : "?");
      klog_puts("'\n\n");

      klog_puts("      RAX: ");
      klog_hex64(regs->rax);
      klog_puts(" RBX: ");
      klog_hex64(regs->rbx);
      klog_puts("\n");
      klog_puts("      RCX: ");
      klog_hex64(regs->rcx);
      klog_puts(" RDX: ");
      klog_hex64(regs->rdx);
      klog_puts("\n");
      klog_puts("      RSI: ");
      klog_hex64(regs->rsi);
      klog_puts(" RDI: ");
      klog_hex64(regs->rdi);
      klog_puts("\n");
      klog_puts("      RBP: ");
      klog_hex64(regs->rbp);
      klog_puts(" RSP: ");
      klog_hex64(regs->rsp);
      klog_puts("\n");
      klog_puts("      R8:  ");
      klog_hex64(regs->r8);
      klog_puts(" R9:  ");
      klog_hex64(regs->r9);
      klog_puts("\n");
      klog_puts("      R10: ");
      klog_hex64(regs->r10);
      klog_puts(" R11: ");
      klog_hex64(regs->r11);
      klog_puts("\n");
      klog_puts("      R12: ");
      klog_hex64(regs->r12);
      klog_puts(" R13: ");
      klog_hex64(regs->r13);
      klog_puts("\n");
      klog_puts("      R14: ");
      klog_hex64(regs->r14);
      klog_puts(" R15: ");
      klog_hex64(regs->r15);
      klog_puts("\n");

      process_do_exit(11); // SIGSEGV
    }
    klog_puts("[VMM] Segmentation fault at CR2=");
    klog_hex64(cr2);
    klog_puts("\n[VMM] Active VMAs:\n");
    vma_dump(&current->mm->vmas);
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

  void *frame = NULL;
  bool cow_mapping = false;
  vfs_node_t *node = (vfs_node_t *)vma_file_node;
  if (node && (node->flags & FS_TYPE_MASK) == FS_FILE) {
    uint64_t page_offset = (cr2 & ~0xFFFULL) - vma_start;
    uint64_t eff_file_size = (vma_file_size > 0) ? vma_file_size : (vma_end - vma_start);

    if (page_offset >= eff_file_size) {
      // 1. Pure BSS page (past eff_file_size) — allocate zero-filled page
      frame = pmm_alloc_page();
      if (!frame)
        return PF_REJECT("PMM out of memory for a BSS page", page_offset,
                         eff_file_size);
      memset(PHYS_TO_VIRT((uint64_t)frame), 0, 4096);
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
      if (!cached) {
        // Clustered 128 KB read-ahead into VFS page cache (32 pages)
        vfs_cache_readahead(node, file_offset, 128 * 1024);
      } else {
        vfs_cache_put(node, cached);
      }
      cached = vfs_cache_get_or_create(node, file_offset);

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
      } else {
        // The page is about to be dirtied (or is the partial tail of the
        // segment): take the private zero-filled copy now.
        frame = pmm_alloc_page();
        if (!frame) {
          vfs_cache_put(node, cached);
          return PF_REJECT("PMM out of memory for a private file page",
                           page_offset, eff_file_size);
        }
        void *priv_virt = PHYS_TO_VIRT((uint64_t)frame);
        memset(priv_virt, 0, 4096);

        uint32_t valid_bytes = 4096;
        if (page_offset + 4096 > eff_file_size)
          valid_bytes = (uint32_t)(eff_file_size - page_offset);

        memcpy(priv_virt, PHYS_TO_VIRT(cached->frame_phys), valid_bytes);
      }
      vfs_cache_put(node, cached);

      // Map the rest of the cached 128 KB window read-only + CoW in one pass so
      // adjacent .data/.got pages do not each take a separate fault.
      if (cow_mapping)
        vmm_map_cow_cluster((uint64_t *)target_cr3, cr2, node, current->mm,
                            vma_start, vma_end, vma_offset, eff_file_size);
    } else {
      // 3. Shared or Read-Only file mapping (e.g. ELF .text / .rodata)
      //    Use shared VFS page-cache frame directly.
      uint32_t file_offset = (uint32_t)(vma_offset + page_offset);
      vfs_page_t *cached = vfs_cache_lookup(node, file_offset);
      if (cached) {
        // Fast path: verify page is ready without redundant lookup cycle
        bool ready = false;
        spinlock_acquire(&node->pages_lock);
        if (!cached->loading && cached->uptodate && cached->frame_phys)
          ready = true;
        spinlock_release(&node->pages_lock);

        if (!ready) {
          vfs_cache_put(node, cached);
          cached = vfs_cache_get_or_create(node, file_offset);
        }
      } else {
        // Cache miss: clustered 128 KB read-ahead starting at current fault
        vfs_cache_readahead(node, file_offset, 128 * 1024);
        cached = vfs_cache_get_or_create(node, file_offset);
      }

      if (cached && cached->frame_phys) {
        frame = (void *)cached->frame_phys;
        pmm_incref(frame);
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
              regs ? regs->rip : 0);
        return PF_REJECT("shared/read-only file mapping: page cache could not "
                         "supply the page", file_offset, page_offset);
      }

      // Proactive cluster mapping: map adjacent cached pages in a 128 KB window (32 pages)
      // to avoid dozens of redundant page faults during library / binary startup.
      uint64_t cluster_vstart = cr2 & ~0x1FFFFULL;
      uint64_t pt_flags = dp_build_flags(vma_prot);
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
        vfs_page_t *p = radix_tree_lookup(&node->pages, (uint64_t)(foff >> 12));
        if (p && !p->loading && p->uptodate && p->frame_phys && !p->evicted) {
          batch[batch_count].vpage = vpage;
          batch[batch_count].phys = p->frame_phys;
          pmm_incref((void *)p->frame_phys);
          batch_count++;
        }
      }
      spinlock_release(&node->pages_lock);

      for (uint32_t bi = 0; bi < batch_count; bi++) {
        if (!vmm_map_page_if_unmapped((uint64_t *)target_cr3, batch[bi].vpage,
                                      batch[bi].phys, pt_flags)) {
          pmm_decref((void *)batch[bi].phys);
        }
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
    if (vma_flags & MAP_HUGEPAGE) {
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)
      uint64_t hp_base = cr2 & ~(HUGE_PAGE_SIZE - 1ULL); // 2 MB-align down
      if (hp_base >= vma_start && hp_base + HUGE_PAGE_SIZE <= vma_end) {
        void *huge_phys = pmm_alloc_huge_page();
        if (huge_phys) {
          // Zero the entire 2 MB frame.
          memset((void *)((uint64_t)huge_phys + pmm_get_hhdm_offset()),
                 0, HUGE_PAGE_SIZE);

          uint64_t hp_flags = dp_build_flags(vma_prot);
          if (vmm_map_huge_page((uint64_t *)target_cr3, hp_base,
                                (uint64_t)huge_phys, hp_flags)) {
            klog_puts("[THP] Installed 2MB huge page at VA ");
            klog_hex64(hp_base);
            klog_puts("\n");
            // Success — the whole 2 MB region is now mapped.
            return 0;
          }
          // Map failed (e.g. OOM for intermediate page table) — free and
          // fall through to the 4 KB path below.
          pmm_free_pages(huge_phys, 512);
        }
        // PMM OOM for 2 MB block — fall through to 4 KB allocation.
      }
#undef HUGE_PAGE_SIZE
    }

    // Zero-Page Sharing: If this is a read fault on a private anonymous mapping
    // that is strictly read-only, map the global shared zero page (read-only + COW).
    // Writable anonymous mappings (heap/stack) directly allocate private frames to
    // eliminate COW overhead and race conditions between concurrent threads.
    if (!write_fault && (vma_flags & MAP_PRIVATE) && !(vma_prot & PROT_WRITE)) {
      uint64_t zp = pmm_get_zero_page_phys();
      if (zp) {
        uint64_t flags = (dp_build_flags(vma_prot) & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
        if (vmm_map_page_if_unmapped((uint64_t *)target_cr3, cr2 & ~0xFFFULL, zp, flags)) {
          return 0; // Zero page mapped! No physical allocation or zeroing needed.
        }
        if (vmm_virt_to_phys((uint64_t *)target_cr3, cr2 & ~0xFFFULL) != 0) {
          return 0; // Concurrently mapped by another thread
        }
      }
    }

    // 4 KB fallback (always correct, also handles non-huge VMAs).
    frame = pmm_alloc_page();

    if (!frame) {
      klog_puts("[VMM] OOM during demand paging!\n");
      if (user_mode) {
        sched_terminate_thread(current->tid);
        return 0;
      }
      return PF_REJECT("PMM out of memory while demand paging a 4KB page",
                       vma_start, vma_end);
    }
    memset(PHYS_TO_VIRT((uint64_t)frame), 0, 4096);
  }


  // ---- Map the faulting page ----------------------------------------------

  // The frame was prepared without mm->lock (file I/O is slow). Another
  // thread may have munmapped this range meanwhile. Mapping into a VA
  // with no VMA creates a phantom page: the app reads zeros instead of
  // faulting, which corrupts heap metadata (e.g. musl malloc asserts).
  spinlock_acquire(&current->mm->lock);
  struct vma *recheck = vma_find(&current->mm->vmas, cr2);
  spinlock_release(&current->mm->lock);
  if (!recheck) {
    if (!node) {
      pmm_free_page(frame);
    } else {
      pmm_decref((void *)frame);
    }
    return PF_REJECT("VMA disappeared while the frame was being prepared "
                     "(racing munmap or mprotect)", cr2, vma_start);
  }

  uint64_t flags = dp_build_flags(vma_prot);
  if (cow_mapping)
    flags = (flags & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
  uint64_t vpage = cr2 & ~0xFFFULL;
  if (!vmm_map_page_if_unmapped((uint64_t *)target_cr3, vpage, (uint64_t)frame,
                                flags)) {
    // If another thread already mapped this page while we prepared the frame:
    if (vmm_virt_to_phys((uint64_t *)target_cr3, vpage) != 0) {
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

  uint64_t *data = (uint64_t *)(PHYS_TO_VIRT(vsyscall_page_phys) + 0xE00);
  data[0] = boot_tsc;
  data[1] = khz;
  data[2] = boot_sec;
  data[3] = tsc_hz;
  data[4] = sec_mult;
  data[5] = (uint64_t)sec_shift;
  data[6] = mult_rem_ns;
  data[7] = mult_rem_us;
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
  if (blob_len > 4096)
    blob_len = 4096;
  memcpy(v, vdso_blob_start, blob_len);

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


bool vmm_is_user_addr_range_valid(uint64_t addr, size_t size) {
  if (addr > USER_SPACE_LIMIT || (addr + size) > 0x800000000000ULL) {
    klog_puts("[VMM] Range validation failed: out of bounds\n");
    return false;
  }

  struct thread *current = sched_get_current();
  if (!current)
    return false;

  uint64_t start_page = addr & ~0xFFFULL;
  uint64_t end_page = (addr + size + 0xFFF) & ~0xFFFULL;

  // Acquire the MM lock once for the entire range scan instead of once per
  // page.  For a 1 MB buffer (256 pages) the old code did 256 lock/unlock
  // round-trips; now it does one.
  //
  // Additionally, when a VMA covers multiple pages we jump straight to its
  // end boundary rather than re-querying vma_find for every page inside it.
  // This reduces AVL tree lookups from O(pages) to O(distinct VMAs in range),
  // which for a normal process is typically 1-3 for any contiguous buffer.
  spinlock_acquire(&current->mm->lock);

  uint64_t page = start_page;
  while (page < end_page) {
    struct vma *v = vma_find(&current->mm->vmas, page);
    if (!v)
      v = vma_find_growdown(&current->mm->vmas, page, 8 * 1024 * 1024);

    if (!v) {
      klog_puts("[VMM] Range validation failed (No VMA) at ");
      klog_hex64(page);
      klog_puts(" in thread ");
      klog_uint64(current->tid);
      klog_puts("\n[VMM] Active VMAs:\n");
      vma_dump(&current->mm->vmas);
      spinlock_release(&current->mm->lock);
      return false;
    }

    if (v->prot == PROT_NONE) {
      klog_puts("[VMM] Range validation failed (PROT_NONE) at 0x");
      klog_uint64(page);
      klog_puts("\n");
      spinlock_release(&current->mm->lock);
      return false;
    }

    // Skip to the end of this VMA — every page inside it has the same prot.
    // Clamp to end_page so we don't overshoot on the last VMA.
    page = v->end < end_page ? v->end : end_page;
  }

  spinlock_release(&current->mm->lock);
  return true;
}


bool vmm_is_user_addr_range_writable(uint64_t addr, size_t size) {
  if (size == 0)
    return true;
  if (addr > USER_SPACE_LIMIT || size > 0x800000000000ULL - addr)
    return false;

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return false;

  uint64_t start_page = addr & ~0xFFFULL;
  uint64_t end_page = (addr + size + 0xFFF) & ~0xFFFULL;

  spinlock_acquire(&current->mm->lock);

  uint64_t page = start_page;
  while (page < end_page) {
    struct vma *v = vma_find(&current->mm->vmas, page);
    if (!v)
      v = vma_find_growdown(&current->mm->vmas, page, 8 * 1024 * 1024);

    if (!v || !(v->prot & PROT_WRITE)) {
      spinlock_release(&current->mm->lock);
      return false;
    }

    page = v->end < end_page ? v->end : end_page;
  }

  spinlock_release(&current->mm->lock);
  return true;
}
