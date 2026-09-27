#ifndef VMM_H
#define VMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_FLAG_PRESENT ((uint64_t)1 << 0)
#define PAGE_FLAG_RW ((uint64_t)1 << 1)
#define PAGE_FLAG_RW ((uint64_t)1 << 1)
#define PAGE_FLAG_USER ((uint64_t)1 << 2)
#define PAGE_FLAG_PWT ((uint64_t)1 << 3)
#define PAGE_FLAG_PCD ((uint64_t)1 << 4)
#define PAGE_FLAG_A ((uint64_t)1 << 5)
#define PAGE_FLAG_D ((uint64_t)1 << 6)
#define PAGE_FLAG_PAT ((uint64_t)1 << 7)
#define PAGE_FLAG_PS ((uint64_t)1 << 7)
#define PAGE_FLAG_COW ((uint64_t)1 << 9)
#define PAGE_FLAG_NX ((uint64_t)1 << 63)

// Mask to extract the physical address from a page table entry.
// Strips both the low 12 flag bits AND the high bits (NX, available).
// Using ~0xFFFULL instead of this will preserve the NX bit and cause
// HHDM address overflow → GPF!
#define PAGE_MASK 0x000FFFFFFFFFF000ULL

// Virtual Memory Layout Definitions
#define USER_SPACE_BASE 0x0000000000000000ULL
#define USER_SPACE_LIMIT 0x00007FFFFFFFFFFFULL
#define KERNEL_SPACE_BASE 0xFFFF800000000000ULL
#define HHDM_BASE 0xFFFF800000000000ULL
#define VMAP_BASE 0xFFFFC00000000000ULL
#define KERNEL_HEAP_BASE 0xFFFFE00000000000ULL

// To retrieve the active top-level page directory from CR3
uint64_t *vmm_get_active_pml4(void);

// Given the active PML4 and a virtual address, map it to a physical frame
// Returns true on success, false on failure (OOM allocating intermediate page
// tables)
bool vmm_map_page(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr,
                  uint64_t flags);

// Maps a virtual page only if no present mapping already exists.
// Returns true on successful mapping, false if already mapped or on OOM.
bool vmm_map_page_if_unmapped(uint64_t *pml4, uint64_t virtual_addr,
                             uint64_t physical_addr, uint64_t flags);

// Maps a contiguous range of pages
bool vmm_map_range(uint64_t *pml4, uint64_t virtual_addr,
                   uint64_t physical_addr, size_t pages, uint64_t flags);

// Maps a huge page (2MB) by setting the PS flag on the Page Directory entry
bool vmm_map_huge_page(uint64_t *pml4, uint64_t virtual_addr,
                       uint64_t physical_addr, uint64_t flags);

// Unmap a virtual page
void vmm_unmap_page(uint64_t *pml4, uint64_t virtual_addr);

// Returns true if the address is mapped with a huge page (PS bit set on PD/PDPT)
bool vmm_is_huge_page(uint64_t *pml4, uint64_t virtual_addr);

// Replaces a 2 MB leaf mapping with 512 4 KB entries so that only part of it
// can be torn down.  Returns false only if the page table for the split could
// not be allocated; true also when the address was already 4 KB mapped.
bool vmm_split_huge_page(uint64_t *pml4, uint64_t virtual_addr);

// Frees empty page tables (PT, PD, PDPT) upwards if they contain no valid
// entries
void vmm_free_empty_tables(uint64_t *pml4, uint64_t virtual_addr);

// Flush the Translation Lookaside Buffer for a specific page
static inline void vmm_flush_tlb(uint64_t virtual_addr) {
  __asm__ volatile("invlpg (%0)" ::"r"(virtual_addr) : "memory");
}

// Resolve a virtual address to its physical address using the given PML4.
// Returns 0 if the mapping does not exist.
uint64_t vmm_virt_to_phys(uint64_t *pml4, uint64_t virtual_addr);

// ---- Bring-up diagnostics (temporary) -----------------------------------
// Raw page-table walk that never follows a frame that is not RAM, so it is
// safe to call when the tables are suspected corrupt (unlike
// vmm_virt_to_phys, which faults on a garbage intermediate entry).  Fills up
// to four raw entries (0 for levels the walk did not reach) and returns the
// translated physical address, or 0 when the address is not mapped.
uint64_t vmm_debug_walk(uint64_t pml4_phys, uint64_t virtual_addr,
                        uint64_t entries[4]);

// Prints the walk above with serial_write_sync(): no locks, safe from an
// interrupt context that may have interrupted the klog path.
void vmm_debug_dump_walk(const char *tag, uint64_t pml4_phys,
                         uint64_t virtual_addr);

// Clone all user-space page mappings (PML4 entries 0-255) from src_pml4
// into a newly allocated PML4. Kernel higher-half entries (256-511) are
// shallow-copied (shared). Each mapped user page gets a fresh physical
// frame with its content copied. Returns the *physical* address of the
// new PML4, or 0 on failure.
uint64_t vmm_clone_user_mappings(uint64_t *src_pml4_phys);

// Clone user-space page mappings with VMA awareness.
// Shared mappings (MAP_SHARED) share physical pages between parent and child.
// Private mappings (MAP_PRIVATE) get deep-copied (child gets its own pages).
// Returns the *physical* address of the new PML4, or 0 on failure.
struct vma_list;
uint64_t vmm_clone_user_mappings_vma(uint64_t *src_pml4_phys,
                                     struct vma_list *vmas);

// Create a new blank address space (shallow copy of kernel-space only)
uint64_t *vmm_create_pml4(void);

// Initialize the base system kernel map
void vmm_init(void);

// Protect all current page table pages from being reclaimed by PMM
void vmm_protect_active_tables(void);

// Free all user-space page tables and mapped pages for a given PML4.
// The PML4 physical page itself is also freed.
// Uses PAGE_MASK to properly strip NX/available bits from PTEs.
void vmm_free_user_pages(uint64_t cr3);

// VMA-aware version: consults vmas to skip freeing physical pages
// that belong to MAP_SHARED mappings (e.g. device MMIO like framebuffer).
void vmm_free_user_pages_vma(uint64_t cr3, struct vma_list *vmas);

struct registers;

// Demand paging fault handler. Returns 0 if handled, -1 if it's an
// unrecoverable fault.
int vmm_handle_page_fault(uint64_t cr2, uint64_t error_code,
                          struct registers *regs);

// Refusing a fault is fatal in kernel mode, and vmm_handle_page_fault() has
// more than a dozen ways to refuse one, all of which used to look identical
// from the outside. Each bail-out records the check that gave up so the panic
// report can name it instead of guessing.
struct vmm_fault_reject {
  const char *reason; // what the check was looking at
  const char *file;   // source file of the check
  uint32_t line;      // line of the check
  uint32_t seq;       // bumped on every rejection
  uint32_t tid;       // thread whose fault was refused
  uint64_t cr2;       // fault address this rejection belongs to
  uint64_t err_code;  // CPU error code as the paging engine saw it
  uint64_t rip;       // faulting instruction, when registers were available
  uint64_t detail;    // the offending entry, VMA prot, or frame
  uint64_t detail2;   // second piece of context (VMA bounds, refcount, ...)
};

// Copies the most recent rejection into *out. False means the paging engine
// has never refused a fault, or the record was caught mid-update by a
// rejection on another CPU.
bool vmm_get_last_fault_reject(struct vmm_fault_reject *out);

// Checks if a user address range is valid (within the user address space
// and covered by one or more VMAs).
bool vmm_is_user_addr_range_valid(uint64_t addr, size_t size);
bool vmm_is_user_addr_range_writable(uint64_t addr, size_t size);

#define VDSO_USER_BASE 0x700000000000ULL

void vmm_map_signal_trampoline(uint64_t *pml4);
void vmm_init_vsyscall_page(void);
void vmm_map_vsyscall_page(uint64_t *pml4);
void vmm_update_vdso_data(void);
uint64_t vmm_get_vsyscall_page_phys(void);


// ---- Internal helpers used across vmm_*.c modules -----------------------
// (not part of the public kernel API — do not call from outside mm/)

#include "../lock/spinlock.h"

// Returns a pointer to the VMM spinlock owned by vmm_map.c.
//
// Prefer vmm_lock_acquire()/vmm_lock_release() below: this is only for code
// that has to reason about the lock itself.
rawspinlock_t *vmm_get_lock(void);

// Instrumented acquisition of vmm_lock.  vmm_lock is the lock the page fault
// handler also needs, so a holder that blocks while holding it stops every
// core that faults — which makes "who holds it, since when, and from where"
// the first question any hang report has to answer.  Acquire through these
// wrappers so lockdiag can answer it.
void vmm_lock_acquire_at(uint64_t caller_ip);
void vmm_lock_release(void);
#define vmm_lock_acquire()                                                     \
  vmm_lock_acquire_at((uint64_t)__builtin_return_address(0))

// Returns the physical address of the permanent kernel PML4.
uint64_t *vmm_get_kernel_pml4(void);

// Sets the permanent kernel PML4 (called once from vmm_init).
void vmm_set_kernel_pml4(uint64_t *pml4_phys);

#endif
