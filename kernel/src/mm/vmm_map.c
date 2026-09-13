#include "vmm.h"
#include "../apic/lapic.h"
#include "../console/klog.h"
#include "../drivers/serial.h"
#include "../lib/tsc.h"
#include "../lock/lockdiag.h"
#include "../lock/spinlock.h"
#include "pmm.h"
#include "tlb_shootdown.h"
#include "../smp/cpu.h"
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

static spinlock_t vmm_lock = SPINLOCK_INIT;

spinlock_t *vmm_get_lock(void) { return &vmm_lock; }

/* Diagnostics used below and defined next to vmm_debug_walk() at the bottom
 * of the file.  Declared here so page-table teardown/split paths can log. */
static void vmm_dbg_out(const char *s);
static void vmm_dbg_hex(uint64_t v);

/* True when the virtual range [base, base + size) contains the LAPIC MMIO
 * page as seen through the HHDM.  Used to flag any page-table operation that
 * can affect the mapping the interrupt path depends on. */
static bool vmm_dbg_range_has_lapic(uint64_t base, uint64_t size) {
  uint64_t lapic_va = lapic_get_va();
  return lapic_va != 0 && lapic_va >= base && lapic_va - base < size;
}

/* --------------------------------------------------------------------------
 * vmm_lock acquisition wrappers
 *
 * vmm_lock is the lock the page fault handler also needs, which makes it the
 * one lock whose holder can stop the entire machine: every other core faults
 * continuously, and a fault handler runs with interrupts masked (the IDT uses
 * interrupt gates), so while it spins here it cannot even take an IPI.
 *
 * Every acquisition therefore goes through here, where lockdiag can name the
 * holder, how long it has been held and where it was taken from.  Uncontended
 * acquisitions cost no rdtsc and no extra atomic: the try-acquire below *is*
 * the acquisition, and the contended path is the one worth measuring.
 *
 * It is a spinlock_t rather than a rawspinlock for the same reason
 * shootdown_lock is: syscalls run interruptible since C6, so an IRQs-on
 * holder can be preempted by the tick mid-walk and then the next page fault
 * on that CPU - which must reach the page tables through this lock - spins
 * forever on a lock whose owner is not running.  Masking interrupts while
 * the lock is held makes the holder non-preemptible; a waiter that had
 * interrupts enabled still opens them while spinning, so it can answer a
 * TLB shootdown IPI.  A held vmm_lock now delays the acknowledgement of an
 * incoming shootdown until release instead of answering inside the critical
 * section, which is bounded: nothing in a vmm_lock section sleeps or waits
 * on another core.
 * -------------------------------------------------------------------------- */
void vmm_lock_acquire_at(uint64_t caller_ip) {
  if (spinlock_try_acquire(&vmm_lock)) {
    lockdiag_spot_take(LOCKDIAG_SPOT_VMM, 0, caller_ip);
    return;
  }
  uint64_t t0 = rdtsc();
  spinlock_acquire(&vmm_lock);
  lockdiag_spot_take(LOCKDIAG_SPOT_VMM, rdtsc() - t0, caller_ip);
}

/* --------------------------------------------------------------------------
 * Deferred work that a vmm_lock critical section cannot do itself
 *
 * Two things must happen after a page-table change but cannot happen inside the
 * lock: waiting for remote TLB acknowledgements (a target core reaches its page
 * fault handler through vmm_lock, so waiting here while holding it is a
 * deadlock), and handing emptied page-table frames back to the allocator (they
 * must not be reused until the invalidations have landed).  Both are parked here
 * and performed from vmm_lock_release(), off the lock.
 * -------------------------------------------------------------------------- */
#define VMM_PENDING_TABLES 64

static void *vmm_pending_tables[MAX_CPUS][VMM_PENDING_TABLES];
static uint32_t vmm_pending_table_count[MAX_CPUS];

static int vmm_pending_slot(void) {
  struct cpu_info *c = cpu_get_current();
  if (!c || c->cpu_id >= MAX_CPUS)
    return -1;
  return (int)c->cpu_id;
}

static void vmm_defer_table_free(void *phys) {
  int slot = vmm_pending_slot();
  if (slot < 0) {
    pmm_free_page(phys); /* early boot: nothing else can be walking tables */
    return;
  }
  hal_irq_state_t flags = hal_irq_save();
  uint32_t n = vmm_pending_table_count[slot];
  if (n < VMM_PENDING_TABLES) {
    vmm_pending_tables[slot][n] = phys;
    vmm_pending_table_count[slot] = n + 1;
    hal_irq_restore(flags);
    return;
  }
  hal_irq_restore(flags);
  pmm_free_page(phys); /* list full: fall back to freeing where it has always */
}                      /* been freed, rather than losing the request        */

static void vmm_drain_pending(void) {
  /* Order matters: invalidate, then release the frames. */
  tlb_flush_deferred_drain();

  int slot = vmm_pending_slot();
  if (slot < 0)
    return;
  for (;;) {
    hal_irq_state_t flags = hal_irq_save();
    uint32_t n = vmm_pending_table_count[slot];
    if (n == 0) {
      hal_irq_restore(flags);
      break;
    }
    void *phys = vmm_pending_tables[slot][n - 1];
    vmm_pending_table_count[slot] = n - 1;
    hal_irq_restore(flags);
    pmm_free_page(phys);
  }
}

void vmm_lock_release(void) {
  lockdiag_spot_drop(LOCKDIAG_SPOT_VMM);
  /* spinlock_release restores the caller's interrupt state before the drain
   * runs, so the deferred shootdowns below execute in exactly the context
   * that would have issued them inline. */
  spinlock_release(&vmm_lock);
  vmm_drain_pending();
}

// ---------------------------------------------------------------------------
// Internal helpers (no locking — caller must hold vmm_lock)
// ---------------------------------------------------------------------------

static uint64_t *get_next_level(uint64_t *current_level, size_t index,
                                bool allocate, unsigned level) {
  if (current_level[index] & PAGE_FLAG_PRESENT) {
    if (current_level[index] & PAGE_FLAG_PS) {
      if (!allocate || (level != 3 && level != 2))
        return NULL;

      uint64_t old = current_level[index];
      void *new_table_phys = pmm_alloc();
      if (!new_table_phys)
        return NULL;
      uint64_t *new_table = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);

      if (level == 3) {
        /* Split a 1 GiB PDPTE into 512 equivalent 2 MiB PDEs.  The PAT
           position is bit 12 for both huge-page formats. */
        const uint64_t address_mask = PAGE_MASK & ~((1ULL << 30) - 1ULL);
        uint64_t base = old & address_mask;
        uint64_t leaf_flags = old & ~address_mask;
        if (base == (lapic_get_phys() & ~0x3FFFFFFFULL)) {
          vmm_dbg_out("[VMMDBG] split 1G page covering LAPIC base=");
          vmm_dbg_hex(base);
          vmm_dbg_out("\n");
        }
        for (size_t i = 0; i < 512; i++)
          new_table[i] = (base + i * (1ULL << 21)) | leaf_flags;
      } else {
        /* Split a 2 MiB PDE into 512 4 KiB PTEs.  Huge-page PAT is bit 12;
           the 4 KiB PTE encoding moves PAT to bit 7 (the former PS bit). */
        const uint64_t address_mask = PAGE_MASK & ~((1ULL << 21) - 1ULL);
        uint64_t base = old & address_mask;
        bool pat = (old & (1ULL << 12)) != 0;
        uint64_t leaf_flags = old & ~address_mask;
        leaf_flags &= ~((1ULL << 12) | PAGE_FLAG_PS);
        if (pat)
          leaf_flags |= PAGE_FLAG_PAT;
        if (base == (lapic_get_phys() & ~0x1FFFFFULL)) {
          vmm_dbg_out("[VMMDBG] split 2M page covering LAPIC base=");
          vmm_dbg_hex(base);
          vmm_dbg_out("\n");
        }
        for (size_t i = 0; i < 512; i++)
          new_table[i] = (base + i * PAGE_SIZE) | leaf_flags;
      }

      uint64_t table_flags = old & (PAGE_FLAG_PRESENT | PAGE_FLAG_RW |
                                    PAGE_FLAG_USER | PAGE_FLAG_PWT |
                                    PAGE_FLAG_PCD);
      current_level[index] = (uint64_t)new_table_phys | table_flags;
      return new_table;
    }
    uint64_t next_phys = current_level[index] & PAGE_MASK;
    return (uint64_t *)PHYS_TO_VIRT(next_phys);
  }

  if (!allocate)
    return NULL;

  void *new_table_phys = pmm_alloc();
  if (!new_table_phys)
    return NULL;

  uint64_t *new_table_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
  for (size_t i = 0; i < 512; i++)
    new_table_virt[i] = 0;

  current_level[index] = ((uint64_t)new_table_phys) | PAGE_FLAG_PRESENT |
                         PAGE_FLAG_RW | PAGE_FLAG_USER;
  return new_table_virt;
}

// Map a single page without acquiring the lock.
// Must only be called while vmm_lock is already held.
//
// flush_tlb controls whether invlpg is issued after writing the PTE.
//
// Pass flush_tlb=false when installing a brand-new mapping (non-present →
// present). The x86 architecture guarantees that the CPU will not use a
// stale TLB entry for an address it has never successfully translated, so
// no invalidation is required for fresh mappings. Issuing invlpg anyway
// wastes 40-100 cycles per page and serialises the pipeline.
//
// Pass flush_tlb=true when *replacing* an existing present mapping (e.g.
// CoW break, permission change) to evict the old cached translation.
static bool vmm_map_page_nolock(uint64_t *pml4, uint64_t virtual_addr,
                                uint64_t physical_addr, uint64_t flags,
                                bool flush_tlb) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt      = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4 & PAGE_MASK);
  uint64_t  propagate_flags = flags & (PAGE_FLAG_USER | PAGE_FLAG_RW);

  uint64_t *pdpt_virt = get_next_level(pml4_virt, pml4_index, true, 4);
  if (!pdpt_virt) {
    klog_puts("[VMM] Error: Failed to get/create PDPT for vaddr 0x");
    klog_uint64(virtual_addr);
    klog_puts("\n");
    return false;
  }
  pml4_virt[pml4_index] |= propagate_flags;

  uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, true, 3);
  if (!pd_virt) {
    klog_puts("[VMM] Error: Failed to get/create PD for vaddr 0x");
    klog_uint64(virtual_addr);
    klog_puts("\n");
    return false;
  }
  pdpt_virt[pdpt_index] |= propagate_flags;

  uint64_t *pt_virt = get_next_level(pd_virt, pd_index, true, 2);
  if (!pt_virt) {
    klog_puts("[VMM] Error: Failed to get/create PT for vaddr 0x");
    klog_uint64(virtual_addr);
    klog_puts("\n");
    return false;
  }
  pd_virt[pd_index] |= propagate_flags;

  pt_virt[pt_index] = (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT;

  if (flush_tlb)
    tlb_flush_deferred(virtual_addr, (uint64_t)pml4);

  return true;
}

// Caller holds vmm_lock. Check leaf presence without allocating page-table
// levels. Fresh mappings must not wait for a remote TLB acknowledgement.
static bool vmm_page_present_nolock(uint64_t *pml4, uint64_t virtual_addr) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4 & PAGE_MASK);
  uint64_t *pdpt = get_next_level(pml4_virt, pml4_index, false, 4);
  if (!pdpt) return false;
  if (pdpt[pdpt_index] & PAGE_FLAG_PS) return true;
  uint64_t *pd = get_next_level(pdpt, pdpt_index, false, 3);
  if (!pd) return false;
  if (pd[pd_index] & PAGE_FLAG_PS) return true;
  uint64_t *pt = get_next_level(pd, pd_index, false, 2);
  return pt && (pt[pt_index] & PAGE_FLAG_PRESENT);
}

// Caller holds vmm_lock. Returns the leaf entry covering virtual_addr, or 0
// when there is none. A 2 MB mapping is returned with PAGE_FLAG_PS still set,
// which is how callers tell that the page is covered by a huge mapping rather
// than by its own 4 KB entry.
static uint64_t vmm_leaf_entry_nolock(uint64_t *pml4, uint64_t virtual_addr) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4 & PAGE_MASK);
  uint64_t *pdpt = get_next_level(pml4_virt, pml4_index, false, 4);
  if (!pdpt || !(pdpt[pdpt_index] & PAGE_FLAG_PRESENT)) return 0;
  if (pdpt[pdpt_index] & PAGE_FLAG_PS) return pdpt[pdpt_index];
  uint64_t *pd = get_next_level(pdpt, pdpt_index, false, 3);
  if (!pd || !(pd[pd_index] & PAGE_FLAG_PRESENT)) return 0;
  if (pd[pd_index] & PAGE_FLAG_PS) return pd[pd_index];
  uint64_t *pt = get_next_level(pd, pd_index, false, 2);
  if (!pt) return 0;
  return pt[pt_index];
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool vmm_map_page(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr,
                  uint64_t flags) {
  vmm_lock_acquire();
  /*
   * Only *replacing* a present mapping can leave a stale translation behind.
   * This used to be `replacing || (cpu_get_count() > 1)`, which forced a
   * broadcast-and-wait shootdown for every single page installed on any SMP
   * system - and every such shootdown made all other cores flush their entire
   * non-global TLB (see mm/tlb_shootdown.c). Installing a fresh mapping needs
   * no invalidation at all: nothing could have cached a translation for an
   * address that has never been translated.
   *
   * This shows up wherever pages are mapped in volume - process startup,
   * demand paging, and the per-page fb/GEM mmaps - because the acknowledgement
   * spin happens while holding vmm_lock, which the page fault path also needs.
   */
  uint64_t want = (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT;
  uint64_t cur = vmm_leaf_entry_nolock(pml4, virtual_addr);

  /* Re-mapping a page to precisely what it already is needs neither a write nor
   * an invalidation: no core can be caching a translation that differs from the
   * one already in the table.  The hardware sets A and D, so those two bits are
   * not a difference.  Without this, every driver that re-maps a BAR or an
   * already-mapped HHDM RAM page on probe paid one broadcast-and-wait shootdown
   * per page - virtio-gpu's 8 MB BAR0 alone is 2048 of them at boot. */
  if ((cur & PAGE_FLAG_PRESENT) && !(cur & PAGE_FLAG_PS) &&
      ((cur ^ want) & ~(PAGE_FLAG_A | PAGE_FLAG_D)) == 0) {
    vmm_lock_release();
    return true;
  }

  bool replacing = (cur & PAGE_FLAG_PRESENT) != 0;
  bool ok = vmm_map_page_nolock(pml4, virtual_addr, physical_addr, flags,
                                replacing);
  vmm_lock_release();
  return ok;
}

bool vmm_map_page_if_unmapped(uint64_t *pml4, uint64_t virtual_addr,
                             uint64_t physical_addr, uint64_t flags) {
  vmm_lock_acquire();
  if (vmm_page_present_nolock(pml4, virtual_addr)) {
    vmm_lock_release();
    return false;
  }
  /* Unreachable-by-construction replacement: the check above already returned
     for a present leaf, so this is always a fresh mapping. */
  bool ok = vmm_map_page_nolock(pml4, virtual_addr, physical_addr, flags, false);
  vmm_lock_release();
  return ok;
}

bool vmm_map_huge_page(uint64_t *pml4, uint64_t virtual_addr,
                       uint64_t physical_addr, uint64_t flags) {
  vmm_lock_acquire();
  bool success = false;

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;

  uint64_t *pml4_virt      = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4 & PAGE_MASK);
  uint64_t  propagate_flags = flags & (PAGE_FLAG_USER | PAGE_FLAG_RW);

  uint64_t *pdpt_virt = get_next_level(pml4_virt, pml4_index, true, 4);
  if (!pdpt_virt)
    goto unlock;
  pml4_virt[pml4_index] |= propagate_flags;

  uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, true, 3);
  if (!pd_virt)
    goto unlock;
  pdpt_virt[pdpt_index] |= propagate_flags;

  pd_virt[pd_index] =
      (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT | PAGE_FLAG_PS;
  tlb_flush_deferred(virtual_addr, (uint64_t)pml4);
  success = true;

unlock:
  vmm_lock_release();
  return success;
}

// Map a contiguous range of pages under a single lock acquisition.
//
// Before this fix vmm_map_range called vmm_map_page in a loop, which
// acquired and released vmm_lock once per page.  For a 512-page (2 MB)
// mapping that was 512 lock/unlock round-trips: each one issues CLI+MFENCE
// on acquire and STI+MFENCE on release, flushing the pipeline both times.
//
// Now the lock is taken once, all PTEs are written in the inner loop via
// vmm_map_page_nolock, and the lock is released once at the end.
// The spinlock overhead drops from O(pages) to O(1) regardless of range size.
//
// Additionally, invlpg is suppressed per-page (flush_tlb=false).  Every
// address in the range was previously non-present: the CPU cannot have a
// cached translation for it, so no TLB invalidation is required.  A single
// memory barrier (implicit in the spinlock release MFENCE) is sufficient to
// ensure the new PTEs are visible to this CPU before the caller touches the
// mapped memory.  This saves 40-100 cycles × N pages — roughly 20,000-50,000
// cycles for a typical 512-page (2 MB) mmap region.
bool vmm_map_range(uint64_t *pml4, uint64_t virtual_addr,
                   uint64_t physical_addr, size_t pages, uint64_t flags) {
  if (pages == 0)
    return true;

  vmm_lock_acquire();

  for (size_t i = 0; i < pages; i++) {
    // flush_tlb=false: these are fresh (non-present → present) mappings.
    // No stale TLB entry can exist for addresses the CPU never translated.
    if (!vmm_map_page_nolock(pml4,
                             virtual_addr  + (i * 4096),
                             physical_addr + (i * 4096),
                             flags, false)) {
      vmm_lock_release();
      return false;
    }
  }

  vmm_lock_release();
  return true;
}

void vmm_free_empty_tables(uint64_t *pml4, uint64_t virtual_addr) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;

  /* Kernel-half tables are shared between the kernel PML4 and every process
   * PML4 (both clone paths shallow-copy entries 256..511).  Freeing one
   * through a process PML4 hands a live table page to the PMM while the
   * kernel (and every other process) still points at it.  Flag it loudly. */
  if (pml4_index >= 256 &&
      (uint64_t)(uintptr_t)pml4 !=
          (uint64_t)(uintptr_t)vmm_get_kernel_pml4()) {
    vmm_dbg_out("[VMMDBG] WARNING: vmm_free_empty_tables on kernel-half "
                "address from a non-kernel PML4 va=");
    vmm_dbg_hex(virtual_addr);
    vmm_dbg_out(" cr3=");
    vmm_dbg_hex((uint64_t)(uintptr_t)pml4);
    vmm_dbg_out(" kernel=");
    vmm_dbg_hex((uint64_t)(uintptr_t)vmm_get_kernel_pml4());
    vmm_dbg_out("\n");
  }

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4 & PAGE_MASK);
  if (!(pml4_virt[pml4_index] & PAGE_FLAG_PRESENT))
    return;

  uint64_t  pdpt_phys = pml4_virt[pml4_index] & PAGE_MASK;
  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_phys);
  if (!(pdpt_virt[pdpt_index] & PAGE_FLAG_PRESENT) ||
      (pdpt_virt[pdpt_index] & PAGE_FLAG_PS))
    return;

  uint64_t  pd_phys = pdpt_virt[pdpt_index] & PAGE_MASK;
  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pd_phys);
  if (!(pd_virt[pd_index] & PAGE_FLAG_PRESENT) ||
      (pd_virt[pd_index] & PAGE_FLAG_PS))
    return;

  uint64_t  pt_phys = pd_virt[pd_index] & PAGE_MASK;
  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pt_phys);

  bool pt_empty = true;
  for (int i = 0; i < 512; i++) {
    if (pt_virt[i] & PAGE_FLAG_PRESENT) {
      pt_empty = false;
      break;
    }
  }

  if (pt_empty) {
    pd_virt[pd_index] = 0;
    if (vmm_dbg_range_has_lapic(virtual_addr, 2ULL << 20)) {
      vmm_dbg_out("[VMMDBG] free PT: cleared PDE covering LAPIC va=");
      vmm_dbg_hex(virtual_addr);
      vmm_dbg_out(" pt_phys=");
      vmm_dbg_hex(pt_phys);
      vmm_dbg_out(" cr3=");
      vmm_dbg_hex((uint64_t)pml4);
      vmm_dbg_out("\n");
    }
    tlb_flush_deferred(virtual_addr, (uint64_t)pml4);
    vmm_defer_table_free((void *)pt_phys);

    bool pd_empty = true;
    for (int i = 0; i < 512; i++) {
      if (pd_virt[i] & PAGE_FLAG_PRESENT) {
        pd_empty = false;
        break;
      }
    }

    if (pd_empty) {
      pdpt_virt[pdpt_index] = 0;
      if (vmm_dbg_range_has_lapic(virtual_addr, 1ULL << 30)) {
        vmm_dbg_out("[VMMDBG] free PD: cleared PDPTE covering LAPIC va=");
        vmm_dbg_hex(virtual_addr);
        vmm_dbg_out(" pd_phys=");
        vmm_dbg_hex(pd_phys);
        vmm_dbg_out(" cr3=");
        vmm_dbg_hex((uint64_t)pml4);
        vmm_dbg_out("\n");
      }
      tlb_flush_deferred(virtual_addr, (uint64_t)pml4);
      vmm_defer_table_free((void *)pd_phys);

      bool pdpt_empty = true;
      for (int i = 0; i < 512; i++) {
        if (pdpt_virt[i] & PAGE_FLAG_PRESENT) {
          pdpt_empty = false;
          break;
        }
      }

      if (pdpt_empty) {
        pml4_virt[pml4_index] = 0;
        if (vmm_dbg_range_has_lapic(virtual_addr, 512ULL << 30)) {
          vmm_dbg_out("[VMMDBG] free PDPT: cleared PML4E covering LAPIC va=");
          vmm_dbg_hex(virtual_addr);
          vmm_dbg_out(" pdpt_phys=");
          vmm_dbg_hex(pdpt_phys);
          vmm_dbg_out(" cr3=");
          vmm_dbg_hex((uint64_t)pml4);
          vmm_dbg_out("\n");
        }
        tlb_flush_deferred(virtual_addr, (uint64_t)pml4);
        vmm_defer_table_free((void *)pdpt_phys);
      }
    }
  }
}

void vmm_unmap_page(uint64_t *pml4, uint64_t virtual_addr) {
  vmm_lock_acquire();

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4 & PAGE_MASK);

  if (!(pml4_virt[pml4_index] & PAGE_FLAG_PRESENT))
    goto unlock;
  uint64_t *pdpt_virt =
      (uint64_t *)PHYS_TO_VIRT(pml4_virt[pml4_index] & PAGE_MASK);

  uint64_t pdpt_entry = pdpt_virt[pdpt_index];
  if (!(pdpt_entry & PAGE_FLAG_PRESENT))
    goto unlock;
  if (pdpt_entry & PAGE_FLAG_PS)
    goto unlock; // 1 GB page — not supported at 4 KB granularity

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_entry & PAGE_MASK);
  uint64_t  pd_entry = pd_virt[pd_index];
  if (!(pd_entry & PAGE_FLAG_PRESENT))
    goto unlock;

  if (pd_entry & PAGE_FLAG_PS) {
    // 2 MB huge page — clear the PDE and free all 512 constituent frames.
    // We must release vmm_lock before calling into the PMM to avoid a
    // lock-order inversion (pmm_free_pages acquires b_zone.lock).
    uint64_t huge_phys = pd_entry & 0xFFFFFFFE00000ULL;
    pd_virt[pd_index] = 0;
    tlb_flush_deferred(virtual_addr & ~0x1FFFFFULL, (uint64_t)pml4);
    vmm_lock_release();
    pmm_free_pages((void *)huge_phys, 512);
    return;
  }

  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pd_entry & PAGE_MASK);
  pt_virt[pt_index] = 0;
  tlb_flush_deferred(virtual_addr, (uint64_t)pml4);

  if (pml4_index < 256 ||
      (pml4_index >= 256 && virtual_addr < KERNEL_HEAP_BASE)) {
    vmm_free_empty_tables(pml4, virtual_addr);
  }

unlock:
  vmm_lock_release();
}


/*
 * vmm_split_huge_page - replace a 2 MB leaf mapping with 512 equivalent 4 KB
 * page-table entries.
 *
 * Tearing down part of a huge mapping must not release the frames the caller
 * still owns, but clearing the PDE drops all 512 of them at once.  Splitting
 * first lets the caller unmap exactly the range it asked for.
 *
 * Returns true when the address is described by a 4 KB page table on return
 * (including when it already was), and false only when the table for the split
 * could not be allocated.
 */
bool vmm_split_huge_page(uint64_t *pml4, uint64_t virtual_addr) {
  bool actually_split = false;
  bool ok = true;

  vmm_lock_acquire();

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4 & PAGE_MASK);
  if (!(pml4_virt[pml4_index] & PAGE_FLAG_PRESENT))
    goto out;
  uint64_t *pdpt_virt =
      (uint64_t *)PHYS_TO_VIRT(pml4_virt[pml4_index] & PAGE_MASK);
  uint64_t pdpt_entry = pdpt_virt[pdpt_index];
  if (!(pdpt_entry & PAGE_FLAG_PRESENT))
    goto out;
  if (pdpt_entry & PAGE_FLAG_PS) {
    /* A 1 GB leaf.  vmm_unmap_page() refuses to unmap anything below one and
     * this function cannot split one, so reporting success would have the
     * caller release a frame that is still mapped. */
    ok = false;
    goto out;
  }

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_entry & PAGE_MASK);
  if (!(pd_virt[pd_index] & PAGE_FLAG_PRESENT))
    goto out;
  if (!(pd_virt[pd_index] & PAGE_FLAG_PS))
    goto out; // already 4 KB granularity

  // get_next_level() performs the in-place leaf-to-table conversion, including
  // the PAT bit relocation between the huge-page and PTE encodings.
  if (!get_next_level(pd_virt, pd_index, true, 2)) {
    ok = false;
    goto out;
  }
  actually_split = true;

out:
  vmm_lock_release();

  if (actually_split) {
    /* Replacing a leaf PDE with a table pointer does not invalidate the
     * large-page translation other CPUs have already cached, and one invlpg
     * for a single 4 KB page inside it is not enough to clear all 512.  This is
     * a rare path (only hit when a huge mapping is partially torn down), so a
     * full flush is the cheap way to be sure. */
    tlb_shootdown_all();
  }

  return ok;
}


/* --------------------------------------------------------------------------
 * Bring-up diagnostics (temporary)
 *
 * vmm_virt_to_phys() assumes every intermediate entry is a valid RAM page
 * table.  When the tables are the thing under suspicion (a missing LAPIC
 * mapping after exec, a GPF in the panic reporter) that assumption turns the
 * diagnostic into a second fault.  This walk checks each frame against
 * pmm_get_total_memory() before dereferencing it, and prints via
 * serial_write_sync() so it is safe from an interrupt that interrupted klog.
 * -------------------------------------------------------------------------- */

static bool vmm_debug_frame_ok(uint64_t phys) {
  if (phys == 0 || (phys & 0xFFF) != 0)
    return false;
  uint64_t total = pmm_get_total_memory();
  return total >= PAGE_SIZE && phys < total;
}

uint64_t vmm_debug_walk(uint64_t pml4_phys, uint64_t virtual_addr,
                        uint64_t entries[4]) {
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t table = pml4_phys & PAGE_MASK;

  for (int level = 0; level < 4; level++) {
    if (entries)
      entries[level] = 0;
    if (!vmm_debug_frame_ok(table))
      return 0;
    uint64_t *tbl = (uint64_t *)(uintptr_t)(table + hhdm);
    uint64_t index = (virtual_addr >> (39 - level * 9)) & 0x1FF;
    uint64_t e = tbl[index];
    if (entries)
      entries[level] = e;
    if (!(e & PAGE_FLAG_PRESENT))
      return 0;
    if (level == 3)
      return (e & PAGE_MASK) | (virtual_addr & 0xFFFULL);
    if (e & PAGE_FLAG_PS) {
      if (level == 1)
        return (e & 0x000FFFFFC0000000ULL) | (virtual_addr & 0x3FFFFFFFULL);
      if (level == 2)
        return (e & 0x000FFFFFFFE00000ULL) | (virtual_addr & 0x1FFFFFULL);
      return 0; /* PS at PML4: corruption */
    }
    table = e & PAGE_MASK;
  }
  return 0;
}

static void vmm_dbg_out(const char *s) {
  size_t n = 0;
  while (n < 256 && s[n])
    n++;
  serial_write_sync(s, n);
}

static void vmm_dbg_hex(uint64_t v) {
  static const char hex[] = "0123456789ABCDEF";
  char b[18];
  b[0] = '0';
  b[1] = 'x';
  for (int i = 0; i < 16; i++)
    b[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
  serial_write_sync(b, sizeof(b));
}

void vmm_debug_dump_walk(const char *tag, uint64_t pml4_phys,
                         uint64_t virtual_addr) {
  static const char *level_name[4] = {"PML4E", "PDPTE", "PDE", "PTE"};
  uint64_t entries[4] = {0, 0, 0, 0};
  uint64_t phys = vmm_debug_walk(pml4_phys, virtual_addr, entries);

  vmm_dbg_out("[VMMDBG] ");
  vmm_dbg_out(tag);
  vmm_dbg_out(" cr3=");
  vmm_dbg_hex(pml4_phys);
  vmm_dbg_out(" va=");
  vmm_dbg_hex(virtual_addr);
  vmm_dbg_out(" -> phys=");
  vmm_dbg_hex(phys);
  vmm_dbg_out("\n");
  for (int i = 0; i < 4; i++) {
    vmm_dbg_out("  ");
    vmm_dbg_out(level_name[i]);
    vmm_dbg_out("=");
    vmm_dbg_hex(entries[i]);
    if (!(entries[i] & PAGE_FLAG_PRESENT)) {
      vmm_dbg_out(" (not present)");
      break;
    }
    if (i == 3 || (entries[i] & PAGE_FLAG_PS))
      break;
  }
  vmm_dbg_out("\n");
}

uint64_t vmm_virt_to_phys(uint64_t *pml4_phys, uint64_t virtual_addr) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys & PAGE_MASK);
  uint64_t  entry;

  entry = pml4_virt[pml4_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;

  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pdpt_virt[pdpt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  if (entry & PAGE_FLAG_PS) // 1 GB huge page
    return (entry & 0xFFFFFC0000000ULL) | (virtual_addr & 0x3FFFFFFFULL);

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pd_virt[pd_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  if (entry & PAGE_FLAG_PS) // 2 MB huge page
    return (entry & 0xFFFFFFFE00000ULL) | (virtual_addr & 0x1FFFFFULL);

  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pt_virt[pt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  return (entry & PAGE_MASK) | (virtual_addr & 0xFFFULL);
}

bool vmm_is_huge_page(uint64_t *pml4_phys, uint64_t virtual_addr) {
  if (!pml4_phys)
    return false;

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys & PAGE_MASK);
  uint64_t  entry = pml4_virt[pml4_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return false;

  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pdpt_virt[pdpt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return false;
  if (entry & PAGE_FLAG_PS)
    return true; // 1 GB huge page

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pd_virt[pd_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return false;
  return (entry & PAGE_FLAG_PS) != 0; // 2 MB huge page
}

