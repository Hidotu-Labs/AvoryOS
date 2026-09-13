#ifndef __AVORY_LINUXKPI_DMA_MAPPING_H
#define __AVORY_LINUXKPI_DMA_MAPPING_H

/* AvoryOS overlay for <linux/dma-mapping.h>.
 *
 * x86 DMA is coherent and identity-mapped in this kernel: a DMA address is
 * the physical address, and the CPU sees all memory write-back cached, so
 * the sync operations are compiler/CPU barriers.  Coherent allocations come
 * from the Linux page allocator (contiguous, zeroed).  Implementations are in
 * linuxkpi/src/dma-mapping.c. */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/scatterlist.h>
#include <linux/dma-direction.h>
#include <asm/barrier.h>

#define DMA_MAPPING_ERROR (~(dma_addr_t)0)

#define DMA_ATTR_WRITE_BARRIER (1UL << 0)
#define DMA_ATTR_WEAK_ORDERING (1UL << 1)
#define DMA_ATTR_WRITE_COMBINE (1UL << 2)
#define DMA_ATTR_NON_CONSISTENT (1UL << 3)
#define DMA_ATTR_NO_KERNEL_MAPPING (1UL << 4)
#define DMA_ATTR_SKIP_CPU_SYNC (1UL << 5)
#define DMA_ATTR_FORCE_CONTIGUOUS (1UL << 6)
#define DMA_ATTR_ALLOC_SINGLE_PAGES (1UL << 7)
#define DMA_ATTR_NO_WARN (1UL << 8)
#define DMA_ATTR_PRIVILEGED (1UL << 9)

#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle,
                         gfp_t flag);
void dma_free_coherent(struct device *dev, size_t size, void *cpu_addr,
                       dma_addr_t dma_handle);

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
                      gfp_t flag, unsigned long attrs);
void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
                    dma_addr_t dma_handle, unsigned long attrs);

dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page,
                              size_t offset, size_t size,
                              enum dma_data_direction dir, unsigned long attrs);
void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
                          enum dma_data_direction dir, unsigned long attrs);

dma_addr_t dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
                                enum dma_data_direction dir,
                                unsigned long attrs);
void dma_unmap_single_attrs(struct device *dev, dma_addr_t addr, size_t size,
                            enum dma_data_direction dir, unsigned long attrs);

int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
                    enum dma_data_direction dir, unsigned long attrs);
void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
                       enum dma_data_direction dir, unsigned long attrs);

void dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
                             enum dma_data_direction dir);
void dma_sync_single_for_device(struct device *dev, dma_addr_t addr,
                                size_t size, enum dma_data_direction dir);
void dma_sync_sgtable_for_cpu(struct device *dev, struct sg_table *sgt,
                              enum dma_data_direction dir);
void dma_sync_sgtable_for_device(struct device *dev, struct sg_table *sgt,
                                 enum dma_data_direction dir);

int dma_set_mask(struct device *dev, u64 mask);
int dma_set_coherent_mask(struct device *dev, u64 mask);
int dma_set_mask_and_coherent(struct device *dev, u64 mask);
u64 dma_get_required_mask(struct device *dev);
size_t dma_max_mapping_size(struct device *dev);

static inline int dma_coerce_mask_and_coherent(struct device *dev, u64 mask) {
  dev->coherent_dma_mask = mask;
  return dma_set_mask_and_coherent(dev, mask);
}

static inline bool dma_mapping_error(struct device *dev, dma_addr_t dma_addr) {
  (void)dev;
  return dma_addr == DMA_MAPPING_ERROR;
}

static inline dma_addr_t dma_map_single(struct device *dev, void *ptr,
                                        size_t size,
                                        enum dma_data_direction dir) {
  return dma_map_single_attrs(dev, ptr, size, dir, 0);
}

/* Segment/pressure helpers (identity-mapped DMA) and resource mapping for
 * peer/device physical addresses (amdgpu_vram_mgr's VRAM mappings). */
static inline void dma_set_max_seg_size(struct device *dev,
                                        unsigned int size) {
  (void)dev;
  (void)size;
}
static inline bool dma_addressing_limited(struct device *dev) {
  (void)dev;
  return false;
}
dma_addr_t dma_map_resource(struct device *dev, phys_addr_t phys_addr,
                            size_t size, enum dma_data_direction dir,
                            unsigned long attrs);
void dma_unmap_resource(struct device *dev, dma_addr_t addr, size_t size,
                        enum dma_data_direction dir, unsigned long attrs);

static inline void dma_unmap_single(struct device *dev, dma_addr_t addr,
                                    size_t size,
                                    enum dma_data_direction dir) {
  dma_unmap_single_attrs(dev, addr, size, dir, 0);
}

static inline dma_addr_t dma_map_page(struct device *dev, struct page *page,
                                      size_t offset, size_t size,
                                      enum dma_data_direction dir) {
  return dma_map_page_attrs(dev, page, offset, size, dir, 0);
}

static inline void dma_unmap_page(struct device *dev, dma_addr_t addr,
                                  size_t size,
                                  enum dma_data_direction dir) {
  dma_unmap_page_attrs(dev, addr, size, dir, 0);
}

static inline int dma_map_sg(struct device *dev, struct scatterlist *sg,
                             int nents, enum dma_data_direction dir) {
  return nents;
}

static inline void dma_unmap_sg(struct device *dev, struct scatterlist *sg,
                                int nents, enum dma_data_direction dir) {
  (void)dev;
  (void)sg;
  (void)nents;
  (void)dir;
}

static inline void dma_sync_sg_for_cpu(struct device *dev,
                                       struct scatterlist *sg, int nents,
                                       enum dma_data_direction dir) {
  (void)dev;
  (void)sg;
  (void)nents;
  (void)dir;
}

static inline void dma_sync_sg_for_device(struct device *dev,
                                          struct scatterlist *sg, int nents,
                                          enum dma_data_direction dir) {
  (void)dev;
  (void)sg;
  (void)nents;
  (void)dir;
}

#endif /* __AVORY_LINUXKPI_DMA_MAPPING_H */
