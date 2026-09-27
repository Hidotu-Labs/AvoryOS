#ifndef VMA_H
#define VMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Linux mmap flags (must match sys_mm.c)
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_GROWSDOWN 0x0100
#define MAP_HUGETLB 0x40000  /* Linux user-space flag: request huge pages */
/* Kernel-internal flags — stored in vma.flags, never exposed to user space */
#define MAP_SYSV_SHM 0x100000000ULL
/* Kernel-internal: use 2 MB huge pages for this anonymous VMA.
 * Set automatically when madvise(MADV_HUGEPAGE) or MAP_HUGETLB is used. */
#define MAP_HUGEPAGE 0x200000000ULL
/* Kernel-internal: VMA created by the generic file-mmap path, so its PTEs hold
 * a PMM reference to page-cache frames that teardown must drop.  System V shm
 * and driver mmaps (DRM, fb, tmpfs) own their frames and must not set it. */
#define MAP_PAGECACHE 0x400000000ULL

// VMA structure - internally represents an AVL Interval Tree Node
struct vma {
  uint64_t start;   // Start virtual address (page-aligned)
  uint64_t end;     // End virtual address (page-aligned, exclusive)
  uint64_t max_end; // Subtree Tracking for Interval overlap queries
  uint64_t prot;    // Protection flags (PROT_READ, PROT_WRITE, PROT_EXEC)
  uint64_t flags;   // Mapping flags (MAP_SHARED, MAP_PRIVATE, MAP_ANONYMOUS)
  uint64_t offset;    // File offset (for file-backed mappings)
  uint64_t file_size; // File size in bytes (for demand paging ELF segments)
  int fd; // File descriptor (for file-backed mappings, -1 if anonymous)
  void *file_node; // VFS node pointer (for demand paging)

  int height; // AVL Balance Height Tracker
  struct vma *left;
  struct vma *right;
};

// VMA list for a process (now a Tree Root)
struct vma_list {
  struct vma *root;
  int count; // Number of active dynamically allocated regions
};

// Initialize a VMA list
void vma_list_init(struct vma_list *list);

// Destroy a VMA list, freeing all AVL tree nodes
void vma_list_destroy(struct vma_list *list);

// Add a new VMA region, returns 0 on success or -1 on overlap/OOM
int vma_add(struct vma_list *list, uint64_t start, uint64_t end, uint64_t prot,
            uint64_t flags, int fd, uint64_t offset, void *file_node,
            uint64_t file_size);

// Remove a VMA region by address range (auto-splits and auto-unmaps Native
// structures) Returns true if any region was removed/split
bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end);

// Update protection bits for a range, splitting VMAs if necessary.
int vma_mprotect(struct vma_list *list, uint64_t start, uint64_t end,
                 uint64_t new_prot);

// Find VMA containing a given address (O(log n))
struct vma *vma_find(struct vma_list *list, uint64_t addr);

// Find VMA that overlaps with given range (O(log n) Interval lookup)
struct vma *vma_find_overlap(struct vma_list *list, uint64_t start,
                             uint64_t end);

// Find nearest GROWSDOWN VMA below cr2 within max_limit
struct vma *vma_find_growdown(struct vma_list *list, uint64_t cr2,
                              uint64_t max_limit);

// Scan tree to find the first linearly unmapped gap capable of fitting 'length'
// cleanly.
uint64_t vma_find_gap(struct vma_list *list, uint64_t length,
                      uint64_t base_addr, uint64_t limit_addr);

// Dynamically condense adjacent matching boundary limits sequentially into
// monolithic mappings natively.
void vma_merge_adjacent(struct vma_list *list);

// Clone VMA list for fork (shared mappings stay shared, private get copied)
void vma_list_clone(struct vma_list *dst, struct vma_list *src);

// Dump VMA list to klog for debugging
void vma_dump(struct vma_list *list);

#endif // VMA_H
