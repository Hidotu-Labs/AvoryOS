// Memory Management Syscalls: mmap, munmap, brk
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../mm/tlb_shootdown.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

// Linux mmap constants
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_HUGETLB 0x40000         /* Linux user-space: request huge pages */
/* Kernel-internal MAP_HUGEPAGE — must match vma.h */
#define MAP_HUGEPAGE 0x200000000ULL /* enable 2MB demand paging for this VMA */

#define MAP_FAILED ((uint64_t)-1)

// Errno constants
#define E_INVAL ((uint64_t)-22)
#define E_NOMEM ((uint64_t)-12)
#define E_BADF ((uint64_t)-9)

// Helpers

#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define HHDM_OFFSET pmm_get_hhdm_offset()
#define PML4_PHYS_TO_VIRT(p) ((uint64_t *)(((uint64_t)(p)) + HHDM_OFFSET))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// User-space pointer validation: reject anything above canonical user range.
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL
static inline bool is_user_pointer(uint64_t addr) {
  return addr <= USER_ADDR_MAX;
}

static uint64_t build_page_flags(uint64_t prot) {
  // PROT_NONE → no flags at all (page must stay non-present).
  // On x86-64 there is no "read disable" bit; PRESENT alone grants reads.
  if (prot == PROT_NONE)
    return 0;

  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & PROT_WRITE)
    flags |= PAGE_FLAG_RW;
  if (!(prot & PROT_EXEC))
    flags |= PAGE_FLAG_NX;
  return flags;
}

// Anonymous mapping
#define MMAP_REGION_BASE 0x7F0000000000ULL
#define MMAP_REGION_LIMIT 0x7FF000000000ULL

// INTERNAL HELPERS

static void safe_unmap_and_free(uint64_t *pml4, uint64_t va, uint64_t phys,
                                bool free_phys, const char *ctx) {
  (void)ctx;
  // Step 1: remove the PTE.  After this the frame is unreachable via this VA.
  vmm_unmap_page(pml4, va);

  /* Step 2: only now is it safe to recycle the frame - and only if removing
   * the PTE actually succeeded.  vmm_unmap_page() silently declines anything
   * under a 1 GB leaf, and freeing a frame that is still mapped hands live
   * memory to the next allocation. */
  if (free_phys && phys != 0) {
    if (vmm_virt_to_phys(pml4, va) == 0)
      pmm_free((void *)phys);
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

/* Does this VA fall in a generic file-mapping VMA whose PTEs hold a PMM
 * reference to their page-cache frame?  Those references must be dropped on
 * unmap (pmm_free_page() is pmm_decref()).  Driver-mapped frames (GEM,
 * framebuffer, tmpfs) are owned by their backing object and are left alone. */
static bool range_page_is_pagecache(struct thread *t, uint64_t va,
                                    bool mm_locked) {
  if (!t || !t->mm)
    return false;

  bool ours;
  if (mm_locked) {
    struct vma *v = vma_find(&t->mm->vmas, va);
    ours = v && (v->flags & MAP_PAGECACHE);
  } else {
    spinlock_acquire(&t->mm->lock);
    struct vma *v = vma_find(&t->mm->vmas, va);
    ours = v && (v->flags & MAP_PAGECACHE);
    spinlock_release(&t->mm->lock);
  }
  return ours;
}

// teardown_range:
//   Unmap [base, base+len) and free anonymous private frames.
//   Used by both MAP_FIXED pre-teardown and munmap proper.
//   Does NOT touch the VMA list — callers manage that themselves.
//
//   @mm_locked: true when the caller holds t->mm->lock.  Passing true keeps the
//   whole teardown atomic against the caller's own VMA edits, but it also means
//   the per-page TLB shootdowns are issued while holding an interrupt-masked
//   lock - a CPU faulting into this same address space then waits for that lock
//   and cannot acknowledge the IPI.  Callers that do not need the atomicity
//   should pass false.
static void teardown_range(uint64_t *pml4, struct thread *t, uint64_t base,
                           uint64_t len, bool mm_locked, const char *ctx) {
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

  bool is_shared = (map_type == MAP_SHARED || map_type == 0x03);
  bool is_private = (map_type == MAP_PRIVATE);
  if (length == 0) {
    return E_INVAL;
  }

  // Determine virtual address
  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  struct thread *current_thread = sched_get_current();


  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t vaddr = 0;

  if (flags & MAP_FIXED) {
    if (addr == 0 || (addr & (PAGE_SIZE - 1))) {
      return E_INVAL;
    }
    if (!is_user_pointer(addr) || !is_user_pointer(addr + aligned_len - 1)) {
      return E_INVAL;
    }
    vaddr = addr;

    // Tear down any existing mappings in the target range.  teardown_range()
    // does its own short-lived mm->lock for the VMA lookups it needs; taking
    // the lock here as well would keep it held across every per-page TLB
    // shootdown in the range.
    teardown_range(pml4, current_thread, vaddr, aligned_len, false,
                   "MAP_FIXED teardown");
    if (current_thread && current_thread->mm) {
      spinlock_acquire(&current_thread->mm->lock);
      vma_remove(&current_thread->mm->vmas, vaddr, vaddr + aligned_len);
      spinlock_release(&current_thread->mm->lock);
    }
  } else {
    // Non-fixed: allocate dynamically utilizing AVL Interval Gap Finding
    if (current_thread && current_thread->mm) {
      spinlock_acquire(&current_thread->mm->lock);

      // Try addr as a hint if provided and aligned
      if (addr != 0 && (addr & (PAGE_SIZE - 1)) == 0 &&
          is_user_pointer(addr) && is_user_pointer(addr + aligned_len - 1) &&
          addr >= MMAP_REGION_BASE && addr + aligned_len <= MMAP_REGION_LIMIT) {
        if (!vma_find_overlap(&current_thread->mm->vmas, addr, addr + aligned_len)) {
          vaddr = addr;
        }
      }

      if (vaddr == 0) {
        vaddr = vma_find_gap(&current_thread->mm->vmas, aligned_len,
                             MMAP_REGION_BASE, MMAP_REGION_LIMIT);
      }

      if (vaddr == 0) {
        vaddr = mm_alloc_mmap_region(aligned_len);
      }

      if (vaddr == 0 || vaddr + aligned_len > MMAP_REGION_LIMIT) {
        spinlock_release(&current_thread->mm->lock);
        return E_NOMEM;
      }

      if (flags & MAP_ANONYMOUS) {
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
    if (!current_thread || fd >= MAX_FDS || !current_thread->fds[fd]) {
      return E_BADF;
    }
    vfs_node_t *node = current_thread->fds[fd];

    if (node->mmap) {
      // Pass MAP_FIXED to internal handler to ensure it respects our vaddr
      uint64_t result =
          node->mmap(node, vaddr, length, prot, flags | MAP_FIXED, offset);
      if (result == MAP_FAILED || result == (uint64_t)-1)
        return E_NOMEM;

      if (current_thread && current_thread->mm) {
        spinlock_acquire(&current_thread->mm->lock);
        int vma_idx =
            vma_add(&current_thread->mm->vmas, result, result + aligned_len, prot,
                    flags, (int)fd, offset, node, 0);
        spinlock_release(&current_thread->mm->lock);
        if (vma_idx < 0) {
          return E_NOMEM;
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

  // Unmap and free.  teardown_range() takes mm->lock itself, for as long as it
  // needs to read the VMA tree: a second thread's mmap()/mprotect() can free
  // the very node it is reading.  It must not be called with that lock held,
  // because every unmap in the range issues a TLB shootdown and waits for the
  // other CPUs, and a CPU faulting into this address space is waiting on the
  // same lock with interrupts masked.
  teardown_range(pml4, current, addr, aligned_len, false, "sys_munmap");

  // Remove VMAs
  spinlock_acquire(&current->mm->lock);
  vma_remove(&current->mm->vmas, addr, addr + aligned_len);
  vma_merge_adjacent(&current->mm->vmas);
  spinlock_release(&current->mm->lock);

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
  if (!is_user_pointer(addr)) {
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

// sys_mprotect  (unchanged from original — included for completeness)
// Linux ABI: mprotect(addr, len, prot)
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
  if (!is_user_pointer(addr) || !is_user_pointer(addr + aligned_len - 1))
    return E_NOMEM;

  uint64_t *pml4 = vmm_get_active_pml4();

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return E_INVAL;

  spinlock_acquire(&current->mm->lock);

  for (uint64_t va = addr; va < addr + aligned_len; va += PAGE_SIZE) {
    uint64_t phys = vmm_virt_to_phys(pml4, va);
    if (phys == 0)
      continue;

    uint64_t new_flags = build_page_flags(prot);
    if (prot == PROT_NONE) {
      // Keep the PTE present so teardown_range / vmm_virt_to_phys can still
      // find this physical frame at munmap time. Without a present PTE the
      // frame becomes unreachable and is never freed (3200 kB leak).
      // Omitting PAGE_FLAG_USER means any ring-3 access will #PF, which is
      // the correct PROT_NONE semantics on x86-64.
      new_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_NX;
    } else if (prot & PROT_WRITE) {
      struct vma *v = vma_find(&current->mm->vmas, va);
      if (PAGE_ALIGN_DOWN(phys) == pmm_get_zero_page_phys() ||
          (v && (v->flags & MAP_PRIVATE) && v->file_node != NULL)) {
        // Shared page cache frame or zero page made writable:
        // Must stay read-only and marked COW so subsequent writes allocate a private frame.
        new_flags = (new_flags & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
      }
    }
    if (!vmm_map_page(pml4, va, PAGE_ALIGN_DOWN(phys), new_flags)) {
      spinlock_release(&current->mm->lock);
      return E_NOMEM;
    }
  }

  // Synchronize the VMA tree so that syscall validation
  // (vmm_is_user_addr_range_valid) sees the updated protection bits.
  vma_mprotect(&current->mm->vmas, addr, addr + aligned_len, prot);
  vma_merge_adjacent(&current->mm->vmas);

  spinlock_release(&current->mm->lock);

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

  // Look up the original VMA to get its protection/flags.
  struct vma *orig_vma = vma_find(&current->mm->vmas, old_addr);
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

      /* Keep the file alive while replacing the old VMA reference. */
      vfs_node_ref(vma_file);
      vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);
      int add_ret =
          vma_add(&current->mm->vmas, old_addr, old_addr + aligned_new, prot,
                  vma_flags, vma_fd, vma_offset, vma_file, 0);
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
    // Extend the VMA to cover the new range.
    vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);
    vma_add(&current->mm->vmas, old_addr, old_addr + aligned_new, prot,
            vma_flags, -1, 0, NULL, 0);
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

    vfs_node_ref(vma_file);
    teardown_range(pml4, current, old_addr, aligned_old, true,
                   "mremap file move");
    vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);
    int add_ret =
        vma_add(&current->mm->vmas, new_addr, new_addr + aligned_new, prot,
                vma_flags, vma_fd, vma_offset, vma_file, 0);
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

  // Tear down old mapping.
  teardown_range(pml4, current, old_addr, aligned_old, true, "mremap move");
  vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);

  // Register new VMA.
  vma_add(&current->mm->vmas, new_addr, new_addr + aligned_new, prot, vma_flags,
          -1, 0, NULL, 0);

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
    /* mm->lock goes back before the teardown: teardown_range() must not run
     * with it held, because each page it unmaps issues a TLB shootdown and
     * waits for every other CPU, while a CPU faulting into this address space
     * waits on this same lock with interrupts masked by the fault gate. */
    spinlock_release(&current->mm->lock);
    teardown_range(pml4, current, addr, aligned_len, false,
                   "madvise DONTNEED");
    /* Belt-and-braces flush, also outside the lock. */
    tlb_shootdown_all();
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

// Public API

void syscall_register_mm(void) {
  syscall_register(SYS_MMAP, sys_mmap);
  syscall_register(SYS_MUNMAP, sys_munmap);
  syscall_register(SYS_MREMAP, sys_mremap);
  syscall_register(SYS_MSYNC, sys_msync);
  syscall_register(SYS_MINCORE, sys_mincore);
  syscall_register(SYS_MADVISE, sys_madvise);
  syscall_register(SYS_BRK, sys_brk);
  syscall_register(SYS_MPROTECT, sys_mprotect);
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
