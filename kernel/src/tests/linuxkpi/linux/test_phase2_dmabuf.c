/* Phase 2 — dma-buf / dma-fence / dma-resv / sync_file self-tests.
 *
 * Uses a tiny test exporter backed by freshly allocated pages.  The fd paths
 * (dma_buf_fd/dma_buf_get, sync_file_create + sync_file_get_fence) exercise
 * the LinuxKPI fd bridge in linuxkpi/src/file.c. */

#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/dma-resv.h>
#include <linux/sync_file.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>

#include <linuxkpi/log.h>

#define TEST_BO_PAGES 1024 /* 4 MB */

struct test_bo {
  struct page *pages[TEST_BO_PAGES];
  unsigned int n_pages;
  struct dma_buf *dmabuf;
};

/* ── test fence ─────────────────────────────────────────────────────────── */

struct test_fence {
  struct dma_fence base;
  spinlock_t lock;
};

static const char *test_fence_get_driver_name(struct dma_fence *f) {
  (void)f;
  return "kpi-test";
}
static const char *test_fence_get_timeline_name(struct dma_fence *f) {
  (void)f;
  return "kpi-timeline";
}

static const struct dma_fence_ops test_fence_ops = {
    .get_driver_name = test_fence_get_driver_name,
    .get_timeline_name = test_fence_get_timeline_name,
};

static struct test_fence *test_fence_create(u64 context, u64 seqno) {
  struct test_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);

  if (!f)
    return NULL;
  spin_lock_init(&f->lock);
  dma_fence_init(&f->base, &test_fence_ops, &f->lock, context, seqno);
  return f;
}

/* ── test exporter ──────────────────────────────────────────────────────── */

static int test_bo_attach(struct dma_buf *dmabuf,
                          struct dma_buf_attachment *attach) {
  (void)dmabuf;
  (void)attach;
  return 0;
}

static void test_bo_detach(struct dma_buf *dmabuf,
                           struct dma_buf_attachment *attach) {
  (void)dmabuf;
  (void)attach;
}

static struct sg_table *test_bo_map(struct dma_buf_attachment *attach,
                                    enum dma_data_direction dir) {
  struct test_bo *bo = attach->dmabuf->priv;
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

static void test_bo_unmap(struct dma_buf_attachment *attach,
                          struct sg_table *sgt, enum dma_data_direction dir) {
  (void)attach;
  (void)dir;
  sg_free_table(sgt);
  kfree(sgt);
}

static void test_bo_release(struct dma_buf *dmabuf) {
  struct test_bo *bo = dmabuf->priv;

  for (unsigned int i = 0; i < bo->n_pages; i++)
    __free_page(bo->pages[i]);
  kfree(bo);
}

static int test_bo_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma) {
  struct test_bo *bo = dmabuf->priv;
  unsigned long pfn = page_to_pfn(bo->pages[0]);

  /* VMA is contiguous here (single sg-backed range); map page by page. */
  for (unsigned long off = 0; off < vma->vm_end - vma->vm_start;
       off += PAGE_SIZE) {
    unsigned long idx = (vma->vm_pgoff << PAGE_SHIFT) + off;
    unsigned long page_idx = idx >> PAGE_SHIFT;

    if (page_idx >= bo->n_pages)
      return -EINVAL;
    if (remap_pfn_range(vma, vma->vm_start + off,
                        page_to_pfn(bo->pages[page_idx]), PAGE_SIZE,
                        vma->vm_page_prot))
      return -EAGAIN;
  }
  (void)pfn;
  vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
  return 0;
}

static int test_bo_begin_cpu_access(struct dma_buf *dmabuf,
                                    enum dma_data_direction dir) {
  (void)dmabuf;
  (void)dir;
  dma_mb();
  return 0;
}

static int test_bo_end_cpu_access(struct dma_buf *dmabuf,
                                  enum dma_data_direction dir) {
  (void)dmabuf;
  (void)dir;
  dma_mb();
  return 0;
}

static int test_bo_vmap(struct dma_buf *dmabuf, struct iosys_map *map) {
  struct test_bo *bo = dmabuf->priv;
  void *vaddr = vmap(bo->pages, bo->n_pages, VM_MAP, PAGE_KERNEL);

  if (!vaddr)
    return -ENOMEM;
  iosys_map_set_vaddr(map, vaddr);
  return 0;
}

static void test_bo_vunmap(struct dma_buf *dmabuf, struct iosys_map *map) {
  (void)dmabuf;
  vunmap(map->vaddr);
  iosys_map_clear(map);
}

static const struct dma_buf_ops test_bo_ops = {
    .attach = test_bo_attach,
    .detach = test_bo_detach,
    .map_dma_buf = test_bo_map,
    .unmap_dma_buf = test_bo_unmap,
    .release = test_bo_release,
    .mmap = test_bo_mmap,
    .begin_cpu_access = test_bo_begin_cpu_access,
    .end_cpu_access = test_bo_end_cpu_access,
    .vmap = test_bo_vmap,
    .vunmap = test_bo_vunmap,
};

static struct dma_buf *test_bo_export(struct test_bo **out) {
  struct test_bo *bo;
  struct dma_buf *dmabuf;
  DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

  bo = kzalloc(sizeof(*bo), GFP_KERNEL);
  if (!bo)
    return ERR_PTR(-ENOMEM);
  bo->n_pages = TEST_BO_PAGES;
  for (unsigned int i = 0; i < bo->n_pages; i++) {
    bo->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!bo->pages[i]) {
      for (unsigned int j = 0; j < i; j++)
        __free_page(bo->pages[j]);
      kfree(bo);
      return ERR_PTR(-ENOMEM);
    }
  }

  exp_info.ops = &test_bo_ops;
  exp_info.size = (size_t)bo->n_pages << PAGE_SHIFT;
  exp_info.flags = O_RDWR;
  exp_info.priv = bo;
  exp_info.exp_name = "kpi-test";

  dmabuf = dma_buf_export(&exp_info);
  if (IS_ERR(dmabuf)) {
    for (unsigned int i = 0; i < bo->n_pages; i++)
      __free_page(bo->pages[i]);
    kfree(bo);
    return dmabuf;
  }
  bo->dmabuf = dmabuf;
  *out = bo;
  return dmabuf;
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static bool test_dma_fence(void) {
  struct test_fence *f = test_fence_create(42, 1);
  struct test_fence *g = test_fence_create(42, 2);
  struct dma_fence **array_fences;
  struct dma_fence_array *array;
  long wait0 = -1, wait1 = -1;
  int unwrap_n = -1;
  bool ok = true;

  if (!f || !g)
    return false;

  if (dma_fence_is_signaled(&f->base))
    ok = false;

  wait0 = dma_fence_wait_timeout(&f->base, false, 10);
  if (wait0 != 0)
    ok = false; /* not signaled yet: 0 = timed out */

  dma_fence_signal(&f->base);
  if (!dma_fence_is_signaled(&f->base))
    ok = false;
  /* 6.6 returns the remaining timeout (> 0) on success. */
  wait1 = dma_fence_wait_timeout(&f->base, false, 10);
  if (wait1 <= 0)
    ok = false;

  /* dma_fence_array_create() takes ownership of the (heap) fences array and
   * of one reference per member. */
  array_fences = kmalloc_array(2, sizeof(*array_fences), GFP_KERNEL);
  if (!array_fences) {
    dma_fence_put(&f->base);
    dma_fence_put(&g->base);
    return false;
  }
  array_fences[0] = &f->base;
  array_fences[1] = &g->base;
  array = dma_fence_array_create(2, array_fences, 43, 1, false);
  if (!array) {
    dma_fence_put(&f->base);
    dma_fence_put(&g->base);
    kfree(array_fences);
    ok = false;
  } else {
    struct dma_fence_unwrap iter;
    struct dma_fence *unwrapped;
    int n = 0;

    if (dma_fence_is_signaled(&array->base))
      ok = false;
    dma_fence_signal(&g->base);
    if (!dma_fence_is_signaled(&array->base))
      ok = false;

    dma_fence_unwrap_for_each(unwrapped, &iter, &array->base)
      n++;
    unwrap_n = n;
    if (n != 2)
      ok = false;
    dma_fence_put(&array->base); /* releases the last f/g references */
  }

  (void)wait0;
  (void)wait1;
  (void)unwrap_n;
  return ok;
}

static bool test_dma_resv(void) {
  struct dma_resv resv;
  struct test_fence *f = test_fence_create(44, 1);
  struct dma_resv_iter cursor;
  struct dma_fence *fence;
  bool seen = false;
  bool ok = true;

  if (!f)
    return false;

  dma_resv_init(&resv);

  if (dma_resv_lock(&resv, NULL) != 0)
    return false;

  /* add_fence requires reserved slots (see dma_resv_add_fence docs). */
  dma_resv_reserve_fences(&resv, 2);

  dma_resv_add_fence(&resv, &f->base, DMA_RESV_USAGE_WRITE);
  dma_resv_add_fence(&resv, &f->base, DMA_RESV_USAGE_READ);

  dma_resv_for_each_fence(&cursor, &resv, DMA_RESV_USAGE_READ, fence) {
    if (fence == &f->base)
      seen = true;
  }
  if (!seen)
    ok = false;

  dma_resv_unlock(&resv);
  dma_resv_fini(&resv);
  dma_fence_put(&f->base);
  return ok;
}

static bool test_dma_buf_vmap(void) {
  struct test_bo *bo = NULL;
  struct dma_buf *dmabuf = test_bo_export(&bo);
  struct iosys_map map;
  int ret;
  bool ok = true;

  if (IS_ERR(dmabuf))
    return false;

  ret = dma_buf_vmap(dmabuf, &map);
  if (ret != 0) {
    klogf("[DBG] dma_buf_vmap ret=%d\n", ret);
    dma_buf_put(dmabuf);
    return false;
  }

  if (iosys_map_is_null(&map)) {
    klogf("[DBG] dma_buf_vmap returned null map\n");
    ok = false;
  } else {
    ((unsigned char *)map.vaddr)[0] = 0x5A;
    ((unsigned char *)map.vaddr)[PAGE_SIZE * 3 + 7] = 0xA5;
    if (*(unsigned char *)page_address(bo->pages[0]) != 0x5A) {
      klogf("[DBG] vmap: page0 readback mismatch\n");
      ok = false;
    }
    if (*((unsigned char *)page_address(bo->pages[3]) + 7) != 0xA5) {
      klogf("[DBG] vmap: page3 mismatch vaddr=%p p0=%llx p3=%llx "
            "via=%02x direct=%02x\n",
            map.vaddr, (unsigned long long)page_to_phys(bo->pages[0]),
            (unsigned long long)page_to_phys(bo->pages[3]),
            (unsigned char)((unsigned char *)map.vaddr)[PAGE_SIZE * 3 + 7],
            *((unsigned char *)page_address(bo->pages[3]) + 7));
      ok = false;
    }
  }

  dma_buf_vunmap(dmabuf, &map);
  dma_buf_put(dmabuf);
  return ok;
}

static bool test_dma_buf_fd(void) {
  struct test_bo *bo = NULL;
  struct dma_buf *dmabuf = test_bo_export(&bo);
  struct dma_buf *imported;
  struct dma_buf_attachment *attach;
  struct device dev = {0};
  int fd;
  bool ok = true;

  if (IS_ERR(dmabuf))
    return false;

  fd = dma_buf_fd(dmabuf, O_CLOEXEC);
  if (fd < 0) {
    dma_buf_put(dmabuf);
    return false;
  }

  imported = dma_buf_get(fd);
  if (IS_ERR(imported)) {
    ok = false;
  } else {
    if (imported != dmabuf)
      ok = false;

    attach = dma_buf_attach(imported, &dev);
    if (IS_ERR(attach)) {
      ok = false;
    } else {
      struct sg_table *sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
      if (IS_ERR(sgt)) {
        ok = false;
      } else {
        if (sgt->nents < 1)
          ok = false;
        dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
      }
      dma_buf_detach(imported, attach);
    }
    dma_buf_put(imported);
  }

  /* Closing the fd drops the last reference and runs dma_buf_release ->
   * ops->release (which frees bo's pages). */
  fput(dmabuf->file);
  return ok;
}

static bool test_sync_file(void) {
  struct test_fence *f = test_fence_create(45, 7);
  struct sync_file *sf;
  struct dma_fence *got;
  int fd;
  bool ok = true;

  if (!f)
    return false;

  sf = sync_file_create(&f->base);
  dma_fence_put(&f->base);
  if (!sf)
    return false;

  fd = get_unused_fd_flags(O_CLOEXEC);
  if (fd < 0)
    return false;
  fd_install(fd, sf->file);

  got = sync_file_get_fence(fd);
  if (!got) {
    ok = false;
  } else {
    if (got != sf->fence)
      ok = false;
    if (dma_fence_is_signaled(got))
      ok = false;
    dma_fence_put(got);
  }

  dma_fence_signal(sf->fence);
  if (!dma_fence_is_signaled(sf->fence))
    ok = false;

  /* fput runs sync_file_release, which drops the fence. */
  fput(sf->file);
  return ok;
}

/* ── aggregator ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase2_dmabuf(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"dma_fence signal/wait/array", test_dma_fence},
      {"dma_resv add/iterate", test_dma_resv},
      {"dma_buf export/vmap", test_dma_buf_vmap},
      {"dma_buf fd get/fput", test_dma_buf_fd},
      {"sync_file create/get_fence", test_sync_file},
  };

  klog_puts("[LINUXKPI] Phase 2 dma-buf self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s wrong result\n", tests[i].name);
  }
}
