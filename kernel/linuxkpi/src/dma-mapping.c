/* LinuxKPI DMA mapping (header: linux/dma-mapping.h).
 *
 * Identity mapping: dma_addr_t == physical address.  Coherent allocations are
 * contiguous, zeroed pages from the Linux page allocator.  x86 memory is
 * coherent (write-back) and the CPU writes through the same cache the device
 * snoops, so the sync operations are ordering barriers, not cache flushes.
 * This is deliberately *not* the native UC retype path used by
 * mm/dma_alloc.c for framebuffers. */

#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/string.h>

/* Physical address behind a kernel virtual address, including vmalloc'd
 * buffers (which are physically contiguous only if size <= one page). */
static dma_addr_t kpi_dma_phys(const void *ptr) {
  if (is_vmalloc_addr(ptr)) {
    struct page *page = vmalloc_to_page(ptr);
    if (!page)
      return DMA_MAPPING_ERROR;
    return page_to_phys(page) + offset_in_page(ptr);
  }
  return (dma_addr_t)(unsigned long)__pa(ptr);
}

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
                      gfp_t flag, unsigned long attrs) {
  struct page *page;
  unsigned int order;
  size_t pages;

  (void)dev;
  (void)attrs;

  if (!size) {
    if (dma_handle)
      *dma_handle = 0;
    return ZERO_SIZE_PTR;
  }

  pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  order = get_order(pages << PAGE_SHIFT);

  if (dev && dev->coherent_dma_mask && dev->coherent_dma_mask < 0x100000000ULL)
    page = alloc_pages(flag | __GFP_ZERO | GFP_DMA32, order);
  else
    page = alloc_pages(flag | __GFP_ZERO, order);
  if (!page)
    return NULL;

  if (dma_handle)
    *dma_handle = page_to_phys(page);
  return page_address(page);
}

void *dma_alloc_coherent(struct device *dev, size_t size,
                         dma_addr_t *dma_handle, gfp_t flag) {
  return dma_alloc_attrs(dev, size, dma_handle, flag, 0);
}

void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
                    dma_addr_t dma_handle, unsigned long attrs) {
  unsigned int order;
  size_t pages;

  (void)dev;
  (void)attrs;

  if (!size || !cpu_addr || ZERO_OR_NULL_PTR(cpu_addr))
    return;

  pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  order = get_order(pages << PAGE_SHIFT);

  if (is_vmalloc_addr(cpu_addr))
    vfree(cpu_addr);
  else
    __free_pages(phys_to_page((phys_addr_t)dma_handle), order);
}

void dma_free_coherent(struct device *dev, size_t size, void *cpu_addr,
                       dma_addr_t dma_handle) {
  dma_free_attrs(dev, size, cpu_addr, dma_handle, 0);
}

dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page,
                              size_t offset, size_t size,
                              enum dma_data_direction dir,
                              unsigned long attrs) {
  (void)dev;
  (void)dir;
  (void)attrs;
  (void)size;
  return (dma_addr_t)page_to_phys(page) + offset;
}

void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
                          enum dma_data_direction dir, unsigned long attrs) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)dir;
  (void)attrs;
}

dma_addr_t dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
                                enum dma_data_direction dir,
                                unsigned long attrs) {
  (void)dev;
  (void)size;
  (void)dir;
  (void)attrs;
  return kpi_dma_phys(ptr);
}

/* Resource mapping is identity too (no IOMMU): amdgpu_vram_mgr maps VRAM
 * pages through dma_map_resource() and expects the physical address back. */
dma_addr_t dma_map_resource(struct device *dev, phys_addr_t phys_addr,
                            size_t size, enum dma_data_direction dir,
                            unsigned long attrs) {
  (void)dev;
  (void)size;
  (void)dir;
  (void)attrs;
  return (dma_addr_t)phys_addr;
}

void dma_unmap_resource(struct device *dev, dma_addr_t addr, size_t size,
                        enum dma_data_direction dir, unsigned long attrs) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)dir;
  (void)attrs;
}

void dma_unmap_single_attrs(struct device *dev, dma_addr_t addr, size_t size,
                            enum dma_data_direction dir, unsigned long attrs) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)dir;
  (void)attrs;
}

int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
                    enum dma_data_direction dir, unsigned long attrs) {
  struct scatterlist *sg;
  int i;

  (void)dev;
  (void)dir;

  for_each_sgtable_sg(sgt, sg, i) {
    sg->dma_address = (dma_addr_t)page_to_phys(sg_page(sg)) + sg->offset;
    sg->dma_length = sg->length;
  }
  return sgt->nents; /* 0 means failure; nents >= 1 on success */
}

void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
                       enum dma_data_direction dir, unsigned long attrs) {
  (void)dev;
  (void)sgt;
  (void)dir;
  (void)attrs;
}

void dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
                             enum dma_data_direction dir) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)dir;
  dma_mb();
}

void dma_sync_single_for_device(struct device *dev, dma_addr_t addr,
                                size_t size, enum dma_data_direction dir) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)dir;
  dma_wmb();
}

void dma_sync_sgtable_for_cpu(struct device *dev, struct sg_table *sgt,
                              enum dma_data_direction dir) {
  (void)dev;
  (void)sgt;
  (void)dir;
  dma_mb();
}

void dma_sync_sgtable_for_device(struct device *dev, struct sg_table *sgt,
                                 enum dma_data_direction dir) {
  (void)dev;
  (void)sgt;
  (void)dir;
  dma_wmb();
}

int dma_set_mask(struct device *dev, u64 mask) {
  if (!dev)
    return -EIO;
  if (mask < 0xFFFFFFFFULL)
    return -EIO; /* narrower than 32 bits is unsupported on this platform */
  dev->dma_mask = mask;
  return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask) {
  if (!dev)
    return -EIO;
  dev->coherent_dma_mask = mask;
  return 0;
}

int dma_set_mask_and_coherent(struct device *dev, u64 mask) {
  int rc = dma_set_mask(dev, mask);

  if (rc == 0)
    dma_set_coherent_mask(dev, mask);
  return rc;
}

u64 dma_get_required_mask(struct device *dev) {
  if (dev && dev->dma_mask)
    return dev->dma_mask;
  return DMA_BIT_MASK(64);
}

size_t dma_max_mapping_size(struct device *dev) {
  (void)dev;
  return ~(size_t)0;
}
