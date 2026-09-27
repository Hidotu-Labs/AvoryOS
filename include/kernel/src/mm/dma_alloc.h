#ifndef MM_DMA_ALLOC_H
#define MM_DMA_ALLOC_H

#include <stddef.h>
#include <stdint.h>

// DMA allocation flags
#define DMA_FLAG_32BIT  0x01  // Allocate below 4GB (for 32-bit DMA devices)
#define DMA_FLAG_LOW    0x02  // Allocate from low memory (below 16MB)
#define DMA_FLAG_NOCACHE 0x04 // Map as uncached (default behavior)
#define DMA_FLAG_ANYWHERE 0x08 // Device supports full 64-bit physical addresses

// DMA buffer structure - tracks both physical and virtual addresses
typedef struct {
    void *virt;      // Virtual address (for CPU access)
    uint64_t phys;   // Physical address (for device DMA)
    size_t size;     // Size in bytes
    size_t pages;    // Number of pages
} dma_buffer_t;

// Initialize the DMA allocator
void dma_alloc_init(void);

// Allocate a DMA buffer
// Returns NULL on failure
// The buffer is mapped as uncached (PCD|PWT) by default
dma_buffer_t *dma_alloc(size_t size, uint32_t flags);

// Free a DMA buffer
void dma_free(dma_buffer_t *buf);

// Convenience functions for common use cases

// Allocate a single page for DMA (32-bit addressable, uncached)
void *dma_alloc_page(uint64_t *phys_out);
void *dma_alloc_page_flags(uint32_t flags, uint64_t *phys_out);

// Allocate multiple pages for DMA
void *dma_alloc_pages(size_t count, uint64_t *phys_out);
void *dma_alloc_pages_flags(size_t count, uint32_t flags,
                            uint64_t *phys_out);

// Free a DMA page allocation
void dma_free_page(void *virt);

const char *dma_get_last_failure(void);
uint32_t dma_get_last_flags(void);
uint64_t dma_get_last_phys(void);

#endif // MM_DMA_ALLOC_H
