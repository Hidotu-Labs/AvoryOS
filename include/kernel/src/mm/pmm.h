#ifndef PMM_H
#define PMM_H

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096

// Initialize basic PMM state (hhdm offset) so early components can function.
void pmm_init_early(uint64_t hhdm_offset);

// Initialize the full physical memory manager using the memory map and HHDM
// offset.
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

// Initialize Per-CPU Page Frame Allocator (PCP) caches after SMP/CPU is initialized
void pmm_pcp_init(void);

// New Buddy Allocator API
void *pmm_alloc_page(void); // Allocate single page
void *pmm_alloc_pages(
    size_t count); // Allocate multiple (will allocate ceil(log2(count)))
void *pmm_alloc_pages_constrained(size_t count, uint64_t max_phys_addr);
void *pmm_alloc_pages_range(size_t count, uint64_t min_phys_addr,
                            uint64_t max_phys_addr);
// Allocate a 2 MB huge page (512 contiguous 4 KB frames, 2 MB-aligned).
// The buddy allocator at order-9 guarantees 2 MB alignment by construction.
// Returns the physical address, or NULL on OOM.
void *pmm_alloc_huge_page(void);
void pmm_free_page(void *ptr);                // Free single page
void pmm_free_pages(void *ptr, size_t count); // Free multiple pages


// Refcounting (for CoW)
void pmm_incref(void *ptr);      // Increment reference count
void pmm_decref(void *ptr);      // Decrement reference count (frees if 0)
uint16_t pmm_get_ref(void *ptr); // Get current reference count
bool pmm_is_managed(
    uint64_t phys); // Check if page is managed by PMM (RAM vs MMIO)

// True when `ptr` is a kernel virtual address inside the HHDM window whose
// backing physical page is currently managed by the buddy allocator.  Data
// structures that cache raw kernel pointers (radix tree nodes, page cache
// entries, VMA file nodes) can be corrupted by a use-after-free or an
// out-of-bounds write; callers use this to reject a wild address before
// dereferencing it.  Note that a stale-but-still-mapped pointer can pass this
// check; it only rules out addresses that would page-fault in ring 0.
bool pmm_kernel_ptr_is_managed(const void *ptr);

// Compatibility aliases for existing code
#define pmm_alloc pmm_alloc_page
#define pmm_alloc_blocks pmm_alloc_pages
#define pmm_free pmm_free_page
#define pmm_free_blocks pmm_free_pages

void pmm_mark_used(void *ptr, size_t count);

// Reclaims the memory occupied by the Limine bootloader after boot structures
// are no longer needed
void pmm_reclaim_bootloader(uint64_t kernel_phys_base);
bool pmm_is_reclaimable(uint64_t phys);

// Return usable memory in bytes
uint64_t pmm_get_usable_memory(void);

// Return total generic memory in bytes
uint64_t pmm_get_total_memory(void);

// Expose the HHDM base
uint64_t pmm_get_hhdm_offset(void);

// Statistics
size_t pmm_get_free_pages(void);

// Shared zero page — a single physically-allocated page whose contents are
// always zero.  Used by the demand-pager for anonymous read faults so that
// physical frames are not allocated until the process actually writes.
// Returns the physical address of the zero page, or 0 if not yet ready.
uint64_t pmm_get_zero_page_phys(void);

#endif
