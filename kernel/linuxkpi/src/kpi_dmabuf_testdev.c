/* /dev/kpi_dmabuf - Phase 2 exit-criteria test device.
 *
 * A native devfs node (registered through kernel/src/linuxkpi/native_vfs.c)
 * whose per-open descriptor forwards into the Linux file bridge.  It exposes
 * a tiny dma-buf exporter to userspace so the remaining Phase 2 criteria can
 * be checked from a real process:
 *
 *   - ioctl(KPI_DMABUF_IOC_ALLOC) exports a fresh BO as a dma-buf fd;
 *   - mmap() on the device node maps the current BO through dma_buf_mmap(),
 *     and mmap() on the returned dma-buf fd uses the upstream
 *     dma_buf_mmap_internal() path;
 *   - the exporter installs vm_ops->close, and ioctl(CLOSE_COUNT) reports how
 *     often the kernel closed a mapping, which pins down exactly-once
 *     semantics across munmap and fork;
 *   - ioctl(IMPORT) runs dma_buf_get() on an inherited fd, exercising the
 *     PRIME-style fd-passing path;
 *   - ioctl(SYNC_FD)/SIGNAL/WAIT wrap a test fence in a sync_file so a child
 *     process can wait on an inherited sync_file fd;
 *   - ioctl(PMM_FREE) exposes the native PMM free-page count for soak checks.
 *
 * The device is a development aid.  It is a singleton: every open() gets a
 * fresh BO slot, but the close counter is global, which is exactly what the
 * tests want to observe from both sides of a fork().
 */

#include <linux/anon_inodes.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sync_file.h>
#include <linux/vmalloc.h>

#include <uapi/kpi_dmabuf.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>
#include <linuxkpi/native_vfs.h>

/* ── test BO exporter ───────────────────────────────────────────────────── */

struct kpi_dmabuf_bo {
  struct page **pages;
  unsigned int n_pages;
};

/* One per closed mapping.  Atomic because a process and a forked child can
 * munmap concurrently while another opener reads the value. */
static atomic_t kpi_dmabuf_close_count = ATOMIC_INIT(0);

static int kpi_bo_attach(struct dma_buf *dmabuf,
                         struct dma_buf_attachment *attach) {
  (void)dmabuf;
  (void)attach;
  return 0;
}

static void kpi_bo_detach(struct dma_buf *dmabuf,
                          struct dma_buf_attachment *attach) {
  (void)dmabuf;
  (void)attach;
}

static struct sg_table *kpi_bo_map(struct dma_buf_attachment *attach,
                                   enum dma_data_direction dir) {
  struct kpi_dmabuf_bo *bo = attach->dmabuf->priv;
  struct sg_table *sgt;
  int ret;

  (void)dir;
  sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
  if (!sgt)
    return ERR_PTR(-ENOMEM);

  ret = sg_alloc_table_from_pages(sgt, bo->pages, bo->n_pages, 0,
                                  (unsigned long)bo->n_pages << PAGE_SHIFT,
                                  GFP_KERNEL);
  if (ret) {
    kfree(sgt);
    return ERR_PTR(ret);
  }
  for (unsigned int i = 0; i < sgt->nents; i++) {
    struct scatterlist *sg = &sgt->sgl[i];

    sg->dma_address = page_to_phys(sg_page(sg)) + sg->offset;
    sg->dma_length = sg->length;
  }
  return sgt;
}

static void kpi_bo_unmap(struct dma_buf_attachment *attach,
                         struct sg_table *sgt, enum dma_data_direction dir) {
  (void)attach;
  (void)dir;
  sg_free_table(sgt);
  kfree(sgt);
}

static void kpi_bo_release(struct dma_buf *dmabuf) {
  struct kpi_dmabuf_bo *bo = dmabuf->priv;

  for (unsigned int i = 0; i < bo->n_pages; i++)
    __free_page(bo->pages[i]);
  kfree(bo->pages);
  kfree(bo);
}

static void kpi_bo_vm_close(struct vm_area_struct *vma) {
  (void)vma;
  atomic_inc(&kpi_dmabuf_close_count);
}

static const struct vm_operations_struct kpi_bo_vm_ops = {
    .close = kpi_bo_vm_close,
};

static int kpi_bo_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma) {
  struct kpi_dmabuf_bo *bo = dmabuf->priv;
  unsigned long pgoff = vma->vm_pgoff;
  unsigned long size = vma->vm_end - vma->vm_start;

  for (unsigned long off = 0; off < size; off += PAGE_SIZE) {
    unsigned long page_idx = pgoff + (off >> PAGE_SHIFT);

    if (page_idx >= bo->n_pages)
      return -EINVAL;
    if (remap_pfn_range(vma, vma->vm_start + off,
                        page_to_pfn(bo->pages[page_idx]), PAGE_SIZE,
                        vma->vm_page_prot))
      return -EAGAIN;
  }

  /* Installed after the fault-time mappings so the bridge records it on the
   * Linux-facing vma and fires close() from the native munmap path. */
  vma->vm_ops = &kpi_bo_vm_ops;
  vma->vm_private_data = bo;
  vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
  return 0;
}

static int kpi_bo_begin_cpu_access(struct dma_buf *dmabuf,
                                   enum dma_data_direction dir) {
  (void)dmabuf;
  (void)dir;
  dma_mb();
  return 0;
}

static int kpi_bo_end_cpu_access(struct dma_buf *dmabuf,
                                 enum dma_data_direction dir) {
  (void)dmabuf;
  (void)dir;
  dma_mb();
  return 0;
}

static int kpi_bo_vmap(struct dma_buf *dmabuf, struct iosys_map *map) {
  struct kpi_dmabuf_bo *bo = dmabuf->priv;
  void *vaddr = vmap(bo->pages, bo->n_pages, VM_MAP, PAGE_KERNEL);

  if (!vaddr)
    return -ENOMEM;
  iosys_map_set_vaddr(map, vaddr);
  return 0;
}

static void kpi_bo_vunmap(struct dma_buf *dmabuf, struct iosys_map *map) {
  (void)dmabuf;
  vunmap(map->vaddr);
  iosys_map_clear(map);
}

static const struct dma_buf_ops kpi_bo_ops = {
    .attach = kpi_bo_attach,
    .detach = kpi_bo_detach,
    .map_dma_buf = kpi_bo_map,
    .unmap_dma_buf = kpi_bo_unmap,
    .release = kpi_bo_release,
    .mmap = kpi_bo_mmap,
    .begin_cpu_access = kpi_bo_begin_cpu_access,
    .end_cpu_access = kpi_bo_end_cpu_access,
    .vmap = kpi_bo_vmap,
    .vunmap = kpi_bo_vunmap,
};

static struct kpi_dmabuf_bo *kpi_bo_create(unsigned int n_pages) {
  struct kpi_dmabuf_bo *bo = kzalloc(sizeof(*bo), GFP_KERNEL);

  if (!bo)
    return NULL;

  bo->pages = kcalloc(n_pages, sizeof(*bo->pages), GFP_KERNEL);
  if (!bo->pages) {
    kfree(bo);
    return NULL;
  }

  for (unsigned int i = 0; i < n_pages; i++) {
    bo->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!bo->pages[i]) {
      for (unsigned int j = 0; j < i; j++)
        __free_page(bo->pages[j]);
      kfree(bo->pages);
      kfree(bo);
      return NULL;
    }
  }
  bo->n_pages = n_pages;
  return bo;
}

static void kpi_bo_destroy(struct kpi_dmabuf_bo *bo) {
  if (!bo)
    return;
  for (unsigned int i = 0; i < bo->n_pages; i++)
    __free_page(bo->pages[i]);
  kfree(bo->pages);
  kfree(bo);
}

/* ── test fence / sync_file ─────────────────────────────────────────────── */

struct kpi_test_fence {
  struct dma_fence base;
  spinlock_t lock;
};

static const char *kpi_fence_get_driver_name(struct dma_fence *f) {
  (void)f;
  return "kpi-dmabuf";
}

static const char *kpi_fence_get_timeline_name(struct dma_fence *f) {
  (void)f;
  return "kpi-dmabuf-test";
}

static const struct dma_fence_ops kpi_fence_ops = {
    .get_driver_name = kpi_fence_get_driver_name,
    .get_timeline_name = kpi_fence_get_timeline_name,
};

static struct kpi_test_fence *kpi_fence_create(void) {
  static atomic_t seqno = ATOMIC_INIT(0);
  struct kpi_test_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);

  if (!f)
    return NULL;
  spin_lock_init(&f->lock);
  dma_fence_init(&f->base, &kpi_fence_ops, &f->lock, 0,
                 (u64)atomic_inc_return(&seqno));
  return f;
}

/* ── per-open device state ──────────────────────────────────────────────── */

struct kpi_dmabuf_state {
  struct kpi_dmabuf_bo *bo;
  struct dma_buf *dmabuf; /* owns one file reference while set */
  struct dma_fence *fence;
};

static long kpi_dmabuf_ioctl_alloc(struct kpi_dmabuf_state *st,
                                   unsigned long arg) {
  unsigned long pages = arg ? arg : 64;
  struct dma_buf *dmabuf;
  DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
  int fd;

  if (pages > KPI_DMABUF_MAX_PAGES)
    return -EINVAL;

  /* One BO at a time: a new allocation replaces the previous one.  Existing
   * mappings keep their own reference to the old dma_buf, so they stay valid
   * until unmapped; this state reference can go. */
  if (st->dmabuf) {
    dma_buf_put(st->dmabuf);
    st->dmabuf = NULL;
    st->bo = NULL;
  }

  st->bo = kpi_bo_create((unsigned int)pages);
  if (!st->bo)
    return -ENOMEM;

  exp_info.ops = &kpi_bo_ops;
  exp_info.size = (size_t)pages << PAGE_SHIFT;
  exp_info.flags = O_RDWR;
  exp_info.priv = st->bo;
  exp_info.exp_name = "kpi-dmabuf";

  dmabuf = dma_buf_export(&exp_info);
  if (IS_ERR(dmabuf)) {
    long ret = PTR_ERR(dmabuf);

    kpi_bo_destroy(st->bo);
    st->bo = NULL;
    return ret;
  }

  fd = dma_buf_fd(dmabuf, O_CLOEXEC);
  if (fd < 0) {
    dma_buf_put(dmabuf);
    kpi_bo_destroy(st->bo);
    st->bo = NULL;
    return fd;
  }

  /* The fd owns the file reference created by dma_buf_getfile(); the device
   * state takes one of its own so the BO survives until this file is closed
   * even if userspace closes the dma-buf fd first. */
  get_file(dmabuf->file);
  st->dmabuf = dmabuf;
  return fd;
}

static long kpi_dmabuf_ioctl_sync_fd(struct kpi_dmabuf_state *st) {
  struct sync_file *sf;
  struct kpi_test_fence *f;
  int fd;

  if (st->fence)
    dma_fence_put(st->fence);
  st->fence = NULL;

  f = kpi_fence_create();
  if (!f)
    return -ENOMEM;

  sf = sync_file_create(&f->base);
  if (!sf) {
    dma_fence_put(&f->base);
    return -ENOMEM;
  }
  st->fence = &f->base; /* creation reference; sync_file holds its own */

  fd = get_unused_fd_flags(O_CLOEXEC);
  if (fd < 0) {
    fput(sf->file);
    return fd;
  }
  fd_install(fd, sf->file);
  return fd;
}

static long kpi_dmabuf_ioctl_wait(unsigned long arg) {
  struct dma_fence *fence = sync_file_get_fence((int)arg);
  long ret;

  if (!fence)
    return -EINVAL;

  /* 6.6 timeout is in jiffies; 10 s at HZ=1000. */
  ret = dma_fence_wait_timeout(fence, false, 10 * 1000);
  dma_fence_put(fence);
  return ret < 0 ? ret : 0;
}

static long kpi_dmabuf_dev_ioctl(struct file *file, unsigned int cmd,
                                 unsigned long arg) {
  struct kpi_dmabuf_state *st = file->private_data;

  if (!st)
    return -EINVAL;

  switch (cmd) {
  case KPI_DMABUF_IOC_ALLOC:
    return kpi_dmabuf_ioctl_alloc(st, arg);
  case KPI_DMABUF_IOC_CLOSE_COUNT:
    return (long)atomic_read(&kpi_dmabuf_close_count);
  case KPI_DMABUF_IOC_IMPORT: {
    struct dma_buf *dmabuf = dma_buf_get((int)arg);
    long ret = 0;

    if (IS_ERR(dmabuf))
      return PTR_ERR(dmabuf);
    if (dmabuf->size < PAGE_SIZE)
      ret = -EINVAL;
    dma_buf_put(dmabuf);
    return ret;
  }
  case KPI_DMABUF_IOC_SYNC_FD:
    return kpi_dmabuf_ioctl_sync_fd(st);
  case KPI_DMABUF_IOC_SIGNAL:
    if (!st->fence)
      return -EINVAL;
    dma_fence_signal(st->fence);
    return 0;
  case KPI_DMABUF_IOC_WAIT:
    return kpi_dmabuf_ioctl_wait(arg);
  case KPI_DMABUF_IOC_PMM_FREE:
    /* Includes PCP-cached pages: the plain buddy count visibly drops after
     * heavy churn even when nothing leaked. */
    return (long)asc_pmm_get_free_pages_total();
  case KPI_DMABUF_IOC_BO_PAGES:
    return st->bo ? (long)st->bo->n_pages : 0;
  case KPI_DMABUF_IOC_VERIFY:
    if (!st->bo)
      return -EINVAL;
    for (unsigned int i = 0; i < st->bo->n_pages; i++) {
      struct page *page = st->bo->pages[i];

      if (!page || compound_head(page) != page)
        return -EIO;
      if (page_ref_count(page) != 1)
        return -EIO;
      if (!asc_pmm_is_managed(page_to_phys(page)))
        return -EIO;
    }
    return (long)st->bo->n_pages;
  case KPI_DMABUF_IOC_UNMAP_MAPPING:
    if (!st->dmabuf)
      return -EINVAL;
    /* Invalidate every userspace mapping of this BO's address_space, the way
     * TTM/amdgpu do when a BO is evicted or freed while mapped. */
    unmap_mapping_range(st->dmabuf->file->f_mapping, 0, 0, 1);
    return 0;
  case KPI_DMABUF_IOC_PTE_PRESENT:
    /* Observation helper: the ioctl runs in the calling process, so the
     * active PML4 is the address space to inspect. */
    if (arg > 0x00007FFFFFFFFFFFUL)
      return -EINVAL;
    return asc_vmm_virt_to_phys(asc_vmm_get_active_pml4(), arg) != 0;
  default:
    return -ENOTTY;
  }
}

/* mmap() on the device node maps the current BO through the exported dma_buf,
 * which is the driver-side dma_buf_mmap() entry point (pgoff zero-based). */
static int kpi_dmabuf_dev_mmap(struct file *file, struct vm_area_struct *vma) {
  struct kpi_dmabuf_state *st = file->private_data;

  if (!st || !st->dmabuf)
    return -EINVAL;
  return dma_buf_mmap(st->dmabuf, vma, vma->vm_pgoff);
}

static int kpi_dmabuf_dev_release(struct inode *inode, struct file *file) {
  struct kpi_dmabuf_state *st = file->private_data;

  (void)inode;
  if (!st)
    return 0;

  if (st->fence)
    dma_fence_put(st->fence);
  if (st->dmabuf)
    dma_buf_put(st->dmabuf);
  kfree(st);
  file->private_data = NULL;
  return 0;
}

static const struct file_operations kpi_dmabuf_dev_fops = {
    .unlocked_ioctl = kpi_dmabuf_dev_ioctl,
    .mmap = kpi_dmabuf_dev_mmap,
    .release = kpi_dmabuf_dev_release,
};

/* ── devfs registration ─────────────────────────────────────────────────── */

static void *kpi_dmabuf_dev_open(void *metadata) {
  struct kpi_dmabuf_state *st;
  struct file *file;

  (void)metadata;
  st = kzalloc(sizeof(*st), GFP_KERNEL);
  if (!st)
    return NULL;

  file = anon_inode_getfile("kpi_dmabuf", &kpi_dmabuf_dev_fops, st, O_RDWR);
  if (!file || IS_ERR(file)) {
    kfree(st);
    return NULL;
  }

  /* The file bridge allocated the native node that forwards read/write/
   * ioctl/mmap/close into this file; that node becomes the descriptor. */
  asc_vfs_node_set_name(file->f_asc_node, "kpi_dmabuf");
  return file->f_asc_node;
}

static int __init kpi_dmabuf_testdev_init(void) {
  int ret = asc_vfs_register_devnode("kpi_dmabuf", kpi_dmabuf_dev_open);

  if (ret) {
    klogf("[WARN] LinuxKPI: /dev/kpi_dmabuf registration failed (%d)\n", ret);
    return ret;
  }
  klog_puts("[  OK  ] LinuxKPI: /dev/kpi_dmabuf ready\n");
  return 0;
}
module_init(kpi_dmabuf_testdev_init);
