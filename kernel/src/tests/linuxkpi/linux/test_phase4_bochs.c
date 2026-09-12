/* Phase 4 C5 — bochs TTM canary kernel test.
 *
 * bochs is the first real TTM-backed driver under LinuxKPI.  The driver binds
 * during initcalls; this suite runs after them and exercises the live card
 * through /dev/dri/cardN:
 *
 *   1. discover the bochs card by DRM_IOCTL_VERSION name (never by minor
 *      number), then discover CRTC/connector/plane and property IDs.
 *   2. CREATE_DUMB (64x64 XRGB8888) -> vmap write a pixel pattern -> ADDFB2 ->
 *      MODE_CREATEPROPBLOB -> MODE_ATOMIC enable.
 *   3. after the synchronous commit, read the VBE XRES/YRES/ENABLE registers
 *      through the driver's BAR2 MMIO mapping and compare with the mode.
 *   4. look the GEM object up from the DRM file, take its VRAM offset
 *      (drm_gem_vram_offset() requires the plane commit's pin), read the
 *      pattern back through the BAR0 framebuffer mapping and compare.  This
 *      proves the SYSTEM -> VRAM move preserved contents.
 *   5. disable commit, then a warm-up + two measured 128-iteration VRAM BO
 *      create/vmap/pin/unpin/free loop with PMM and TTM VRAM-manager usage
 *      invariants (the second pass must be exactly stable).
 *
 * Like the Phase 3 tests it maps a scratch region into the user range of the
 * active PML4, because DRM ioctls copy arguments with copy_from_user(). */

#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_mode.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_resource.h>
#include <drm/ttm/ttm_tt.h>

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>
#include <linuxkpi/native_vfs.h>

/* Scratch user VA.  Layout:
 *   0x000  ioctl argument struct (<= 128 bytes)
 *   0x100  u32 object id arrays
 *   0x1c0  u32 property id array
 *   0x280  u64 property value array
 *   0x800  DRM version name buffer
 *   0x900  mode blob source (drm_mode_modeinfo)
 *   0x1d00 atomic request arrays
 */
#define P4B_VA 0x0000000020000000ULL

#define P4B_OFF_STRUCT 0x000
#define P4B_OFF_IDS 0x100
#define P4B_OFF_PROPS 0x1c0
#define P4B_OFF_VALS 0x280
#define P4B_OFF_NAME 0x800
#define P4B_OFF_MODE 0x900
#define P4B_OFF_ATOMIC 0x1d00

#define P4B_MODE_CAP 48
#define P4B_PROP_CAP 48

#define P4B_STRUCT(s) ((void *)((char *)(s)->kva + P4B_OFF_STRUCT))
#define P4B_IDS(s) ((uint32_t *)((char *)(s)->kva + P4B_OFF_IDS))
#define P4B_MODES(s) \
  ((struct drm_mode_modeinfo *)((char *)(s)->kva + P4B_OFF_MODE))

/* bochs VBE Dispi registers live in BAR2 MMIO at 0x500 + (index << 1); the
 * index/data port pair (0x1ce/0x1cf) is only the non-MMIO path.  These
 * constants are private to bochs.c upstream. */
#define P4B_DISPI_BASE 0x500
#define P4B_VBE_INDEX_XRES 0x1
#define P4B_VBE_INDEX_YRES 0x2
#define P4B_VBE_INDEX_ENABLE 0x4
#define P4B_VBE_ENABLED 0x01

struct p4b_scratch {
  void *phys;
  void *kva;
};

struct p4b_atomic_buf {
  uint32_t objs[4];
  uint32_t counts[4];
  uint32_t props[24];
  uint64_t values[24];
};

struct p4b_prop_set {
  uint32_t ids[P4B_PROP_CAP];
  uint64_t values[P4B_PROP_CAP];
  uint32_t count;
};

static int p4b_failures;

static void p4b_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: bochs %s\n", what);
}

static void p4b_fail(const char *what, long v) {
  p4b_failures++;
  klogf("[FAIL] LinuxKPI: bochs %s (%ld)\n", what, v);
}

/* ── scratch / ioctl plumbing ───────────────────────────────────────────── */

static int p4b_scratch_alloc(struct p4b_scratch *s) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  s->phys = asc_pmm_alloc_pages(2);
  if (!s->phys)
    return -1;
  s->kva = (void *)((uint64_t)s->phys + asc_pmm_get_hhdm_offset());
  memset(s->kva, 0, 8192);

  if (!asc_vmm_map_page(pml4, P4B_VA, (uint64_t)s->phys,
                        ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_USER)) {
    asc_pmm_free_pages(s->phys, 2);
    return -1;
  }
  if (!asc_vmm_map_page(pml4, P4B_VA + 4096, (uint64_t)s->phys + 4096,
                        ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_USER)) {
    asc_vmm_unmap_page(pml4, P4B_VA);
    asc_invlpg(P4B_VA);
    asc_pmm_free_pages(s->phys, 2);
    return -1;
  }
  return 0;
}

static void p4b_scratch_free(struct p4b_scratch *s) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  asc_vmm_unmap_page(pml4, P4B_VA);
  asc_vmm_unmap_page(pml4, P4B_VA + 4096);
  asc_invlpg(P4B_VA);
  asc_invlpg(P4B_VA + 4096);
  asc_pmm_free_pages(s->phys, 2);
}

static int p4b_ioctl(void *node, unsigned int request) {
  return asc_vfs_kernel_ioctl(node, request, P4B_VA);
}

/* Open card1..card6, read DRM_IOCTL_VERSION.name and return the bochs one. */
static void *p4b_find_bochs(int *minor_out) {
  for (int minor = 1; minor <= 6; minor++) {
    char path[32];
    char name[32];
    struct p4b_scratch s;
    struct drm_version *ver;
    void *node;

    snprintf(path, sizeof(path), "/dev/dri/card%d", minor);
    node = asc_vfs_kernel_open(path);
    if (!node)
      continue;
    if (p4b_scratch_alloc(&s)) {
      asc_vfs_kernel_close(node);
      continue;
    }

    ver = P4B_STRUCT(&s);
    memset(ver, 0, sizeof(*ver));
    ver->name = (char *)(uintptr_t)(P4B_VA + P4B_OFF_NAME);
    ver->name_len = sizeof(name);
    if (p4b_ioctl(node, DRM_IOCTL_VERSION) == 0) {
      memcpy(name, (char *)s.kva + P4B_OFF_NAME, sizeof(name));
      name[sizeof(name) - 1] = '\0';
      if (strncmp(name, "bochs", 5) == 0) {
        if (minor_out)
          *minor_out = minor;
        p4b_scratch_free(&s);
        return node;
      }
    }
    p4b_scratch_free(&s);
    asc_vfs_kernel_close(node);
  }
  return NULL;
}

/* ── object / property discovery ────────────────────────────────────────── */

static int p4b_set_atomic_cap(void *node, struct p4b_scratch *s) {
  struct drm_set_client_cap *cap = P4B_STRUCT(s);

  memset(cap, 0, sizeof(*cap));
  cap->capability = DRM_CLIENT_CAP_ATOMIC;
  cap->value = 1;
  return p4b_ioctl(node, DRM_IOCTL_SET_CLIENT_CAP);
}

static int p4b_get_resources(void *node, struct p4b_scratch *s,
                             uint32_t *crtc_id, uint32_t *connector_id) {
  struct drm_mode_card_res *res = P4B_STRUCT(s);
  uint32_t *ids = P4B_IDS(s);
  uint32_t ncrtc, nconn;
  int ret;

  memset(res, 0, sizeof(*res));
  ret = p4b_ioctl(node, DRM_IOCTL_MODE_GETRESOURCES);
  if (ret || !res->count_crtcs || !res->count_connectors)
    return ret ? ret : -1;
  ncrtc = res->count_crtcs;
  nconn = res->count_connectors;

  memset(res, 0, sizeof(*res));
  res->crtc_id_ptr = P4B_VA + P4B_OFF_IDS;
  res->connector_id_ptr = P4B_VA + P4B_OFF_IDS + 0x40;
  res->count_crtcs = ncrtc > 8 ? 8 : ncrtc;
  res->count_connectors = nconn > 8 ? 8 : nconn;
  ret = p4b_ioctl(node, DRM_IOCTL_MODE_GETRESOURCES);
  if (ret)
    return ret;

  *crtc_id = ids[0];
  *connector_id = ids[16];
  return (*crtc_id && *connector_id) ? 0 : -1;
}

static int p4b_get_planes(void *node, struct p4b_scratch *s,
                          uint32_t *plane_ids, uint32_t *count) {
  struct drm_mode_get_plane_res *pr = P4B_STRUCT(s);
  uint32_t *ids = P4B_IDS(s);
  int ret;

  memset(pr, 0, sizeof(*pr));
  pr->plane_id_ptr = P4B_VA + P4B_OFF_IDS;
  pr->count_planes = 8;
  ret = p4b_ioctl(node, DRM_IOCTL_MODE_GETPLANERESOURCES);
  if (ret)
    return ret;

  *count = pr->count_planes > 8 ? 8 : pr->count_planes;
  for (uint32_t i = 0; i < *count; i++)
    plane_ids[i] = ids[i];
  return *count ? 0 : -1;
}

static int p4b_connector_modes(void *node, struct p4b_scratch *s,
                               uint32_t connector_id,
                               struct drm_mode_modeinfo *mode,
                               uint32_t *mode_count) {
  struct drm_mode_get_connector *con = P4B_STRUCT(s);
  uint32_t nmodes;
  int ret;

  memset(con, 0, sizeof(*con));
  con->connector_id = connector_id;
  ret = p4b_ioctl(node, DRM_IOCTL_MODE_GETCONNECTOR);
  if (ret || !con->count_modes)
    return ret ? ret : -1;
  nmodes = con->count_modes;

  memset(con, 0, sizeof(*con));
  con->connector_id = connector_id;
  con->count_modes = nmodes > P4B_MODE_CAP ? P4B_MODE_CAP : nmodes;
  con->modes_ptr = P4B_VA + P4B_OFF_MODE;
  ret = p4b_ioctl(node, DRM_IOCTL_MODE_GETCONNECTOR);
  if (ret)
    return ret;

  memcpy(mode, P4B_MODES(s), sizeof(*mode));
  *mode_count = nmodes;
  return 0;
}

static int p4b_obj_props(void *node, struct p4b_scratch *s, uint32_t obj_id,
                         uint32_t obj_type, struct p4b_prop_set *out) {
  struct drm_mode_obj_get_properties *p = P4B_STRUCT(s);
  uint32_t total;
  int ret;

  memset(p, 0, sizeof(*p));
  p->obj_id = obj_id;
  p->obj_type = obj_type;
  ret = p4b_ioctl(node, DRM_IOCTL_MODE_OBJ_GETPROPERTIES);
  if (ret)
    return ret;
  total = p->count_props;
  if (!total)
    return -1;

  memset(p, 0, sizeof(*p));
  p->obj_id = obj_id;
  p->obj_type = obj_type;
  p->count_props = total > P4B_PROP_CAP ? P4B_PROP_CAP : total;
  p->props_ptr = P4B_VA + P4B_OFF_PROPS;
  p->prop_values_ptr = P4B_VA + P4B_OFF_VALS;
  ret = p4b_ioctl(node, DRM_IOCTL_MODE_OBJ_GETPROPERTIES);
  if (ret)
    return ret;

  out->count = total > P4B_PROP_CAP ? P4B_PROP_CAP : total;
  memcpy(out->ids, (char *)s->kva + P4B_OFF_PROPS, out->count * 4);
  memcpy(out->values, (char *)s->kva + P4B_OFF_VALS, out->count * 8);
  return 0;
}

static int p4b_prop_find(void *node, struct p4b_scratch *s,
                         const struct p4b_prop_set *set, const char *name,
                         uint32_t *id_out, uint64_t *value_out) {
  struct drm_mode_get_property *p = P4B_STRUCT(s);

  for (uint32_t i = 0; i < set->count; i++) {
    memset(p, 0, sizeof(*p));
    p->prop_id = set->ids[i];
    if (p4b_ioctl(node, DRM_IOCTL_MODE_GETPROPERTY))
      return -1;
    if (strncmp(p->name, name, 32) == 0) {
      if (id_out)
        *id_out = set->ids[i];
      if (value_out)
        *value_out = set->values[i];
      return 0;
    }
  }
  return -1;
}

/* ── dumb buffers / atomic commits ──────────────────────────────────────── */

static int p4b_dumb_create(void *node, struct p4b_scratch *s, uint32_t width,
                           uint32_t height, uint32_t *handle, uint32_t *pitch,
                           uint32_t *size) {
  struct drm_mode_create_dumb *cd = P4B_STRUCT(s);
  int ret;

  memset(cd, 0, sizeof(*cd));
  cd->width = width;
  cd->height = height;
  cd->bpp = 32;
  ret = p4b_ioctl(node, DRM_IOCTL_MODE_CREATE_DUMB);
  if (ret || !cd->handle || !cd->pitch || !cd->size)
    return ret ? ret : -1;
  *handle = cd->handle;
  *pitch = cd->pitch;
  *size = cd->size;
  return 0;
}

static int p4b_gem_close(void *node, struct p4b_scratch *s, uint32_t handle) {
  struct drm_gem_close *gc = P4B_STRUCT(s);

  memset(gc, 0, sizeof(*gc));
  gc->handle = handle;
  return p4b_ioctl(node, DRM_IOCTL_GEM_CLOSE);
}

static int p4b_atomic_commit(void *node, struct p4b_scratch *s,
                             struct p4b_atomic_buf *ab, uint32_t n_objs,
                             uint32_t n_props, uint32_t flags) {
  struct drm_mode_atomic *at = P4B_STRUCT(s);

  memset(at, 0, sizeof(*at));
  at->flags = flags;
  at->count_objs = n_objs;
  at->objs_ptr = P4B_VA + P4B_OFF_ATOMIC +
                 __builtin_offsetof(struct p4b_atomic_buf, objs);
  at->count_props_ptr = P4B_VA + P4B_OFF_ATOMIC +
                        __builtin_offsetof(struct p4b_atomic_buf, counts);
  at->props_ptr = P4B_VA + P4B_OFF_ATOMIC +
                  __builtin_offsetof(struct p4b_atomic_buf, props);
  at->prop_values_ptr = P4B_VA + P4B_OFF_ATOMIC +
                        __builtin_offsetof(struct p4b_atomic_buf, values);
  (void)n_props;
  return p4b_ioctl(node, DRM_IOCTL_MODE_ATOMIC);
}

/* ── bochs hardware register access ─────────────────────────────────────── */

static uint16_t p4b_dispi_read(void __iomem *mmio, uint16_t reg) {
  return readw(mmio + P4B_DISPI_BASE + (reg << 1));
}

/* ── GEM loop ───────────────────────────────────────────────────────────── */

#define P4B_LOOP_WARMUP 16
#define P4B_LOOP_ITERS 128

/* One measured pass: create/vmap/pin(V RAM)/unpin/free cycles.  Samples the
 * PMM free-page count and the TTM VRAM manager usage before and after. */
static int p4b_gem_pass(void *node, struct p4b_scratch *s,
                        struct drm_file *df,
                        struct ttm_resource_manager *vram_man, int iters,
                        unsigned long *pmm_imm, unsigned long *pmm_after,
                        unsigned long long *vram_imm,
                        unsigned long long *vram_after) {
  unsigned long pmm_before = asc_pmm_get_free_pages_total();
  uint64_t vram_before = ttm_resource_manager_usage(vram_man);

  for (int i = 0; i < iters; i++) {
    struct drm_gem_object *gem;
    struct drm_gem_vram_object *gbo;
    struct iosys_map map;
    uint32_t handle, pitch, size;
    s64 offset;

    if (p4b_dumb_create(node, s, 64, 64, &handle, &pitch, &size))
      return -1;
    gem = drm_gem_object_lookup(df, handle);
    if (!gem) {
      p4b_gem_close(node, s, handle);
      return -1;
    }
    gbo = drm_gem_vram_of_gem(gem);

    if (drm_gem_vram_vmap(gbo, &map)) {
      drm_gem_object_put(gem);
      p4b_gem_close(node, s, handle);
      return -1;
    }
    *(uint32_t *)map.vaddr = 0x5a5a0000u | (uint32_t)i;
    drm_gem_vram_vunmap(gbo, &map);

    if (drm_gem_vram_pin(gbo, DRM_GEM_VRAM_PL_FLAG_VRAM) ||
        drm_gem_vram_offset(gbo) < 0) {
      drm_gem_object_put(gem);
      p4b_gem_close(node, s, handle);
      return -1;
    }
    offset = drm_gem_vram_offset(gbo);
    if (offset < 0) {
      drm_gem_vram_unpin(gbo);
      drm_gem_object_put(gem);
      p4b_gem_close(node, s, handle);
      return -1;
    }
    drm_gem_vram_unpin(gbo);
    drm_gem_object_put(gem);
    if (p4b_gem_close(node, s, handle))
      return -1;
  }

  *pmm_imm = asc_pmm_get_free_pages_total();
  *vram_imm = ttm_resource_manager_usage(vram_man);
  msleep(200);
  *pmm_after = asc_pmm_get_free_pages_total();
  *vram_after = ttm_resource_manager_usage(vram_man);

  /* Deltas are reported as consumed pages/bytes (0 == back to baseline). */
  *pmm_imm = pmm_before >= *pmm_imm ? pmm_before - *pmm_imm : 0;
  *pmm_after = pmm_before >= *pmm_after ? pmm_before - *pmm_after : 0;
  *vram_imm = vram_before >= *vram_imm ? vram_before - *vram_imm : 0;
  *vram_after = vram_before >= *vram_after ? vram_before - *vram_after : 0;
  return 0;
}

static int p4b_gem_loop(void *node, struct p4b_scratch *s, struct drm_file *df,
                        struct ttm_resource_manager *vram_man) {
  unsigned long pmm1_imm, pmm1_after, pmm2_imm, pmm2_after;
  unsigned long long vram1_imm, vram1_after, vram2_imm, vram2_after;
  int ret;

  for (int i = 0; i < P4B_LOOP_WARMUP; i++) {
    struct drm_gem_object *gem;
    struct drm_gem_vram_object *gbo;
    uint32_t handle, pitch, size;

    if (p4b_dumb_create(node, s, 64, 64, &handle, &pitch, &size))
      return -1;
    gem = drm_gem_object_lookup(df, handle);
    if (!gem) {
      p4b_gem_close(node, s, handle);
      return -1;
    }
    gbo = drm_gem_vram_of_gem(gem);
    if (drm_gem_vram_pin(gbo, DRM_GEM_VRAM_PL_FLAG_VRAM) ||
        drm_gem_vram_offset(gbo) < 0) {
      drm_gem_object_put(gem);
      p4b_gem_close(node, s, handle);
      return -1;
    }
    drm_gem_vram_unpin(gbo);
    drm_gem_object_put(gem);
    if (p4b_gem_close(node, s, handle))
      return -1;
  }

  /* Pass 1 may retain allocator pages while caches warm; pass 2 gates. */
  ret = p4b_gem_pass(node, s, df, vram_man, P4B_LOOP_ITERS, &pmm1_imm,
                     &pmm1_after, &vram1_imm, &vram1_after);
  if (ret < 0)
    return -1;
  ret = p4b_gem_pass(node, s, df, vram_man, P4B_LOOP_ITERS, &pmm2_imm,
                     &pmm2_after, &vram2_imm, &vram2_after);
  if (ret < 0)
    return -1;

  klogf("[INFO] LinuxKPI: bochs GEM loop pass1 pmm %lu/%lu vram %llu/%llu, "
        "pass2 pmm %lu/%lu vram %llu/%llu\n",
        pmm1_imm, pmm1_after, vram1_imm, vram1_after,
        pmm2_imm, pmm2_after, vram2_imm, vram2_after);

  if (pmm2_after == 0 && vram2_after == 0)
    return 0;
  return 1;
}

/* ── main ───────────────────────────────────────────────────────────────── */

void linuxkpi_test_phase4_bochs(void) {
  struct p4b_scratch s;
  struct p4b_prop_set conn_props, crtc_props, plane_props;
  struct drm_mode_modeinfo mode;
  struct p4b_atomic_buf *ab;
  struct drm_gem_object *gem = NULL;
  struct drm_gem_vram_object *gbo = NULL;
  struct ttm_resource_manager *vram_man;
  struct pci_dev *pdev = NULL;
  void __iomem *mmio = NULL, *fb = NULL;
  void *node;
  int minor = 0;
  uint32_t crtc_id = 0, connector_id = 0, mode_count = 0;
  uint32_t plane_ids[8], plane_count = 0, primary_plane = 0;
  uint32_t conn_crtc_id, crtc_mode_id, crtc_active, plane_fb_id, plane_crtc_id;
  uint32_t p_src_x, p_src_y, p_src_w, p_src_h;
  uint32_t p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
  uint32_t dumb_handle = 0, dumb_pitch = 0, dumb_size = 0, fb_id = 0;
  uint32_t blob_id = 0;
  s64 vram_offset;
  int ret, loop_ret = -1;

  p4b_failures = 0;
  klog_puts("[LINUXKPI] Phase 4 bochs TTM canary test\n");

  node = p4b_find_bochs(&minor);
  if (!node) {
    klog_puts("[SKIP] LinuxKPI: bochs TTM canary (card not present)\n");
    return;
  }
  klogf("[INFO] LinuxKPI: bochs canary is /dev/dri/card%d\n", minor);

  if (p4b_scratch_alloc(&s)) {
    klog_puts("[FAIL] LinuxKPI: bochs scratch allocation failed\n");
    asc_vfs_kernel_close(node);
    return;
  }

  /* Locate the backing PCI device and its register/framebuffer BARs. */
  {
    struct file *f = asc_vfs_node_device(node);
    struct drm_file *df = f ? f->private_data : NULL;
    struct drm_device *ddev = df ? df->minor->dev : NULL;

    /* drm_dev_init() stores the parent device directly (dev->dev =
     * get_device(parent)), so the DRM device's `dev` is the PCI device. */
    pdev = (ddev && ddev->dev) ? to_pci_dev(ddev->dev) : NULL;
    if (pdev && pdev->vendor == 0x1234 && pdev->device == 0x1111)
      p4b_ok("PCI parent is a bochs VGA device");
    else {
      p4b_fail("PCI parent lookup", pdev ? pdev->device : -1);
      pdev = NULL;
    }

    if (pdev && (pci_resource_flags(pdev, 2) & IORESOURCE_MEM))
      mmio = pci_iomap(pdev, 2, 0);
    if (pdev)
      fb = pci_iomap(pdev, 0, 0);

    vram_man = (ddev && ddev->vram_mm)
                   ? ttm_manager_type(&ddev->vram_mm->bdev, TTM_PL_VRAM)
                   : NULL;

    if (!pdev || !mmio || !fb || !vram_man) {
      klog_puts("[FAIL] LinuxKPI: bochs BAR/VRAM manager unavailable\n");
      goto out;
    }
  }

  if (p4b_set_atomic_cap(node, &s)) {
    p4b_fail("SET_CLIENT_CAP(ATOMIC)", 0);
    goto out;
  }

  if (p4b_get_resources(node, &s, &crtc_id, &connector_id) ||
      p4b_get_planes(node, &s, plane_ids, &plane_count) ||
      p4b_connector_modes(node, &s, connector_id, &mode, &mode_count)) {
    p4b_fail("resource discovery", 0);
    goto out;
  }
  if (!mode.hdisplay || !mode.vdisplay) {
    p4b_fail("connector mode", 0);
    goto out;
  }

  if (p4b_obj_props(node, &s, connector_id, DRM_MODE_OBJECT_CONNECTOR,
                    &conn_props) ||
      p4b_obj_props(node, &s, crtc_id, DRM_MODE_OBJECT_CRTC, &crtc_props)) {
    p4b_fail("connector/CRTC property query", 0);
    goto out;
  }
  if (p4b_prop_find(node, &s, &conn_props, "CRTC_ID", &conn_crtc_id, 0) ||
      p4b_prop_find(node, &s, &crtc_props, "MODE_ID", &crtc_mode_id, 0) ||
      p4b_prop_find(node, &s, &crtc_props, "ACTIVE", &crtc_active, 0)) {
    p4b_fail("connector/CRTC props not found", 0);
    goto out;
  }

  for (uint32_t i = 0; i < plane_count; i++) {
    uint64_t type_val = 0;

    if (p4b_obj_props(node, &s, plane_ids[i], DRM_MODE_OBJECT_PLANE,
                      &plane_props))
      continue;
    if (p4b_prop_find(node, &s, &plane_props, "type", 0, &type_val))
      continue;
    if (type_val == 1 /* DRM_PLANE_TYPE_PRIMARY */) {
      primary_plane = plane_ids[i];
      break;
    }
  }
  if (!primary_plane ||
      p4b_obj_props(node, &s, primary_plane, DRM_MODE_OBJECT_PLANE,
                    &plane_props) ||
      p4b_prop_find(node, &s, &plane_props, "FB_ID", &plane_fb_id, 0) ||
      p4b_prop_find(node, &s, &plane_props, "CRTC_ID", &plane_crtc_id, 0) ||
      p4b_prop_find(node, &s, &plane_props, "SRC_X", &p_src_x, 0) ||
      p4b_prop_find(node, &s, &plane_props, "SRC_Y", &p_src_y, 0) ||
      p4b_prop_find(node, &s, &plane_props, "SRC_W", &p_src_w, 0) ||
      p4b_prop_find(node, &s, &plane_props, "SRC_H", &p_src_h, 0) ||
      p4b_prop_find(node, &s, &plane_props, "CRTC_X", &p_crtc_x, 0) ||
      p4b_prop_find(node, &s, &plane_props, "CRTC_Y", &p_crtc_y, 0) ||
      p4b_prop_find(node, &s, &plane_props, "CRTC_W", &p_crtc_w, 0) ||
      p4b_prop_find(node, &s, &plane_props, "CRTC_H", &p_crtc_h, 0)) {
    p4b_fail("primary plane props not found", 0);
    goto out;
  }

  klogf("[  OK  ] LinuxKPI: bochs objects crtc=%u conn=%u plane=%u modes=%u "
        "(%ux%u)\n",
        crtc_id, connector_id, primary_plane, mode_count, mode.hdisplay,
        mode.vdisplay);

  /* Dumb buffer + pixel pattern (still SYSTEM-placed here).  bochs uses the
   * simple-pipe primary plane, whose atomic check requires the destination to
   * cover the entire CRTC, so the buffer matches the committed mode exactly. */
  if (p4b_dumb_create(node, &s, mode.hdisplay, mode.vdisplay, &dumb_handle,
                      &dumb_pitch, &dumb_size)) {
    p4b_fail("CREATE_DUMB", 0);
    goto out;
  }

  {
    struct file *f = asc_vfs_node_device(node);
    struct drm_file *df = f->private_data;
    struct iosys_map map;

    gem = drm_gem_object_lookup(df, dumb_handle);
    if (!gem) {
      p4b_fail("GEM object lookup", 0);
      goto out;
    }
    gbo = drm_gem_vram_of_gem(gem);
    if (drm_gem_vram_vmap(gbo, &map)) {
      p4b_fail("drm_gem_vram_vmap", 0);
      goto out;
    }
    for (uint32_t i = 0; i < dumb_size / 4; i++)
      ((uint32_t *)map.vaddr)[i] = 0xfeed0000u ^ i;
    drm_gem_vram_vunmap(gbo, &map);
    p4b_ok("dumb buffer pattern written via vmap");
  }

  {
    struct drm_mode_fb_cmd2 *fbc = P4B_STRUCT(&s);

    memset(fbc, 0, sizeof(*fbc));
    fbc->width = mode.hdisplay;
    fbc->height = mode.vdisplay;
    fbc->pixel_format = DRM_FORMAT_XRGB8888;
    fbc->handles[0] = dumb_handle;
    fbc->pitches[0] = dumb_pitch;
    if (p4b_ioctl(node, DRM_IOCTL_MODE_ADDFB2) || !fbc->fb_id) {
      p4b_fail("ADDFB2", 0);
      goto out;
    }
    fb_id = fbc->fb_id;
  }
  {
    struct drm_mode_create_blob *cb = P4B_STRUCT(&s);

    memset(cb, 0, sizeof(*cb));
    cb->data = P4B_VA + P4B_OFF_MODE;
    cb->length = sizeof(struct drm_mode_modeinfo);
    if (p4b_ioctl(node, DRM_IOCTL_MODE_CREATEPROPBLOB) || !cb->blob_id) {
      p4b_fail("MODE_CREATEPROPBLOB", 0);
      goto out;
    }
    blob_id = cb->blob_id;
  }

  /* Atomic enable (synchronous commit; prepare_fb pins the FB to VRAM).
   * The simple-pipe primary plane must cover the entire CRTC, hence the
   * mode-sized source and destination rectangles. */
  ab = (struct p4b_atomic_buf *)((char *)s.kva + P4B_OFF_ATOMIC);
  memset(ab, 0, sizeof(*ab));
  {
    uint32_t no = 0, np = 0;

    ab->objs[no] = connector_id;
    ab->counts[no] = 1;
    ab->props[np] = conn_crtc_id;
    ab->values[np] = crtc_id;
    no++;
    np++;

    ab->objs[no] = crtc_id;
    ab->counts[no] = 2;
    ab->props[np] = crtc_mode_id;
    ab->values[np] = blob_id;
    np++;
    ab->props[np] = crtc_active;
    ab->values[np] = 1;
    np++;
    no++;

    ab->objs[no] = primary_plane;
    ab->counts[no] = 10;
    ab->props[np] = plane_fb_id;
    ab->values[np] = fb_id;
    np++;
    ab->props[np] = plane_crtc_id;
    ab->values[np] = crtc_id;
    np++;
    ab->props[np] = p_src_x;
    ab->values[np] = 0;
    np++;
    ab->props[np] = p_src_y;
    ab->values[np] = 0;
    np++;
    ab->props[np] = p_src_w;
    ab->values[np] = (uint64_t)mode.hdisplay << 16;
    np++;
    ab->props[np] = p_src_h;
    ab->values[np] = (uint64_t)mode.vdisplay << 16;
    np++;
    ab->props[np] = p_crtc_x;
    ab->values[np] = 0;
    np++;
    ab->props[np] = p_crtc_y;
    ab->values[np] = 0;
    np++;
    ab->props[np] = p_crtc_w;
    ab->values[np] = mode.hdisplay;
    np++;
    ab->props[np] = p_crtc_h;
    ab->values[np] = mode.vdisplay;
    np++;
    no++;

    ret = p4b_atomic_commit(node, &s, ab, no, np,
                            DRM_MODE_ATOMIC_ALLOW_MODESET);
  }
  if (ret) {
    p4b_fail("atomic enable commit", ret);
    goto out;
  }
  p4b_ok("atomic enable commit");

  /* VBE XRES/YRES/ENABLE must match the committed mode. */
  {
    uint16_t xres = p4b_dispi_read(mmio, P4B_VBE_INDEX_XRES);
    uint16_t yres = p4b_dispi_read(mmio, P4B_VBE_INDEX_YRES);
    uint16_t enable = p4b_dispi_read(mmio, P4B_VBE_INDEX_ENABLE);

    klogf("[INFO] LinuxKPI: bochs VBE xres=%u yres=%u enable=0x%x\n", xres,
          yres, enable);
    if (xres == mode.hdisplay && yres == mode.vdisplay &&
        (enable & P4B_VBE_ENABLED))
      p4b_ok("VBE registers match the committed mode");
    else
      p4b_fail("VBE register mismatch", ((long)xres << 16) | yres);
  }

  /* The plane commit pinned the FB to VRAM; compare its BAR0 contents with
   * the pattern written while it lived in SYSTEM memory. */
  vram_offset = drm_gem_vram_offset(gbo);
  if (vram_offset < 0) {
    p4b_fail("drm_gem_vram_offset", vram_offset);
  } else if ((uint64_t)vram_offset + dumb_size > pci_resource_len(pdev, 0)) {
    p4b_fail("VRAM offset range", (long)vram_offset);
  } else {
    int mismatches = 0;

    klogf("[INFO] LinuxKPI: bochs VRAM offset=0x%llx mem_type=%u\n",
          (unsigned long long)vram_offset,
          gbo->bo.resource ? gbo->bo.resource->mem_type : 0xFFFFu);

    for (uint32_t i = 0; i < 256; i++) {
      uint32_t expect = 0xfeed0000u ^ i;
      uint32_t got = readl((char __iomem *)fb + vram_offset + i * 4);

      if (got != expect)
        mismatches++;
    }
    if (mismatches == 0)
      p4b_ok("BAR0 pattern readback at the BO's VRAM offset");
    else
      p4b_fail("BAR0 pattern readback", mismatches);
  }

  /* Disable commit, then the VRAM BO loop. */
  memset(ab, 0, sizeof(*ab));
  {
    uint32_t no = 0, np = 0;

    ab->objs[no] = connector_id;
    ab->counts[no] = 1;
    ab->props[np] = conn_crtc_id;
    ab->values[np] = 0;
    no++;
    np++;

    ab->objs[no] = crtc_id;
    ab->counts[no] = 2;
    ab->props[np] = crtc_mode_id;
    ab->values[np] = 0;
    np++;
    ab->props[np] = crtc_active;
    ab->values[np] = 0;
    np++;
    no++;

    ab->objs[no] = primary_plane;
    ab->counts[no] = 2;
    ab->props[np] = plane_fb_id;
    ab->values[np] = 0;
    np++;
    ab->props[np] = plane_crtc_id;
    ab->values[np] = 0;
    np++;
    no++;

    ret = p4b_atomic_commit(node, &s, ab, no, np,
                            DRM_MODE_ATOMIC_ALLOW_MODESET);
  }
  if (ret)
    p4b_fail("atomic disable commit", ret);
  else
    p4b_ok("atomic disable commit");

  if (gem) {
    struct file *f = asc_vfs_node_device(node);
    struct drm_file *df = f->private_data;

    drm_gem_object_put(gem);
    gem = NULL;
    gbo = NULL;

    loop_ret = p4b_gem_loop(node, &s, df, vram_man);
    if (loop_ret < 0)
      p4b_fail("GEM loop", 0);
    else if (loop_ret > 0)
      p4b_fail("GEM loop second-pass drift", loop_ret);
    else
      p4b_ok("2x128 VRAM BO create/map/pin/free loop, PMM + VRAM stable");
  }

  if (dumb_handle && p4b_gem_close(node, &s, dumb_handle))
    p4b_fail("GEM_CLOSE", 0);

out:
  if (gem)
    drm_gem_object_put(gem);
  if (pdev && mmio)
    pci_iounmap(pdev, mmio);
  if (pdev && fb)
    pci_iounmap(pdev, fb);
  p4b_scratch_free(&s);
  asc_vfs_kernel_close(node);

  if (p4b_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: bochs suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: bochs suite had %d failure(s)\n", p4b_failures);
}
