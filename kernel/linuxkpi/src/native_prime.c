/* Native display GEM <-> Linux dma-buf (AMDGPU, vgem, and DRI3). */
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linuxkpi/native_mm.h>
#include <linuxkpi/native_prime.h>
#include <linuxkpi/native_vfs.h>
#include <linuxkpi/gpu_diag.h>
#include <linuxkpi/native_sched.h>

struct native_prime_bo {
  void *object;
  struct page **pages;
  unsigned int count;
  void (*release)(void *);
};

struct native_prime_mapping {
  struct dma_buf_attachment *attach;
  struct sg_table *sgt;
  struct iosys_map map;
  struct mutex cpu_lock;
};

static struct sg_table *native_map(struct dma_buf_attachment *attach,
                                  enum dma_data_direction dir) {
  struct native_prime_bo *bo = attach->dmabuf->priv;
  struct sg_table *sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
  int ret;
  if (!sgt)
    return ERR_PTR(-ENOMEM);
  ret = sg_alloc_table_from_pages(sgt, bo->pages, bo->count, 0,
                                 attach->dmabuf->size, GFP_KERNEL);
  if (!ret)
    ret = dma_map_sgtable(attach->dev, sgt, dir, 0);
  if (ret) {
    sg_free_table(sgt);
    kfree(sgt);
    return ERR_PTR(ret);
  }
  return sgt;
}

static void native_unmap(struct dma_buf_attachment *attach, struct sg_table *sgt,
                         enum dma_data_direction dir) {
  dma_unmap_sgtable(attach->dev, sgt, dir, 0);
  sg_free_table(sgt);
  kfree(sgt);
}

static void native_release(struct dma_buf *buf) {
  struct native_prime_bo *bo = buf->priv;
  bo->release(bo->object);
  kfree(bo->pages);
  kfree(bo);
}

static int native_mmap(struct dma_buf *buf, struct vm_area_struct *vma) {
  struct native_prime_bo *bo = buf->priv;
  unsigned long count = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
  if (vma->vm_pgoff > bo->count || count > bo->count - vma->vm_pgoff)
    return -EINVAL;
  for (unsigned long i = 0; i < count; i++)
    if (remap_pfn_range(vma, vma->vm_start + i * PAGE_SIZE,
                        page_to_pfn(bo->pages[vma->vm_pgoff + i]), PAGE_SIZE,
                        vma->vm_page_prot))
      return -EAGAIN;
  vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
  return 0;
}

static int native_vmap(struct dma_buf *buf, struct iosys_map *map) {
  struct native_prime_bo *bo = buf->priv;
  void *address = vmap(bo->pages, bo->count, VM_MAP, PAGE_KERNEL);
  if (!address)
    return -ENOMEM;
  iosys_map_set_vaddr(map, address);
  return 0;
}

static void native_vunmap(struct dma_buf *buf, struct iosys_map *map) {
  vunmap(map->vaddr);
  iosys_map_clear(map);
}

static int native_cpu_access(struct dma_buf *buf, enum dma_data_direction dir) {
  dma_mb();
  return 0;
}

static const struct dma_buf_ops native_ops = {
  .map_dma_buf = native_map, .unmap_dma_buf = native_unmap,
  .release = native_release, .mmap = native_mmap,
  .vmap = native_vmap, .vunmap = native_vunmap,
  .begin_cpu_access = native_cpu_access, .end_cpu_access = native_cpu_access,
};

/* Each published fd owns one file reference and a distinct native node.
 * This also allows re-export after the original fd was closed: the held
 * dma-buf outlives that fd's native node. */
int linuxkpi_prime_fd(void *buffer, unsigned int flags) {
  struct dma_buf *buf = buffer;
  void *node;
  int fd;
  if (flags & ~(O_CLOEXEC | O_RDWR))
    return -EINVAL;
  node = asc_vfs_anon_node();
  if (!node)
    return -ENOMEM;
  fd = asc_vfs_fd_alloc(flags);
  if (fd < 0) {
    asc_vfs_node_release_kernel(node);
    return fd;
  }
  asc_vfs_node_set_name(node, "dmabuf");
  asc_vfs_node_set_size(node, buf->size);
  asc_vfs_node_set_device(node, get_file(buf->file));
  asc_vfs_node_set_wait_queue(node, buf->file->f_poll_wq);
  asc_vfs_fd_install(fd, node, flags);
  return fd;
}

/* Consumes the caller's native object reference, including on failure. */
int linuxkpi_prime_export(void *object, size_t size, void *reservation,
                         unsigned long (*page_phys)(void *, unsigned int),
                         void (*release)(void *), unsigned int flags) {
  DEFINE_DMA_BUF_EXPORT_INFO(info);
  struct native_prime_bo *bo;
  struct dma_buf *buf;
  int ret = -EINVAL;
  if (!size || (size & ~PAGE_MASK) || size > UINT_MAX ||
      flags & ~(O_CLOEXEC | O_RDWR))
    goto fail_object;
  bo = kzalloc(sizeof(*bo), GFP_KERNEL);
  if (!bo) { ret = -ENOMEM; goto fail_object; }
  bo->count = size >> PAGE_SHIFT;
  bo->object = object;
  bo->release = release;
  bo->pages = kcalloc(bo->count, sizeof(*bo->pages), GFP_KERNEL);
  if (!bo->pages) { ret = -ENOMEM; goto fail_bo; }
  for (unsigned int i = 0; i < bo->count; i++) {
    unsigned long phys = page_phys(object, i);
    /* Native scanout MMIO (e.g. the GOP framebuffer) is not shareable RAM. */
    if (!phys || (phys & ~PAGE_MASK) || !asc_pmm_is_managed(phys))
      goto fail_pages;
    bo->pages[i] = pfn_to_page(phys >> PAGE_SHIFT);
    if (!bo->pages[i]) { ret = -ENOMEM; goto fail_pages; }
  }
  info.ops = &native_ops;
  info.size = size;
  info.flags = O_RDWR;
  info.priv = bo;
  info.resv = reservation;
  buf = dma_buf_export(&info);
  if (IS_ERR(buf)) { ret = PTR_ERR(buf); goto fail_pages; }
  ret = linuxkpi_prime_fd(buf, flags);
  dma_buf_put(buf);
  return ret;
fail_pages:
  kfree(bo->pages);
fail_bo:
  kfree(bo);
fail_object:
  release(object);
  return ret;
}

int linuxkpi_prime_get(int fd, void **buffer, size_t *size) {
  struct dma_buf *buf = dma_buf_get(fd);
  if (IS_ERR(buf))
    return PTR_ERR(buf);
  *buffer = buf;
  *size = buf->size;
  return 0;
}

void linuxkpi_prime_put(void *buffer) { dma_buf_put(buffer); }

void *linuxkpi_prime_native_object(void *buffer) {
  struct dma_buf *buf = buffer;
  return buf->ops == &native_ops ? ((struct native_prime_bo *)buf->priv)->object : NULL;
}

static struct device prime_device;
static u64 prime_dma_mask = DMA_BIT_MASK(64);
static bool prime_device_ready;
static DEFINE_MUTEX(prime_device_lock);

int linuxkpi_prime_map(void *buffer, void **mapping, void **address) {
  struct dma_buf *buf = buffer;
  struct native_prime_mapping *m;
  int ret;
  unsigned int stage = GPU_PRIME_OBJECT_ALLOC;
  mutex_lock(&prime_device_lock);
  if (!prime_device_ready) {
    device_initialize(&prime_device);
    dev_set_name(&prime_device, "ascentdrm-prime");
    prime_device.dma_mask = prime_dma_mask;
    prime_device.coherent_dma_mask = prime_dma_mask;
    prime_device_ready = true;
  }
  mutex_unlock(&prime_device_lock);
  m = kzalloc(sizeof(*m), GFP_KERNEL);
  if (!m) {
    gpu_diag_prime_error(stage, buf, 0, buf->size, -ENOMEM);
    return -ENOMEM;
  }
  mutex_init(&m->cpu_lock);
  /* A non-dynamic attachment pins the exporter in shared system memory;
   * its scatterlist and CPU mapping stay valid until native GEM teardown. */
  stage = GPU_PRIME_ATTACH;
  m->attach = dma_buf_attach(buf, &prime_device);
  if (IS_ERR(m->attach)) { ret = PTR_ERR(m->attach); goto fail; }
  stage = GPU_PRIME_SG_MAP;
  m->sgt = dma_buf_map_attachment_unlocked(m->attach, DMA_BIDIRECTIONAL);
  if (IS_ERR(m->sgt)) { ret = PTR_ERR(m->sgt); goto fail_attach; }
  stage = GPU_PRIME_CPU_MAP;
  ret = dma_buf_vmap_unlocked(buf, &m->map);
  if (ret)
    goto fail_sgt;
  if (m->map.is_iomem) {
    stage = GPU_PRIME_IOMEM;
    ret = -EOPNOTSUPP;
    dma_buf_vunmap_unlocked(buf, &m->map);
    goto fail_sgt;
  }
  *mapping = m;
  *address = m->map.vaddr;
  return 0;
fail_sgt:
  dma_buf_unmap_attachment_unlocked(m->attach, m->sgt, DMA_BIDIRECTIONAL);
fail_attach:
  dma_buf_detach(buf, m->attach);
fail:
  gpu_diag_prime_error(stage, buf, 0, buf->size, ret);
  kfree(m);
  return ret;
}

void linuxkpi_prime_unmap(void *mapping) {
  struct native_prime_mapping *m = mapping;
  struct dma_buf *buf = m->attach->dmabuf;
  dma_buf_vunmap_unlocked(buf, &m->map);
  dma_buf_unmap_attachment_unlocked(m->attach, m->sgt, DMA_BIDIRECTIONAL);
  dma_buf_detach(buf, m->attach);
  kfree(m);
}

unsigned long linuxkpi_prime_page_phys(void *mapping, unsigned int page) {
  struct native_prime_mapping *m = mapping;
  struct scatterlist *sg;
  unsigned int i;
  unsigned long offset = (unsigned long)page << PAGE_SHIFT;
  for_each_sg(m->sgt->sgl, sg, m->sgt->orig_nents, i) {
    if (offset < sg->length)
      return sg_page(sg) ? page_to_phys(sg_page(sg)) + sg->offset + offset : 0;
    offset -= sg->length;
  }
  return 0;
}

int linuxkpi_prime_begin_cpu(void *buffer, void *mapping) {
  struct dma_buf *buf = buffer;
  struct native_prime_mapping *m = mapping;
  int ret;
  unsigned long long start=linuxkpi_monotonic_ms();

  mutex_lock(&m->cpu_lock);
  /* Exporter callbacks may reserve their BO; call them without resv held. */
  ret = dma_buf_begin_cpu_access(buf, DMA_FROM_DEVICE);
  if (ret)
    goto fail;

  /* begin_cpu_access waits for an existing fence snapshot, but does not
   * exclude new GPU submissions. Keep the reservation locked while native
   * scanout copies the pixels, and recheck fences added before we got it.
   * Otherwise a busy front buffer can change halfway through the upload. */
  ret = dma_resv_lock_interruptible(buf->resv, NULL);
  if (ret)
    goto end_access;
  if (gpu_diag_level()) {
    struct dma_resv_iter cursor;
    struct dma_fence *fence;
    unsigned int n=0;
    dma_resv_for_each_fence(&cursor,buf->resv,DMA_RESV_USAGE_BOOKKEEP,fence) {
      gpu_diag_record(GPU_DIAG_FENCE,buf,fence->context,fence->seqno,
                      dma_resv_iter_usage(&cursor),READ_ONCE(fence->flags),0);
      if (++n==8) break;
    }
  }
  long waited = dma_resv_wait_timeout(buf->resv, DMA_RESV_USAGE_WRITE, true,
                                      MAX_SCHEDULE_TIMEOUT);
  if (waited <= 0) {
    ret = waited < 0 ? (int)waited : -ETIMEDOUT;
    dma_resv_unlock(buf->resv);
    goto end_access;
  }
  gpu_diag_record(GPU_DIAG_CPU_BEGIN,buf,(unsigned long)m->map.vaddr,buf->size,
                  linuxkpi_monotonic_ms()-start,0,0);
  return 0;

end_access:
  dma_buf_end_cpu_access(buf, DMA_FROM_DEVICE);
fail:
  gpu_diag_record(GPU_DIAG_CPU_BEGIN,buf,(unsigned long)m->map.vaddr,buf->size,
                  linuxkpi_monotonic_ms()-start,0,ret);
  mutex_unlock(&m->cpu_lock);
  return ret;
}
void linuxkpi_prime_end_cpu(void *buffer, void *mapping) {
  struct dma_buf *buf = buffer;
  struct native_prime_mapping *m = mapping;
  dma_resv_unlock(buf->resv);
  int ret=dma_buf_end_cpu_access(buf, DMA_FROM_DEVICE);
  gpu_diag_record(GPU_DIAG_CPU_END,buf,(unsigned long)m->map.vaddr,buf->size,0,0,ret);
  mutex_unlock(&m->cpu_lock);
}

void *linuxkpi_prime_reservation_new(void) {
  struct dma_resv *resv = kmalloc(sizeof(*resv), GFP_KERNEL);
  if (resv)
    dma_resv_init(resv);
  return resv;
}

void linuxkpi_prime_reservation_free(void *reservation) {
  if (reservation) {
    dma_resv_fini(reservation);
    kfree(reservation);
  }
}

int linuxkpi_prime_reservation_wait(void *reservation) {
  long ret = dma_resv_wait_timeout(reservation, DMA_RESV_USAGE_WRITE, true,
                                   MAX_SCHEDULE_TIMEOUT);
  return ret < 0 ? ret : 0;
}

unsigned long linuxkpi_prime_mmap(void *buffer, unsigned long addr,
                                 unsigned long length, unsigned long prot,
                                 unsigned long flags, unsigned long offset) {
  struct dma_buf *buf = buffer;
  return linuxkpi_file_mmap_file(buf->file, addr, length, prot, flags, offset);
}
