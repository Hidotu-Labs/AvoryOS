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
#include <linuxkpi/gpu_diag.h>

/* Identity DMA still has to fit the device's address mask. No bounce
 * buffers/IOMMU translation exist: report failure instead of truncation. */
static bool kpi_dma_range_ok(dma_addr_t addr, size_t size, u64 mask) {
  return size && addr != DMA_MAPPING_ERROR && addr <= mask &&
         size - 1 <= mask - addr;
}

static dma_addr_t kpi_dma_phys(const void *ptr) {
  /* dma_map_single requires directly mapped, physically contiguous memory;
   * vmalloc buffers must use page/SG mapping instead. */
  if (!ptr || is_vmalloc_addr(ptr))
    return DMA_MAPPING_ERROR;
  return (dma_addr_t)(unsigned long)__pa(ptr);
}

static void *kpi_dma_alloc(size_t size, dma_addr_t *dma_handle,
                           gfp_t flag, u64 mask) {
  struct page *page;
  unsigned int order;
  size_t pages;


  if (!size) {
    if (dma_handle)
      *dma_handle = 0;
    return ZERO_SIZE_PTR;
  }

  if (size > ~(size_t)0 - (PAGE_SIZE - 1))
    return NULL;
  pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  order = get_order(pages << PAGE_SHIFT);

  if (mask && mask < 0x100000000ULL)
    page = alloc_pages(flag | __GFP_ZERO | GFP_DMA32, order);
  else
    page = alloc_pages(flag | __GFP_ZERO, order);
  if (!page)
    return NULL;

  if (!kpi_dma_range_ok(page_to_phys(page), size,
                        mask)) {
    __free_pages(page, order);
    return NULL;
  }
  if (dma_handle)
    *dma_handle = page_to_phys(page);
  return page_address(page);
}

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle,
                      gfp_t flags, unsigned long attrs) {
  (void)attrs;
  return kpi_dma_alloc(size, handle, flags,
                       dev ? dev->coherent_dma_mask : DMA_BIT_MASK(64));
}
void *dma_alloc_noncoherent(struct device *dev, size_t size, dma_addr_t *handle,
    enum dma_data_direction dir, gfp_t flags) {
  if (dir == DMA_NONE)
    return NULL;
  return kpi_dma_alloc(size, handle, flags,
                       dev ? dev->dma_mask : DMA_BIT_MASK(64));
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

void dma_free_noncoherent(struct device *dev, size_t size, void *addr,
    dma_addr_t handle, enum dma_data_direction dir) {
  (void)dir;
  dma_free_attrs(dev, size, addr, handle, 0);
}

dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page,
                              size_t offset, size_t size,
                              enum dma_data_direction dir,
                              unsigned long attrs) {
  (void)dev;
  (void)dir;
  (void)attrs;
  if (!page || offset > DMA_MAPPING_ERROR - page_to_phys(page))
    return DMA_MAPPING_ERROR;
  dma_addr_t addr = (dma_addr_t)page_to_phys(page) + offset;
  dma_addr_t mapped = kpi_dma_range_ok(addr, size, dev ? dev->dma_mask : DMA_BIT_MASK(64))
                          ? addr : DMA_MAPPING_ERROR;
  if (gpu_diag_resource_enabled())
    gpu_diag_record(GPU_DIAG_DMA_MAP, page, addr, mapped, size, offset,
                    mapped == DMA_MAPPING_ERROR ? -EIO : 0);
  return mapped;
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
  dma_addr_t addr = kpi_dma_phys(ptr);
  return kpi_dma_range_ok(addr, size, dev ? dev->dma_mask : DMA_BIT_MASK(64))
             ? addr : DMA_MAPPING_ERROR;
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
  return kpi_dma_range_ok(phys_addr, size, dev ? dev->dma_mask : DMA_BIT_MASK(64))
             ? (dma_addr_t)phys_addr : DMA_MAPPING_ERROR;
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

int dma_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
               enum dma_data_direction dir) {
  struct scatterlist *sg;
  int i;
  if (!sgl || nents <= 0)
    return 0;
  for_each_sg(sgl, sg, nents, i) {
    dma_addr_t addr = dma_map_page_attrs(dev, sg_page(sg), sg->offset,
                                         sg->length, dir, 0);
    if (addr == DMA_MAPPING_ERROR) {
      for_each_sg(sgl, sg, nents, i) {
        sg->dma_address = DMA_MAPPING_ERROR;
        sg->dma_length = 0;
      }
      return 0;
    }
    sg->dma_address = addr;
    sg->dma_length = sg->length;
  }
  return nents;
}

int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
                    enum dma_data_direction dir, unsigned long attrs) {
  int mapped;
  (void)attrs;
  if (!sgt || !sgt->sgl || !sgt->orig_nents)
    return -EINVAL;
  mapped = dma_map_sg(dev, sgt->sgl, sgt->orig_nents, dir);
  sgt->nents = mapped;
  return mapped ? 0 : -EIO;
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
  /* GTT textures are WC: dma_wmb() is only a compiler barrier on x86
   * (__dma_wmb()->barrier()), which does not drain WC buffers. A GPU sampling
   * right after a CPU upload would read stale/green texels until natural
   * eviction. sfence drains WC and is only a compiler barrier for WB. */
  __asm__ volatile("sfence" ::: "memory");
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
  /* Same WC-drain reason as dma_sync_single_for_device: dma_wmb() alone does
   * not drain GTT WC buffers before the GPU samples the texture. */
  __asm__ volatile("sfence" ::: "memory");
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
  if (!dev || mask < 0xFFFFFFFFULL)
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
