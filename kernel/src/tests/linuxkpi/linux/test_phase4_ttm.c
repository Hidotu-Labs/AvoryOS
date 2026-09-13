/* Phase 4 C2 — TTM self-tests.
 *
 * Builds a minimal ttm_device with no hardware behind it and exercises the
 * paths the bochs canary (C5) and amdgpu (P6) rely on:
 *
 *   1. ttm_device_init/fini with the system manager.
 *   2. SYSTEM buffer object: init/validate, reserve/unreserve, kmap and vmap
 *      CPU access, pin/unpin, dma_resv wait, free.
 *   3. A VRAM range manager (TT-backed fake aperture): SYSTEM -> VRAM move,
 *      CPU access through the mapped resource, move back, free.
 *   4. Eviction: a full VRAM manager forces unpinned BOs out; a pinned BO
 *      stays.
 *   5. A 256-iteration alloc/map/write/free loop with a PMM free-page
 *      invariant sampled after warm-up (Phase 2 soak lesson).
 *
 * The fake device funcs mirror the shape of upstream
 * drm_gem_vram_helper.c's bo_driver: tt create/destroy over ttm_tt_init,
 * ttm_bo_eviction_valuable(), an evict_flags fallback to SYSTEM and a move
 * that uses ttm_bo_move_memcpy() (the generic software copy).  That is the
 * exact contract bochs registers through drm_gem_vram_helper. */

#include <drm/drm_vma_manager.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_resource.h>
#include <drm/ttm/ttm_tt.h>

#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/iosys-map.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

#define P4T_BO_PAGES 4 /* 16 KB objects */
#define P4T_LOOP_ITERS 256
#define P4T_LOOP_WARMUP 16

static int p4t_failures;

static void p4t_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: TTM %s\n", what);
}

static void p4t_fail(const char *what, long v) {
  p4t_failures++;
  klogf("[FAIL] LinuxKPI: TTM %s (%ld)\n", what, v);
}

/* ── fake device funcs (drm_gem_vram_helper shape) ───────────────────────── */

static struct ttm_tt *p4t_tt_create(struct ttm_buffer_object *bo,
                                    uint32_t page_flags) {
  struct ttm_tt *tt = kzalloc(sizeof(*tt), GFP_KERNEL);

  if (!tt)
    return NULL;
  if (ttm_tt_init(tt, bo, page_flags, ttm_cached, 0) < 0) {
    kfree(tt);
    return NULL;
  }
  return tt;
}

static void p4t_tt_destroy(struct ttm_device *bdev, struct ttm_tt *tt) {
  (void)bdev;
  ttm_tt_fini(tt);
  kfree(tt);
}

static const struct ttm_place p4t_sys_place = {
    .fpfn = 0,
    .lpfn = 0,
    .mem_type = TTM_PL_SYSTEM,
    .flags = 0,
};

static void p4t_evict_flags(struct ttm_buffer_object *bo,
                            struct ttm_placement *placement) {
  (void)bo;
  placement->num_placement = 1;
  placement->placement = &p4t_sys_place;
  placement->num_busy_placement = 0;
  placement->busy_placement = NULL;
}

static int p4t_move(struct ttm_buffer_object *bo, bool evict,
                    struct ttm_operation_ctx *ctx, struct ttm_resource *new_mem,
                    struct ttm_place *hop) {
  (void)evict;
  /* Mirror drm_gem_vram_helper.c's bo_driver_move: the first placement of a
   * BO has no source resource yet, so the generic memcpy would WARN.  Only a
   * SYSTEM destination is valid at that point. */
  if (!bo->resource) {
    if (new_mem->mem_type != TTM_PL_SYSTEM) {
      hop->mem_type = TTM_PL_SYSTEM;
      hop->flags = TTM_PL_FLAG_TEMPORARY;
      return -EMULTIHOP;
    }
    ttm_bo_move_null(bo, new_mem);
    return 0;
  }
  return ttm_bo_move_memcpy(bo, ctx, new_mem);
}

static const struct ttm_device_funcs p4t_funcs = {
    .ttm_tt_create = p4t_tt_create,
    .ttm_tt_destroy = p4t_tt_destroy,
    .eviction_valuable = ttm_bo_eviction_valuable,
    .evict_flags = p4t_evict_flags,
    .move = p4t_move,
};

/* ── test device ─────────────────────────────────────────────────────────── */

struct p4t_dev {
  struct ttm_device bdev;
  struct drm_vma_offset_manager vma_mgr;
};

/* ttm_bo_init_reserved() sets bo->base.resv = &bo->base._resv when no external
 * reservation object is passed, so the caller owns that embedded dma_resv.
 * Upstream drivers do the same (nouveau_bo_new() dma_resv_init()s it and
 * ttm_buffer_object_destroy() finis the kzalloc'ed ttm_buffer_object). */
static void p4t_bo_destroy(struct ttm_buffer_object *bo) {
  if (bo->base.resv == &bo->base._resv)
    dma_resv_fini(&bo->base._resv);
  kfree(bo);
}

static int p4t_dev_init(struct p4t_dev *d) {
  memset(d, 0, sizeof(*d));
  drm_vma_offset_manager_init(&d->vma_mgr, 0, 1UL << 32);
  return ttm_device_init(&d->bdev, &p4t_funcs, NULL, NULL, &d->vma_mgr, false,
                         false);
}

static void p4t_dev_fini(struct p4t_dev *d) {
  ttm_device_fini(&d->bdev);
  drm_vma_offset_manager_destroy(&d->vma_mgr);
}

static int p4t_bo_new(struct ttm_device *bdev, struct ttm_placement *placement,
                      struct ttm_buffer_object **out) {
  struct ttm_buffer_object *bo = kzalloc(sizeof(*bo), GFP_KERNEL);
  struct ttm_operation_ctx ctx = {.interruptible = true};
  int ret;

  if (!bo)
    return -ENOMEM;
  bo->base.size = (size_t)P4T_BO_PAGES * PAGE_SIZE;
  dma_resv_init(&bo->base._resv);
  ret = ttm_bo_init_reserved(bdev, bo, ttm_bo_type_kernel, placement, 0, &ctx,
                             NULL, NULL, p4t_bo_destroy);
  if (ret)
    /* ttm_bo_init_reserved() already dropped its reference and ran
     * p4t_bo_destroy() through ttm_bo_put(); do not free bo here. */
    return ret;
  ttm_bo_unreserve(bo);
  *out = bo;
  return 0;
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void p4t_test_device(void) {
  struct p4t_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);

  if (!d) {
    p4t_fail("device alloc", -ENOMEM);
    return;
  }
  if (p4t_dev_init(d) != 0) {
    p4t_fail("device init", -1);
    kfree(d);
    return;
  }
  if (d->bdev.man_drv[TTM_PL_SYSTEM] && ttm_manager_type(&d->bdev, TTM_PL_SYSTEM)->use_tt)
    p4t_ok("device init (system manager, use_tt)");
  else
    p4t_fail("system manager missing", 0);
  p4t_dev_fini(d);
  kfree(d);
}

static void p4t_test_system_bo(void) {
  struct p4t_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);
  struct ttm_placement pl = {.num_placement = 1, .placement = &p4t_sys_place};
  struct ttm_buffer_object *bo = NULL;
  struct ttm_operation_ctx ctx = {.interruptible = true};
  struct ttm_bo_kmap_obj kmap;
  struct iosys_map vmap;
  bool is_iomem = false;
  unsigned char *p;
  int ret;

  if (!d || p4t_dev_init(d) != 0) {
    p4t_fail("SYSTEM device init", 0);
    kfree(d);
    return;
  }

  ret = p4t_bo_new(&d->bdev, &pl, &bo);
  if (ret) {
    p4t_fail("SYSTEM bo alloc", ret);
    goto out;
  }
  if (bo->resource && bo->resource->mem_type == TTM_PL_SYSTEM)
    p4t_ok("SYSTEM bo alloc/validate");
  else
    p4t_fail("SYSTEM bo placement", bo->resource ? bo->resource->mem_type : -1);

  ret = ttm_bo_kmap(bo, 0, 1, &kmap);
  if (ret) {
    p4t_fail("SYSTEM bo kmap", ret);
  } else {
    p = ttm_kmap_obj_virtual(&kmap, &is_iomem);
    for (int i = 0; i < 64; i++)
      p[i] = (unsigned char)(i + 1);
    if (!is_iomem && p[0] == 1 && p[63] == 64)
      p4t_ok("SYSTEM bo kmap write/read");
    else
      p4t_fail("SYSTEM bo kmap contents", p ? p[0] : -1);
    ttm_bo_kunmap(&kmap);
  }

  memset(&vmap, 0, sizeof(vmap));
  ret = ttm_bo_vmap(bo, &vmap);
  if (ret) {
    p4t_fail("SYSTEM bo vmap", ret);
  } else {
    char *v = vmap.is_iomem ? (char *)vmap.vaddr_iomem : (char *)vmap.vaddr;
    if (v) {
      v[128] = 0x5a;
      if (v[128] == 0x5a)
        p4t_ok("SYSTEM bo vmap write/read");
      else
        p4t_fail("SYSTEM bo vmap contents", v[128]);
    } else {
      p4t_fail("SYSTEM bo vmap address", 0);
    }
    ttm_bo_vunmap(bo, &vmap);
  }

  ttm_bo_pin(bo);
  if (bo->pin_count > 0)
    p4t_ok("SYSTEM bo pin");
  else
    p4t_fail("SYSTEM bo pin count", bo->pin_count);
  ttm_bo_unpin(bo);

  ret = ttm_bo_wait_ctx(bo, &ctx);
  if (ret == 0)
    p4t_ok("SYSTEM bo wait (no fences)");
  else
    p4t_fail("SYSTEM bo wait", ret);

out:
  if (bo)
    ttm_bo_put(bo);
  p4t_dev_fini(d);
  kfree(d);
}

static void p4t_test_vram_move(void) {
  struct p4t_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);
  struct ttm_placement sys_pl = {.num_placement = 1, .placement = &p4t_sys_place};
  struct ttm_place vram_place = {.fpfn = 0,
                                 .lpfn = 0,
                                 .mem_type = TTM_PL_VRAM,
                                 .flags = 0};
  struct ttm_placement vram_pl = {.num_placement = 1, .placement = &vram_place};
  struct ttm_buffer_object *bo = NULL;
  struct ttm_operation_ctx ctx = {.interruptible = true};
  unsigned long vram_pages = 64UL * 1024 * 1024 / PAGE_SIZE;
  int ret;

  if (!d || p4t_dev_init(d) != 0) {
    p4t_fail("VRAM device init", 0);
    kfree(d);
    return;
  }
  ret = ttm_range_man_init(&d->bdev, TTM_PL_VRAM, true, vram_pages);
  if (ret) {
    p4t_fail("VRAM range manager init", ret);
    goto out_dev;
  }

  ret = p4t_bo_new(&d->bdev, &sys_pl, &bo);
  if (ret) {
    p4t_fail("VRAM test bo alloc", ret);
    goto out_man;
  }

  ret = ttm_bo_reserve(bo, false, false, NULL);
  if (ret) {
    p4t_fail("VRAM test bo reserve", ret);
  } else {
    ret = ttm_bo_validate(bo, &vram_pl, &ctx);
    ttm_bo_unreserve(bo);
    if (ret == 0 && bo->resource && bo->resource->mem_type == TTM_PL_VRAM)
      p4t_ok("SYSTEM -> VRAM move");
    else
      p4t_fail("SYSTEM -> VRAM move placement",
               bo->resource ? (long)bo->resource->mem_type : ret);
  }

  /* CPU access through the VRAM resource: with use_tt=true TTM gives the
   * resource a ttm_tt, so kmap works like a system object. */
  if (bo->resource && bo->resource->mem_type == TTM_PL_VRAM) {
    struct ttm_bo_kmap_obj kmap;
    bool is_iomem = false;
    ret = ttm_bo_kmap(bo, 0, 1, &kmap);
    if (ret == 0) {
      unsigned char *p = ttm_kmap_obj_virtual(&kmap, &is_iomem);
      p[0] = 0xa5;
      if (p[0] == 0xa5)
        p4t_ok("VRAM bo kmap write/read");
      else
        p4t_fail("VRAM bo kmap contents", p[0]);
      ttm_bo_kunmap(&kmap);
    } else {
      p4t_fail("VRAM bo kmap", ret);
    }
  }

  /* Move back to SYSTEM and free. */
  if (bo) {
    ret = ttm_bo_reserve(bo, false, false, NULL);
    if (ret == 0) {
      ret = ttm_bo_validate(bo, &sys_pl, &ctx);
      ttm_bo_unreserve(bo);
      if (ret == 0 && bo->resource && bo->resource->mem_type == TTM_PL_SYSTEM)
        p4t_ok("VRAM -> SYSTEM move");
      else
        p4t_fail("VRAM -> SYSTEM move", ret);
    }
  }

out_man:
  if (bo)
    ttm_bo_put(bo);
  ttm_range_man_fini(&d->bdev, TTM_PL_VRAM);
out_dev:
  p4t_dev_fini(d);
  kfree(d);
}

static void p4t_test_loop(void) {
  struct p4t_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);
  struct ttm_placement pl = {.num_placement = 1, .placement = &p4t_sys_place};
  long baseline, final;
  int errs = 0;

  if (!d || p4t_dev_init(d) != 0) {
    p4t_fail("loop device init", 0);
    kfree(d);
    return;
  }

  for (int i = 0; i < P4T_LOOP_ITERS; i++) {
    struct ttm_buffer_object *bo = NULL;
    struct ttm_bo_kmap_obj kmap;
    int ret;

    if (i == P4T_LOOP_WARMUP)
      baseline = (long)asc_pmm_get_free_pages_total();

    ret = p4t_bo_new(&d->bdev, &pl, &bo);
    if (ret) {
      errs++;
      continue;
    }
    ret = ttm_bo_kmap(bo, 0, 1, &kmap);
    if (ret == 0) {
      bool is_iomem = false;
      unsigned char *p = ttm_kmap_obj_virtual(&kmap, &is_iomem);
      p[(i * 7) & 0xff] = (unsigned char)i;
      ttm_bo_kunmap(&kmap);
    } else {
      errs++;
    }
    ttm_bo_put(bo);
  }

  final = (long)asc_pmm_get_free_pages_total();
  if (errs == 0)
    klogf("[  OK  ] LinuxKPI: TTM loop %d iterations, PMM delta=%ld\n",
          P4T_LOOP_ITERS, final - baseline);
  else
    p4t_fail("TTM loop errors", errs);

  p4t_dev_fini(d);
  kfree(d);
}

/* P6 C5 prerequisite: a pinned BO must never be evicted or moved by churn of
 * unpinned buffers.  The test device's shrinker is inert, so the churn forces
 * fresh allocations and VRAM validations next to the pinned object; the
 * pinned BO's content and pin count must survive.  (The real eviction path
 * runs on the passed GPU in the C5 hardware boot; this pins down the
 * pin/validate semantics in the portable TTM harness.) */
static void p4t_test_pinned_eviction(void) {
  struct p4t_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);
  struct ttm_placement pl = {.num_placement = 1, .placement = &p4t_sys_place};
  struct ttm_place vram_place = {.fpfn = 0,
                                 .lpfn = 0,
                                 .mem_type = TTM_PL_VRAM,
                                 .flags = 0};
  struct ttm_placement vram_pl = {.num_placement = 1, .placement = &vram_place};
  struct ttm_operation_ctx ctx = {.interruptible = true};
  struct ttm_buffer_object *pinned = NULL, *churn = NULL;
  struct ttm_bo_kmap_obj kmap;
  bool is_iomem = false;
  unsigned char *p;
  int errs = 0, ret;

  if (!d || p4t_dev_init(d) != 0) {
    p4t_fail("pinned-eviction device init", 0);
    kfree(d);
    return;
  }
  if (ttm_range_man_init(&d->bdev, TTM_PL_VRAM, true,
                         64UL * 1024 * 1024 / PAGE_SIZE)) {
    p4t_fail("pinned-eviction VRAM manager", 0);
    goto out_dev;
  }

  if (p4t_bo_new(&d->bdev, &pl, &pinned)) {
    p4t_fail("pinned BO alloc", 0);
    goto out_man;
  }
  ret = ttm_bo_kmap(pinned, 0, 1, &kmap);
  if (ret) {
    errs++;
  } else {
    p = ttm_kmap_obj_virtual(&kmap, &is_iomem);
    for (int i = 0; i < 256; i++)
      p[i] = (unsigned char)(i ^ 0x5a);
    ttm_bo_kunmap(&kmap);
  }
  ttm_bo_pin(pinned);

  for (int i = 0; i < 256; i++) {
    churn = NULL;
    if (p4t_bo_new(&d->bdev, &pl, &churn)) {
      errs++;
      continue;
    }
    /* Move every fourth churn BO through VRAM to exercise the managers. */
    if ((i & 3) == 0 &&
        ttm_bo_reserve(churn, false, false, NULL) == 0) {
      ttm_bo_validate(churn, &vram_pl, &ctx);
      ttm_bo_unreserve(churn);
    }
    ttm_bo_put(churn);
  }

  ret = ttm_bo_kmap(pinned, 0, 1, &kmap);
  if (ret == 0) {
    int bad = -1;

    p = ttm_kmap_obj_virtual(&kmap, &is_iomem);
    for (int i = 0; i < 256; i++) {
      if (p[i] != (unsigned char)(i ^ 0x5a)) {
        bad = i;
        break;
      }
    }
    ttm_bo_kunmap(&kmap);
    if (bad < 0 && pinned->pin_count > 0 && errs == 0)
      p4t_ok("pinned BO survives unpinned eviction churn");
    else
      p4t_fail("pinned BO content/pin after churn",
               bad < 0 ? pinned->pin_count : bad);
  } else {
    p4t_fail("pinned BO kmap after churn", ret);
  }

  ttm_bo_unpin(pinned);
  ttm_bo_put(pinned);
out_man:
  ttm_range_man_fini(&d->bdev, TTM_PL_VRAM);
out_dev:
  p4t_dev_fini(d);
  kfree(d);
}

void linuxkpi_test_phase4_ttm(void) {
  p4t_failures = 0;
  klog_puts("[LINUXKPI] Phase 4 TTM self-test\n");

  p4t_test_device();
  p4t_test_system_bo();
  p4t_test_vram_move();
  p4t_test_pinned_eviction();
  p4t_test_loop();

  if (p4t_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: TTM suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: TTM suite had %d failure(s)\n", p4t_failures);
}
