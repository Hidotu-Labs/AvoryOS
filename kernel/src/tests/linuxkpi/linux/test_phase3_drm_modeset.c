/* Phase 3 DRM canary completion: atomic modeset, vblank and page-flip events.
 *
 * This is the kernel-side half of C4:
 *
 *   1. enable the DRM atomic client capability, then discover the vkms
 *      connector/CRTC/plane object IDs and the CRTC_ID / MODE_ID / ACTIVE /
 *      FB_ID property IDs (GETRESOURCES, GETPLANERESOURCES, GETCONNECTOR,
 *      MODE_OBJ_GETPROPERTIES + MODE_GETPROPERTY).
 *   2. CREATE_DUMB -> ADDFB2 (XRGB8888) -> MODE_CREATEPROPBLOB from the
 *      connector's first mode.
 *   3. MODE_ATOMIC with DRM_MODE_ATOMIC_ALLOW_MODESET + PAGE_FLIP_EVENT:
 *      connector CRTC_ID, CRTC MODE_ID/ACTIVE=1, plane FB_ID/CRTC_ID/rects.
 *   4. the pending-flip event must arrive through the Phase 2 poll bridge:
 *      asc_vfs_kernel_poll() sees POLLIN and asc_vfs_kernel_read() consumes the
 *      struct drm_event_vblank from drm_read().
 *   5. DRM_IOCTL_WAIT_VBLANK (relative, 1) must return, proving vkms's
 *      drm_crtc_vblank_on()/hrtimer vblank path is live.
 *   6. a 256-iteration GEM create/map/close loop with live-handle uniqueness
 *      and a stable PMM free-page count.
 *   7. a second commit disables the CRTC (ACTIVE=0, plane detached), then the
 *      blob/FB/handle are torn down.
 *
 * Like test_phase3_drm.c this runs from a kernel thread and maps a scratch
 * region into the user range of the active PML4 because DRM's ioctl wrapper
 * copies arguments and pointed-to arrays with copy_from_user()/put_user().
 * Two contiguous pages are mapped: GETCONNECTOR only copies the mode list when
 * the caller's capacity covers it, and vkms exposes 34 modes.
 */

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/delay.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>
#include <linuxkpi/native_vfs.h>

/* Scratch user VA.  Layout:
 *   0x000  ioctl argument struct (<= 128 bytes)
 *   0x100  u32 object id arrays
 *   0x1c0  u32 property id array
 *   0x280  u64 property value array
 *   0x800  drm_event_vblank read buffer
 *   0x1000 mode list (48 * sizeof(drm_mode_modeinfo))
 *   0x1d00 atomic request arrays
 */
#define P3M_VA 0x0000000010000000ULL

#define P3M_OFF_STRUCT 0x000
#define P3M_OFF_IDS 0x100
#define P3M_OFF_PROPS 0x1c0
#define P3M_OFF_VALS 0x280
#define P3M_OFF_EVENT 0x800
#define P3M_OFF_MODE 0x1000
#define P3M_OFF_ATOMIC 0x1d00

#define P3M_MODE_CAP 48
#define P3M_PROP_CAP 48

#define P3M_STRUCT(s) ((void *)((char *)(s)->kva + P3M_OFF_STRUCT))
#define P3M_IDS(s) ((uint32_t *)((char *)(s)->kva + P3M_OFF_IDS))
#define P3M_MODES(s) \
  ((struct drm_mode_modeinfo *)((char *)(s)->kva + P3M_OFF_MODE))

struct p3m_scratch {
  void *phys;
  void *kva;
};

struct p3m_atomic_buf {
  uint32_t objs[4];
  uint32_t counts[4];
  uint32_t props[24];
  uint64_t values[24];
};

struct p3m_prop_set {
  uint32_t ids[P3M_PROP_CAP];
  uint64_t values[P3M_PROP_CAP];
  uint32_t count;
};

static int p3m_scratch_alloc(struct p3m_scratch *s) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  s->phys = asc_pmm_alloc_pages(2);
  if (!s->phys)
    return -1;
  s->kva = (void *)((uint64_t)s->phys + asc_pmm_get_hhdm_offset());
  memset(s->kva, 0, 8192);

  if (!asc_vmm_map_page(pml4, P3M_VA, (uint64_t)s->phys,
                        ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_USER)) {
    asc_pmm_free_pages(s->phys, 2);
    return -1;
  }
  if (!asc_vmm_map_page(pml4, P3M_VA + 4096, (uint64_t)s->phys + 4096,
                        ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_USER)) {
    asc_vmm_unmap_page(pml4, P3M_VA);
    asc_invlpg(P3M_VA);
    asc_pmm_free_pages(s->phys, 2);
    return -1;
  }
  return 0;
}

static void p3m_scratch_free(struct p3m_scratch *s) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  asc_vmm_unmap_page(pml4, P3M_VA);
  asc_vmm_unmap_page(pml4, P3M_VA + 4096);
  asc_invlpg(P3M_VA);
  asc_invlpg(P3M_VA + 4096);
  asc_pmm_free_pages(s->phys, 2);
}

static int p3m_ioctl(void *node, unsigned int request) {
  return asc_vfs_kernel_ioctl(node, request, P3M_VA);
}

/* ── object / property discovery ────────────────────────────────────────── */

static int p3m_set_atomic_cap(void *node, struct p3m_scratch *s) {
  struct drm_set_client_cap *cap = P3M_STRUCT(s);

  memset(cap, 0, sizeof(*cap));
  cap->capability = DRM_CLIENT_CAP_ATOMIC;
  cap->value = 1;
  return p3m_ioctl(node, DRM_IOCTL_SET_CLIENT_CAP);
}

static int p3m_get_resources(void *node, struct p3m_scratch *s,
                             uint32_t *crtc_id, uint32_t *connector_id) {
  struct drm_mode_card_res *res = P3M_STRUCT(s);
  uint32_t *ids = P3M_IDS(s);
  uint32_t ncrtc, nconn;
  int ret;

  memset(res, 0, sizeof(*res));
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_GETRESOURCES);
  if (ret || !res->count_crtcs || !res->count_connectors)
    return ret ? ret : -1;
  ncrtc = res->count_crtcs;
  nconn = res->count_connectors;

  memset(res, 0, sizeof(*res));
  res->crtc_id_ptr = P3M_VA + P3M_OFF_IDS;
  res->connector_id_ptr = P3M_VA + P3M_OFF_IDS + 0x40;
  res->count_crtcs = ncrtc > 8 ? 8 : ncrtc;
  res->count_connectors = nconn > 8 ? 8 : nconn;
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_GETRESOURCES);
  if (ret)
    return ret;

  *crtc_id = ids[0];
  *connector_id = ids[16];
  return (*crtc_id && *connector_id) ? 0 : -1;
}

static int p3m_get_planes(void *node, struct p3m_scratch *s,
                          uint32_t *plane_ids, uint32_t *count) {
  struct drm_mode_get_plane_res *pr = P3M_STRUCT(s);
  uint32_t *ids = P3M_IDS(s);
  int ret;

  memset(pr, 0, sizeof(*pr));
  pr->plane_id_ptr = P3M_VA + P3M_OFF_IDS;
  pr->count_planes = 8;
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_GETPLANERESOURCES);
  if (ret)
    return ret;

  *count = pr->count_planes > 8 ? 8 : pr->count_planes;
  for (uint32_t i = 0; i < *count; i++)
    plane_ids[i] = ids[i];
  return *count ? 0 : -1;
}

static int p3m_connector_modes(void *node, struct p3m_scratch *s,
                               uint32_t connector_id,
                               struct drm_mode_modeinfo *mode,
                               uint32_t *mode_count) {
  struct drm_mode_get_connector *con = P3M_STRUCT(s);
  uint32_t nmodes;
  int ret;

  memset(con, 0, sizeof(*con));
  con->connector_id = connector_id;
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_GETCONNECTOR);
  if (ret || !con->count_modes)
    return ret ? ret : -1;
  nmodes = con->count_modes;

  memset(con, 0, sizeof(*con));
  con->connector_id = connector_id;
  con->count_modes = nmodes > P3M_MODE_CAP ? P3M_MODE_CAP : nmodes;
  con->modes_ptr = P3M_VA + P3M_OFF_MODE;
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_GETCONNECTOR);
  if (ret)
    return ret;

  memcpy(mode, P3M_MODES(s), sizeof(*mode));
  *mode_count = nmodes;
  return 0;
}

static int p3m_obj_props(void *node, struct p3m_scratch *s, uint32_t obj_id,
                         uint32_t obj_type, struct p3m_prop_set *out) {
  struct drm_mode_obj_get_properties *p = P3M_STRUCT(s);
  uint32_t total;
  int ret;

  memset(p, 0, sizeof(*p));
  p->obj_id = obj_id;
  p->obj_type = obj_type;
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_OBJ_GETPROPERTIES);
  if (ret)
    return ret;
  total = p->count_props;
  if (!total)
    return -1;

  memset(p, 0, sizeof(*p));
  p->obj_id = obj_id;
  p->obj_type = obj_type;
  p->count_props = total > P3M_PROP_CAP ? P3M_PROP_CAP : total;
  p->props_ptr = P3M_VA + P3M_OFF_PROPS;
  p->prop_values_ptr = P3M_VA + P3M_OFF_VALS;
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_OBJ_GETPROPERTIES);
  if (ret)
    return ret;

  out->count = total > P3M_PROP_CAP ? P3M_PROP_CAP : total;
  memcpy(out->ids, (char *)s->kva + P3M_OFF_PROPS, out->count * 4);
  memcpy(out->values, (char *)s->kva + P3M_OFF_VALS, out->count * 8);
  return 0;
}

static int p3m_prop_find(void *node, struct p3m_scratch *s,
                         const struct p3m_prop_set *set, const char *name,
                         uint32_t *id_out, uint64_t *value_out) {
  struct drm_mode_get_property *p = P3M_STRUCT(s);

  for (uint32_t i = 0; i < set->count; i++) {
    memset(p, 0, sizeof(*p));
    p->prop_id = set->ids[i];
    if (p3m_ioctl(node, DRM_IOCTL_MODE_GETPROPERTY))
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

/* ── dumb buffer + framebuffer ──────────────────────────────────────────── */

static int p3m_dumb_create(void *node, struct p3m_scratch *s,
                           uint32_t *handle, uint32_t *pitch) {
  struct drm_mode_create_dumb *cd = P3M_STRUCT(s);
  int ret;

  memset(cd, 0, sizeof(*cd));
  cd->width = 64;
  cd->height = 64;
  cd->bpp = 32;
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_CREATE_DUMB);
  if (ret || !cd->handle || !cd->pitch || !cd->size)
    return ret ? ret : -1;
  *handle = cd->handle;
  *pitch = cd->pitch;
  return 0;
}

static int p3m_dumb_map(void *node, struct p3m_scratch *s, uint32_t handle) {
  struct drm_mode_map_dumb *md = P3M_STRUCT(s);
  int ret;

  memset(md, 0, sizeof(*md));
  md->handle = handle;
  ret = p3m_ioctl(node, DRM_IOCTL_MODE_MAP_DUMB);
  return (ret || !md->offset) ? (ret ? ret : -1) : 0;
}

static int p3m_gem_close(void *node, struct p3m_scratch *s, uint32_t handle) {
  struct drm_gem_close *gc = P3M_STRUCT(s);

  memset(gc, 0, sizeof(*gc));
  gc->handle = handle;
  return p3m_ioctl(node, DRM_IOCTL_GEM_CLOSE);
}

/* One measured pass: `loops` create/map/close cycles with two live buffers
 * each.  Handles must be distinct while both are open; the PMM free-page
 * count is sampled immediately (slab pages may still be in flight) and after
 * a short settle.  Returns <0 on a loop error, 0 when the count returned to
 * the pass baseline, 1 on drift. */
static int p3m_gem_pass(void *node, struct p3m_scratch *s, int loops,
                        unsigned long *immediate, unsigned long *settled) {
  unsigned long before, after, later;

  before = asc_pmm_get_free_pages_total();
  for (int i = 0; i < loops; i++) {
    uint32_t h1, h2, p1, p2;

    if (p3m_dumb_create(node, s, &h1, &p1) ||
        p3m_dumb_create(node, s, &h2, &p2))
      return -1;
    if (h1 == h2)
      return -1;
    if (p3m_dumb_map(node, s, h1) || p3m_dumb_map(node, s, h2))
      return -1;
    if (p3m_gem_close(node, s, h1) || p3m_gem_close(node, s, h2))
      return -1;
  }
  after = asc_pmm_get_free_pages_total();
  msleep(200);
  later = asc_pmm_get_free_pages_total();
  *immediate = before >= after ? before - after : 0;
  *settled = before >= later ? before - later : 0;
  return before == later ? 0 : 1;
}

/* Two measured passes of 256 iterations after a 64-iteration warm-up.  The
 * first pass can retain one allocator slab/PCP page while the caches reach the
 * loop's steady state (the same one-page noise the Phase 4 TTM suite reports
 * as delta=-1), so it is informational; the second pass runs on warm caches
 * and must be exactly stable.  A real per-iteration leak shows up there. */
static int p3m_gem_loop(void *node, struct p3m_scratch *s,
                        unsigned long *immediate, unsigned long *settled) {
  enum { WARMUP = 64, LOOPS = 256 };
  unsigned long first_immediate, first_settled, second_immediate;
  int ret;

  for (int i = 0; i < WARMUP; i++) {
    uint32_t h, p;

    if (p3m_dumb_create(node, s, &h, &p) || p3m_dumb_map(node, s, h) ||
        p3m_gem_close(node, s, h))
      return -1;
  }

  ret = p3m_gem_pass(node, s, LOOPS, &first_immediate, &first_settled);
  if (ret < 0)
    return -1;
  ret = p3m_gem_pass(node, s, LOOPS, &second_immediate, settled);
  if (ret < 0)
    return -1;

  *immediate = first_immediate;
  klogf("[INFO] LinuxKPI: vkms GEM pass deltas immediate/settled: "
        "first %lu/%lu, second %lu/%lu\n",
        first_immediate, first_settled, second_immediate, *settled);
  return ret;
}

/* ── atomic commit + event/wait plumbing ────────────────────────────────── */

static int p3m_atomic_commit(void *node, struct p3m_scratch *s,
                             struct p3m_atomic_buf *ab, uint32_t n_objs,
                             uint32_t n_props, uint32_t flags) {
  struct drm_mode_atomic *at = P3M_STRUCT(s);

  memset(at, 0, sizeof(*at));
  at->flags = flags;
  at->count_objs = n_objs;
  at->objs_ptr = P3M_VA + P3M_OFF_ATOMIC +
                 __builtin_offsetof(struct p3m_atomic_buf, objs);
  at->count_props_ptr = P3M_VA + P3M_OFF_ATOMIC +
                        __builtin_offsetof(struct p3m_atomic_buf, counts);
  at->props_ptr = P3M_VA + P3M_OFF_ATOMIC +
                  __builtin_offsetof(struct p3m_atomic_buf, props);
  at->prop_values_ptr = P3M_VA + P3M_OFF_ATOMIC +
                        __builtin_offsetof(struct p3m_atomic_buf, values);
  (void)n_props;
  return p3m_ioctl(node, DRM_IOCTL_MODE_ATOMIC);
}

/* Bounded wait for the page-flip event; returns the event sequence or 0. */
static uint32_t p3m_wait_flip_event(void *node, struct p3m_scratch *s) {
  struct drm_event_vblank ev;
  unsigned int waited = 0;
  int revents = 0;

  while (waited < 500) {
    revents = asc_vfs_kernel_poll(node, 0x1 /* POLLIN */);
    if (revents & 0x1)
      break;
    msleep(10);
    waited += 10;
  }
  if (!(revents & 0x1))
    return 0;

  /* drm_read() copy_to_user()s into the scratch page's user VA. */
  memset((char *)s->kva + P3M_OFF_EVENT, 0, sizeof(ev));
  if (asc_vfs_kernel_read(node, 0, sizeof(ev),
                          (unsigned char *)(P3M_VA + P3M_OFF_EVENT)) !=
      sizeof(ev))
    return 0;
  memcpy(&ev, (char *)s->kva + P3M_OFF_EVENT, sizeof(ev));
  if (ev.base.type != DRM_EVENT_FLIP_COMPLETE)
    return 0;
  return ev.sequence ? ev.sequence : 1;
}

static int p3m_wait_vblank(void *node, struct p3m_scratch *s) {
  union drm_wait_vblank *vbl = P3M_STRUCT(s);

  memset(vbl, 0, sizeof(*vbl));
  vbl->request.type = _DRM_VBLANK_RELATIVE;
  vbl->request.sequence = 1;
  return p3m_ioctl(node, DRM_IOCTL_WAIT_VBLANK);
}

/* ── main ───────────────────────────────────────────────────────────────── */

static void phase3_drm_modeset(void) {
  struct p3m_scratch s;
  struct p3m_prop_set conn_props, crtc_props, plane_props;
  struct drm_mode_modeinfo mode;
  struct p3m_atomic_buf *ab;
  uint32_t crtc_id = 0, connector_id = 0, mode_count = 0;
  uint32_t plane_ids[8], plane_count = 0, primary_plane = 0;
  uint32_t conn_crtc_id, crtc_mode_id, crtc_active, plane_fb_id, plane_crtc_id;
  uint32_t p_src_x, p_src_y, p_src_w, p_src_h;
  uint32_t p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
  uint32_t dumb_handle, dumb_pitch, fb_id = 0, blob_id = 0;
  unsigned long gem_imm = 0, gem_delta = 0;
  void *node;
  int ret, gem_ret;

  node = asc_vfs_kernel_open("/dev/dri/card1");
  if (!node) {
    klog_puts("[SKIP] LinuxKPI: card1 atomic modeset tests (node absent)\n");
    return;
  }
  if (p3m_scratch_alloc(&s)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms atomic scratch allocation failed\n");
    asc_vfs_kernel_close(node);
    return;
  }

  if (p3m_set_atomic_cap(node, &s)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms SET_CLIENT_CAP(ATOMIC) failed\n");
    goto out;
  }

  if (p3m_get_resources(node, &s, &crtc_id, &connector_id) ||
      p3m_get_planes(node, &s, plane_ids, &plane_count)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms atomic resource discovery failed\n");
    goto out;
  }

  if (p3m_connector_modes(node, &s, connector_id, &mode, &mode_count)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms connector mode discovery failed\n");
    goto out;
  }

  if (p3m_obj_props(node, &s, connector_id, DRM_MODE_OBJECT_CONNECTOR,
                    &conn_props) ||
      p3m_obj_props(node, &s, crtc_id, DRM_MODE_OBJECT_CRTC, &crtc_props)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms connector/CRTC property query failed\n");
    goto out;
  }
  if (p3m_prop_find(node, &s, &conn_props, "CRTC_ID", &conn_crtc_id, 0) ||
      p3m_prop_find(node, &s, &crtc_props, "MODE_ID", &crtc_mode_id, 0) ||
      p3m_prop_find(node, &s, &crtc_props, "ACTIVE", &crtc_active, 0)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms connector/CRTC props not found\n");
    goto out;
  }

  /* The primary plane is the first plane whose immutable "type" property is
   * DRM_PLANE_TYPE_PRIMARY (1); the cursor plane follows it. */
  for (uint32_t i = 0; i < plane_count; i++) {
    uint64_t type_val = 0;

    if (p3m_obj_props(node, &s, plane_ids[i], DRM_MODE_OBJECT_PLANE,
                      &plane_props))
      continue;
    if (p3m_prop_find(node, &s, &plane_props, "type", 0, &type_val))
      continue;
    /* DRM_PLANE_TYPE_PRIMARY is the kernel-internal enum; the UAPI only
     * documents the property values (Overlay 0, Primary 1, Cursor 2). */
    if (type_val == 1) {
      primary_plane = plane_ids[i];
      break;
    }
  }
  if (!primary_plane ||
      p3m_obj_props(node, &s, primary_plane, DRM_MODE_OBJECT_PLANE,
                    &plane_props) ||
      p3m_prop_find(node, &s, &plane_props, "FB_ID", &plane_fb_id, 0) ||
      p3m_prop_find(node, &s, &plane_props, "CRTC_ID", &plane_crtc_id, 0) ||
      p3m_prop_find(node, &s, &plane_props, "SRC_X", &p_src_x, 0) ||
      p3m_prop_find(node, &s, &plane_props, "SRC_Y", &p_src_y, 0) ||
      p3m_prop_find(node, &s, &plane_props, "SRC_W", &p_src_w, 0) ||
      p3m_prop_find(node, &s, &plane_props, "SRC_H", &p_src_h, 0) ||
      p3m_prop_find(node, &s, &plane_props, "CRTC_X", &p_crtc_x, 0) ||
      p3m_prop_find(node, &s, &plane_props, "CRTC_Y", &p_crtc_y, 0) ||
      p3m_prop_find(node, &s, &plane_props, "CRTC_W", &p_crtc_w, 0) ||
      p3m_prop_find(node, &s, &plane_props, "CRTC_H", &p_crtc_h, 0)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms primary plane props not found\n");
    goto out;
  }

  klogf("[  OK  ] LinuxKPI: vkms atomic objects crtc=%u conn=%u plane=%u "
        "modes=%u (%ux%u)\n",
        crtc_id, connector_id, primary_plane, mode_count, mode.hdisplay,
        mode.vdisplay);

  /* GEM invariants first: the modeset below keeps a dumb buffer alive. */
  gem_ret = p3m_gem_loop(node, &s, &gem_imm, &gem_delta);
  if (gem_ret < 0) {
    klog_puts("[ FAIL ] LinuxKPI: vkms GEM loop create/map/close failed\n");
    goto out;
  }
  if (gem_ret > 0)
    klogf("[ FAIL ] LinuxKPI: vkms GEM loop PMM drift %lu pages on the warm "
          "second pass of 256 iterations (first-pass immediate %lu)\n",
          gem_delta, gem_imm);
  else
    klogf("[  OK  ] LinuxKPI: vkms GEM loop 2x256 iterations, distinct live "
          "handles, PMM stable (first-pass immediate delta %lu)\n",
          gem_imm);

  if (p3m_dumb_create(node, &s, &dumb_handle, &dumb_pitch)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms modeset CREATE_DUMB failed\n");
    goto out;
  }

  {
    struct drm_mode_fb_cmd2 *fb = P3M_STRUCT(&s);

    memset(fb, 0, sizeof(*fb));
    fb->width = 64;
    fb->height = 64;
    fb->pixel_format = DRM_FORMAT_XRGB8888;
    fb->handles[0] = dumb_handle;
    fb->pitches[0] = dumb_pitch;
    if (p3m_ioctl(node, DRM_IOCTL_MODE_ADDFB2) || !fb->fb_id) {
      klog_puts("[ FAIL ] LinuxKPI: vkms ADDFB2 failed\n");
      goto out;
    }
    fb_id = fb->fb_id;
  }

  {
    struct drm_mode_create_blob *cb = P3M_STRUCT(&s);

    memset(cb, 0, sizeof(*cb));
    cb->data = P3M_VA + P3M_OFF_MODE;
    cb->length = sizeof(struct drm_mode_modeinfo);
    if (p3m_ioctl(node, DRM_IOCTL_MODE_CREATEPROPBLOB) || !cb->blob_id) {
      klog_puts("[ FAIL ] LinuxKPI: vkms MODE_CREATEPROPBLOB failed\n");
      goto out;
    }
    blob_id = cb->blob_id;
  }

  klogf("[  OK  ] LinuxKPI: vkms dumb buffer handle=%u pitch=%u fb_id=%u "
        "blob_id=%u\n",
        dumb_handle, dumb_pitch, fb_id, blob_id);

  /* Atomic enable: connector -> CRTC, CRTC mode blob + ACTIVE=1, primary plane
   * full-surface.  PAGE_FLIP_EVENT leaves an event to read back. */
  ab = (struct p3m_atomic_buf *)((char *)s.kva + P3M_OFF_ATOMIC);
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
    ab->values[np] = (uint64_t)64 << 16;
    np++;
    ab->props[np] = p_src_h;
    ab->values[np] = (uint64_t)64 << 16;
    np++;
    ab->props[np] = p_crtc_x;
    ab->values[np] = 0;
    np++;
    ab->props[np] = p_crtc_y;
    ab->values[np] = 0;
    np++;
    ab->props[np] = p_crtc_w;
    ab->values[np] = 64;
    np++;
    ab->props[np] = p_crtc_h;
    ab->values[np] = 64;
    np++;
    no++;

    ret = p3m_atomic_commit(node, &s, ab, no, np,
                            DRM_MODE_ATOMIC_ALLOW_MODESET |
                                DRM_MODE_PAGE_FLIP_EVENT);
  }
  if (ret) {
    klogf("[ FAIL ] LinuxKPI: vkms atomic enable commit -> %d\n", ret);
    goto out;
  }
  klog_puts("[  OK  ] LinuxKPI: vkms atomic enable commit (ALLOW_MODESET)\n");

  {
    uint32_t seq = p3m_wait_flip_event(node, &s);

    if (!seq) {
      klog_puts("[ FAIL ] LinuxKPI: vkms page-flip event poll/read failed\n");
      goto out;
    }
    klogf("[  OK  ] LinuxKPI: vkms page-flip event via poll/read (seq=%u)\n",
          seq);
  }

  ret = p3m_wait_vblank(node, &s);
  if (ret) {
    klogf("[ FAIL ] LinuxKPI: vkms WAIT_VBLANK -> %d\n", ret);
    goto out;
  }
  klog_puts("[  OK  ] LinuxKPI: vkms WAIT_VBLANK relative 1 returned\n");

  /* Atomic disable: detach the connector and plane, then clear the CRTC
   * MODE_ID/ACTIVE.  The connector must be part of the commit or
   * drm_atomic_helper_check_modeset() rejects enable=0 with a non-empty
   * connector_mask. */
  memset(ab, 0, sizeof(*ab));
  {
    uint32_t no = 0, np = 0;

    ab->objs[no] = connector_id;
    ab->counts[no] = 1;
    ab->props[np] = conn_crtc_id;
    ab->values[np] = 0;
    np++;
    no++;

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

    ret = p3m_atomic_commit(node, &s, ab, no, np,
                            DRM_MODE_ATOMIC_ALLOW_MODESET);
  }
  if (ret) {
    klogf("[ FAIL ] LinuxKPI: vkms atomic disable commit -> %d\n", ret);
    goto out;
  }
  klog_puts("[  OK  ] LinuxKPI: vkms atomic disable commit\n");

out:
  /* Teardown in reverse order; entries may not have been created. */
  if (blob_id) {
    struct drm_mode_destroy_blob *db = P3M_STRUCT(&s);

    memset(db, 0, sizeof(*db));
    db->blob_id = blob_id;
    if (p3m_ioctl(node, DRM_IOCTL_MODE_DESTROYPROPBLOB))
      klog_puts("[WARN] LinuxKPI: vkms blob destroy failed\n");
  }
  if (fb_id) {
    uint32_t *fbid = P3M_STRUCT(&s);

    *fbid = fb_id;
    if (p3m_ioctl(node, DRM_IOCTL_MODE_RMFB))
      klog_puts("[WARN] LinuxKPI: vkms RMFB failed\n");
  }
  if (fb_id) {
    uint32_t h = dumb_handle;

    if (p3m_gem_close(node, &s, h))
      klog_puts("[WARN] LinuxKPI: vkms mode-set GEM close failed\n");
  }

  p3m_scratch_free(&s);
  asc_vfs_kernel_close(node);
}

void linuxkpi_test_phase3_drm_modeset(void) {
  klog_puts("[LINUXKPI] Phase 3 DRM atomic modeset/vblank self-test\n");
  phase3_drm_modeset();
}
