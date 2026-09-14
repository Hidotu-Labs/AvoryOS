/* Phase 6 C6 — amdgpu DCN KMS self-test (kernel-side).
 *
 * Runs after the P6 link suite on every C6 boot, so the KMS path gets
 * evidence without an interactive session (QEMU-only validation).  It mirrors
 * the userland `bin/test_kpi_amdgpu` KMS section through the kernel VFS
 * bridge:
 *
 *   1. discover the amdgpu card node by DRM_IOCTL_VERSION name (card2 in the
 *      current vkms/vgem layout, discovered rather than hard-coded);
 *   2. force an HDMI/DP connector on through the DRM 6.6 RW `status` sysfs
 *      attribute ("on" -> forced probe + noedid modes) when no sink is
 *      attached, remembering the path to restore "detect" afterwards;
 *   3. SET_CLIENT_CAP(ATOMIC), GETRESOURCES/GETPLANERESOURCES, pick the
 *      connector that now has modes and its largest mode, then discover the
 *      connector/CRTC/primary/cursor property IDs;
 *   4. CREATE_DUMB x2 -> ADDFB2 at the mode size -> MODE_CREATEPROPBLOB;
 *      atomic enable (ALLOW_MODESET | PAGE_FLIP_EVENT), consume the flip
 *      event through the Phase 2 poll/read bridge, flip to the second FB and
 *      consume the second event, then DRM_IOCTL_WAIT_VBLANK;
 *   5. cursor plane on/off (64x64 ARGB) and the atomic disable commit;
 *   6. teardown and connector unforce.
 *
 * Like the Phase 3 modeset suite, arguments and pointed-to arrays live in a
 * scratch region mapped into the user range of the active PML4, because the
 * DRM ioctl wrapper copies them with copy_from_user()/put_user().
 */

#include <drm/drm.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/sprintf.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>
#include <linuxkpi/native_vfs.h>

extern struct drm_device *linuxkpi_drm_find_dev(const char *name);
extern int drm_edid_override_set(struct drm_connector *connector,
                                 const void *edid, size_t size);
extern int drm_edid_override_reset(struct drm_connector *connector);

/* Scratch user VA (distinct from the P3 suite's 0x10000000).  Layout:
 *   0x000  ioctl argument struct
 *   0x100  u32 id arrays (crtcs at +0, connectors at +0x40)
 *   0x1c0  u32 property id array
 *   0x300  u64 property value array
 *   0x600  DRM version name buffer (32 bytes)
 *   0x800  drm_event_vblank read buffer
 *   0x1000 mode list (48 * sizeof(drm_mode_modeinfo))
 *   0x1d00 atomic request arrays
 */
#define P6D_VA 0x0000000020000000ULL

#define P6D_OFF_STRUCT 0x000
#define P6D_OFF_IDS 0x100
#define P6D_OFF_PROPS 0x1c0
#define P6D_OFF_VALS 0x300
#define P6D_OFF_NAME 0x600
#define P6D_OFF_EVENT 0x800
#define P6D_OFF_MODE 0x1000
#define P6D_OFF_ATOMIC 0x1d00
#define P6D_OFF_BLOB 0x1f00

#define P6D_MODE_CAP 48
#define P6D_PROP_CAP 48

#define P6D_STRUCT(s) ((void *)((char *)(s)->kva + P6D_OFF_STRUCT))
#define P6D_IDS(s) ((uint32_t *)((char *)(s)->kva + P6D_OFF_IDS))
#define P6D_MODES(s) \
  ((struct drm_mode_modeinfo *)((char *)(s)->kva + P6D_OFF_MODE))

struct p6d_scratch {
  void *phys;
  void *kva;
};

struct p6d_prop_set {
  uint32_t ids[P6D_PROP_CAP];
  uint64_t values[P6D_PROP_CAP];
  uint32_t count;
};

struct p6d_atomic_buf {
  uint32_t objs[6];
  uint32_t counts[6];
  uint32_t props[32];
  uint64_t values[32];
};

static int p6d_failures;

static void p6d_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: dcn %s\n", what);
}

static void p6d_fail(const char *what, long v) {
  p6d_failures++;
  klogf("[FAIL] LinuxKPI: dcn %s (%ld)\n", what, v);
}

static void p6d_scratch_alloc(struct p6d_scratch *s) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  s->phys = NULL;
  s->kva = NULL;
  s->phys = asc_pmm_alloc_pages(2);
  if (!s->phys)
    return;
  s->kva = (void *)((uint64_t)s->phys + asc_pmm_get_hhdm_offset());
  memset(s->kva, 0, 8192);

  if (!asc_vmm_map_page(pml4, P6D_VA, (uint64_t)s->phys,
                        ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_USER) ||
      !asc_vmm_map_page(pml4, P6D_VA + 4096, (uint64_t)s->phys + 4096,
                        ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_USER)) {
    asc_vmm_unmap_page(pml4, P6D_VA);
    asc_invlpg(P6D_VA);
    asc_pmm_free_pages(s->phys, 2);
    s->phys = NULL;
    s->kva = NULL;
  }
}

static void p6d_scratch_free(struct p6d_scratch *s) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  if (!s->phys)
    return;
  asc_vmm_unmap_page(pml4, P6D_VA);
  asc_vmm_unmap_page(pml4, P6D_VA + 4096);
  asc_invlpg(P6D_VA);
  asc_invlpg(P6D_VA + 4096);
  asc_pmm_free_pages(s->phys, 2);
  s->phys = NULL;
  s->kva = NULL;
}

static int p6d_ioctl(void *node, unsigned int request) {
  return asc_vfs_kernel_ioctl(node, request, P6D_VA);
}

/* ── card + connector discovery ─────────────────────────────────────────── */

/* Open /dev/dri/cardN and keep it when DRM_IOCTL_VERSION says "amdgpu".
 * Returns the node or NULL; *card_out gets N. */
static void *p6d_find_card(struct p6d_scratch *s, int *card_out) {
  struct drm_version *ver = P6D_STRUCT(s);

  for (int i = 2; i <= 5; i++) {
    char path[32];
    char name[32];
    void *node;

    snprintf(path, sizeof(path), "/dev/dri/card%d", i);
    node = asc_vfs_kernel_open(path);
    if (!node)
      continue;

    memset(ver, 0, sizeof(*ver));
    ver->name = (char *)(uintptr_t)(P6D_VA + P6D_OFF_NAME);
    ver->name_len = 31;
    if (p6d_ioctl(node, DRM_IOCTL_VERSION) == 0) {
      memcpy(name, (char *)s->kva + P6D_OFF_NAME, sizeof(name));
      name[31] = '\0';
      if (strcmp(name, "amdgpu") == 0) {
        if (card_out)
          *card_out = i;
        return node;
      }
    }
    asc_vfs_kernel_close(node);
  }
  return NULL;
}

/* Force an HDMI/DP connector on with an EDID override, the igt-style
 * headless combination: amdgpu DM refuses `force=on` without an EDID, but
 * with `connector->edid_override` set `amdgpu_dm_connector_funcs_force()`
 * copies it into `aconnector->edid` and `handle_edid_mgmt()` builds an
 * emulated sink, so modes exist without a physical monitor.  Returns the
 * connector (to unforce later) or NULL. */
/* Prefer a connector with a physical sink attached, then any HDMI, then DP.
 * Callers fall back to the EDID-override + force path only when the chosen
 * connector reports disconnected. */
static struct drm_connector *p6d_find_connector(struct drm_device *dev) {
  struct drm_connector *connector, *hdmi = NULL, *dp = NULL;
  struct drm_connector *conn_hdmi = NULL, *conn_dp = NULL;
  struct drm_connector_list_iter iter;

  drm_connector_list_iter_begin(dev, &iter);
  drm_for_each_connector_iter(connector, &iter) {
    bool connected = connector->status == connector_status_connected;

    if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA) {
      if (!hdmi)
        hdmi = connector;
      if (connected && !conn_hdmi)
        conn_hdmi = connector;
    } else if (connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort) {
      if (!dp)
        dp = connector;
      if (connected && !conn_dp)
        conn_dp = connector;
    }
  }
  drm_connector_list_iter_end(&iter);

  if (conn_hdmi)
    return conn_hdmi;
  if (conn_dp)
    return conn_dp;
  return hdmi ? hdmi : dp;
}

/* One 128-byte EDID: 1920x1080@60 DTD (preferred) plus 640x480@60 and
 * 1024x768@60 established timings. */
static void p6d_build_edid(u8 *e) {
  static const u8 dtd_1080p[18] = {
      0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58,
      0x2c, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
  };
  static const u8 dtd_480p[18] = {
      0xd5, 0x09, 0x80, 0xa0, 0x20, 0xe0, 0x2d, 0x10, 0x10,
      0x60, 0xa2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
  };
  u32 sum = 0;

  memset(e, 0, 128);
  e[0] = 0x00;
  memset(&e[1], 0xff, 6);
  e[7] = 0x00;
  e[8] = 0x04; /* mfg id */
  e[9] = 0x21;
  e[10] = 0x34; /* product code */
  e[11] = 0x12;
  e[16] = 0x00; /* week/year (2020) */
  e[17] = 0x1e;
  e[18] = 0x01; /* EDID 1.3 */
  e[19] = 0x03;
  e[20] = 0x80; /* digital input */
  e[21] = 16;   /* h size cm */
  e[22] = 10;   /* v size cm */
  e[23] = 120;  /* gamma 2.2 */
  e[24] = 0x02; /* preferred timing in the first DTD */
  e[35] = 0x20; /* established: 640x480@60 */
  e[36] = 0x08; /* established: 1024x768@60 */
  memcpy(&e[54], dtd_1080p, sizeof(dtd_1080p));
  memcpy(&e[72], dtd_480p, sizeof(dtd_480p));
  for (int i = 0; i < 127; i++)
    sum += e[i];
  e[127] = (u8)(0 - sum);
}

/* ── resources ──────────────────────────────────────────────────────────── */

static int p6d_set_atomic_cap(void *node, struct p6d_scratch *s) {
  struct drm_set_client_cap *cap = P6D_STRUCT(s);

  memset(cap, 0, sizeof(*cap));
  cap->capability = DRM_CLIENT_CAP_ATOMIC;
  cap->value = 1;
  return p6d_ioctl(node, DRM_IOCTL_SET_CLIENT_CAP);
}

static int p6d_get_resources(void *node, struct p6d_scratch *s,
                             uint32_t *crtcs, uint32_t *n_crtcs,
                             uint32_t *conns, uint32_t *n_conns) {
  struct drm_mode_card_res *res = P6D_STRUCT(s);
  uint32_t count_crtcs, count_conns;
  uint32_t *ids = P6D_IDS(s);
  int ret;

  memset(res, 0, sizeof(*res));
  ret = p6d_ioctl(node, DRM_IOCTL_MODE_GETRESOURCES);
  if (ret || !res->count_crtcs || !res->count_connectors)
    return ret ? ret : -1;
  count_crtcs = res->count_crtcs;
  count_conns = res->count_connectors;

  memset(res, 0, sizeof(*res));
  res->crtc_id_ptr = P6D_VA + P6D_OFF_IDS;
  res->connector_id_ptr = P6D_VA + P6D_OFF_IDS + 0x40;
  res->count_crtcs = count_crtcs > 8 ? 8 : count_crtcs;
  res->count_connectors = count_conns > 8 ? 8 : count_conns;
  ret = p6d_ioctl(node, DRM_IOCTL_MODE_GETRESOURCES);
  if (ret)
    return ret;

  *n_crtcs = res->count_crtcs;
  *n_conns = res->count_connectors;
  for (uint32_t i = 0; i < *n_crtcs; i++)
    crtcs[i] = ids[i];
  for (uint32_t i = 0; i < *n_conns; i++)
    conns[i] = ids[16 + i];
  return 0;
}

static int p6d_get_planes(void *node, struct p6d_scratch *s,
                          uint32_t *plane_ids, uint32_t *count) {
  struct drm_mode_get_plane_res *pr = P6D_STRUCT(s);
  uint32_t *ids = P6D_IDS(s);
  int ret;

  memset(pr, 0, sizeof(*pr));
  pr->plane_id_ptr = P6D_VA + P6D_OFF_IDS;
  pr->count_planes = 16;
  ret = p6d_ioctl(node, DRM_IOCTL_MODE_GETPLANERESOURCES);
  if (ret)
    return ret;

  *count = pr->count_planes > 16 ? 16 : pr->count_planes;
  for (uint32_t i = 0; i < *count; i++)
    plane_ids[i] = ids[i];
  return *count ? 0 : -1;
}

/* Primary planes are per-pipe: DM sets possible_crtcs = 1 << pipe_index for
 * them, and drm_atomic_plane_check() rejects a (plane, CRTC) pair that is not
 * in that mask with -EINVAL, before amdgpu_dm_atomic_check() ever runs. */
static int p6d_get_plane(void *node, struct p6d_scratch *s, uint32_t plane_id,
                         uint32_t *possible_crtcs) {
  struct drm_mode_get_plane *p = P6D_STRUCT(s);

  memset(p, 0, sizeof(*p));
  p->plane_id = plane_id;
  if (p6d_ioctl(node, DRM_IOCTL_MODE_GETPLANE))
    return -1;
  if (possible_crtcs)
    *possible_crtcs = p->possible_crtcs;
  return 0;
}

/* Pick the connector with modes (prefer connection == connected) and its
 * largest mode.  The first GETCONNECTOR call probes the connector when its
 * mode count is zero. */
static int p6d_pick_connector(void *node, struct p6d_scratch *s,
                              const uint32_t *conns, uint32_t n_conns,
                              uint32_t *conn_out,
                              struct drm_mode_modeinfo *mode_out,
                              uint32_t *nmodes_out) {
  struct drm_mode_get_connector *con = P6D_STRUCT(s);
  uint32_t best_conn = 0, best_modes = 0;
  struct drm_mode_modeinfo best_mode;
  int have_connected = 0;

  memset(&best_mode, 0, sizeof(best_mode));

  for (uint32_t i = 0; i < n_conns; i++) {
    uint32_t nmodes, cap, best = 0;
    uint64_t best_area = 0;
    int connected;

    memset(con, 0, sizeof(*con));
    con->connector_id = conns[i];
    if (p6d_ioctl(node, DRM_IOCTL_MODE_GETCONNECTOR) || !con->count_modes)
      continue;
    nmodes = con->count_modes;
    connected = con->connection == 1;

    memset(con, 0, sizeof(*con));
    con->connector_id = conns[i];
    con->count_modes = nmodes > P6D_MODE_CAP ? P6D_MODE_CAP : nmodes;
    con->modes_ptr = P6D_VA + P6D_OFF_MODE;
    if (p6d_ioctl(node, DRM_IOCTL_MODE_GETCONNECTOR))
      continue;
    cap = con->count_modes;

    for (uint32_t m = 0; m < cap; m++) {
      uint64_t area = (uint64_t)P6D_MODES(s)[m].hdisplay *
                      P6D_MODES(s)[m].vdisplay;

      if (area > best_area) {
        best_area = area;
        best = m;
      }
    }
    (void)best;

    if (!best_modes || (connected && !have_connected)) {
      best_conn = conns[i];
      best_modes = nmodes;
      best_mode = P6D_MODES(s)[best];
      have_connected = connected;
    }
    if (connected)
      break;
  }

  if (!best_modes)
    return -1;
  *conn_out = best_conn;
  *mode_out = best_mode;
  *nmodes_out = best_modes;
  return 0;
}

static int p6d_obj_props(void *node, struct p6d_scratch *s, uint32_t obj_id,
                         uint32_t obj_type, struct p6d_prop_set *out) {
  struct drm_mode_obj_get_properties *p = P6D_STRUCT(s);
  uint32_t total;
  int ret;

  memset(p, 0, sizeof(*p));
  p->obj_id = obj_id;
  p->obj_type = obj_type;
  ret = p6d_ioctl(node, DRM_IOCTL_MODE_OBJ_GETPROPERTIES);
  if (ret)
    return ret;
  total = p->count_props;
  if (!total)
    return -1;

  memset(p, 0, sizeof(*p));
  p->obj_id = obj_id;
  p->obj_type = obj_type;
  p->count_props = total > P6D_PROP_CAP ? P6D_PROP_CAP : total;
  p->props_ptr = P6D_VA + P6D_OFF_PROPS;
  p->prop_values_ptr = P6D_VA + P6D_OFF_VALS;
  ret = p6d_ioctl(node, DRM_IOCTL_MODE_OBJ_GETPROPERTIES);
  if (ret)
    return ret;

  out->count = total > P6D_PROP_CAP ? P6D_PROP_CAP : total;
  memcpy(out->ids, (char *)s->kva + P6D_OFF_PROPS, out->count * 4);
  memcpy(out->values, (char *)s->kva + P6D_OFF_VALS, out->count * 8);
  return 0;
}

static int p6d_prop_find(void *node, struct p6d_scratch *s,
                         const struct p6d_prop_set *set, const char *name,
                         uint32_t *id_out, uint64_t *value_out) {
  struct drm_mode_get_property *p = P6D_STRUCT(s);

  for (uint32_t i = 0; i < set->count; i++) {
    memset(p, 0, sizeof(*p));
    p->prop_id = set->ids[i];
    if (p6d_ioctl(node, DRM_IOCTL_MODE_GETPROPERTY))
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

/* ── buffers ────────────────────────────────────────────────────────────── */

static int p6d_dumb_create(void *node, struct p6d_scratch *s, uint32_t w,
                           uint32_t h, uint32_t *handle, uint32_t *pitch) {
  struct drm_mode_create_dumb *cd = P6D_STRUCT(s);

  memset(cd, 0, sizeof(*cd));
  cd->width = w;
  cd->height = h;
  cd->bpp = 32;
  if (p6d_ioctl(node, DRM_IOCTL_MODE_CREATE_DUMB) || !cd->handle || !cd->pitch)
    return -1;
  *handle = cd->handle;
  *pitch = cd->pitch;
  return 0;
}

static int p6d_add_fb2(void *node, struct p6d_scratch *s, uint32_t handle,
                       uint32_t w, uint32_t h, uint32_t pitch,
                       uint32_t format, uint32_t *fb_id) {
  struct drm_mode_fb_cmd2 *fb = P6D_STRUCT(s);

  memset(fb, 0, sizeof(*fb));
  fb->width = w;
  fb->height = h;
  fb->pixel_format = format;
  fb->handles[0] = handle;
  fb->pitches[0] = pitch;
  if (p6d_ioctl(node, DRM_IOCTL_MODE_ADDFB2) || !fb->fb_id)
    return -1;
  *fb_id = fb->fb_id;
  return 0;
}

static int p6d_dumb_destroy(void *node, struct p6d_scratch *s,
                            uint32_t handle) {
  struct drm_mode_destroy_dumb *dd = P6D_STRUCT(s);

  memset(dd, 0, sizeof(*dd));
  dd->handle = handle;
  return p6d_ioctl(node, DRM_IOCTL_MODE_DESTROY_DUMB);
}

static int p6d_rmfb(void *node, struct p6d_scratch *s, uint32_t fb_id) {
  uint32_t *slot = P6D_STRUCT(s);

  *slot = fb_id;
  return p6d_ioctl(node, DRM_IOCTL_MODE_RMFB);
}

static int p6d_destroy_blob(void *node, struct p6d_scratch *s,
                            uint32_t blob_id) {
  struct drm_mode_destroy_blob *db = P6D_STRUCT(s);

  memset(db, 0, sizeof(*db));
  db->blob_id = blob_id;
  return p6d_ioctl(node, DRM_IOCTL_MODE_DESTROYPROPBLOB);
}

/* ── atomic commits ─────────────────────────────────────────────────────── */

static int p6d_atomic_commit(void *node, struct p6d_scratch *s,
                             uint32_t n_objs, uint32_t flags) {
  struct drm_mode_atomic *at = P6D_STRUCT(s);

  memset(at, 0, sizeof(*at));
  at->flags = flags;
  at->count_objs = n_objs;
  at->objs_ptr = P6D_VA + P6D_OFF_ATOMIC +
                 __builtin_offsetof(struct p6d_atomic_buf, objs);
  at->count_props_ptr = P6D_VA + P6D_OFF_ATOMIC +
                        __builtin_offsetof(struct p6d_atomic_buf, counts);
  at->props_ptr = P6D_VA + P6D_OFF_ATOMIC +
                  __builtin_offsetof(struct p6d_atomic_buf, props);
  at->prop_values_ptr = P6D_VA + P6D_OFF_ATOMIC +
                        __builtin_offsetof(struct p6d_atomic_buf, values);
  return p6d_ioctl(node, DRM_IOCTL_MODE_ATOMIC);
}

static uint32_t p6d_wait_flip_event(void *node, struct p6d_scratch *s) {
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

  memset((char *)s->kva + P6D_OFF_EVENT, 0, sizeof(ev));
  if (asc_vfs_kernel_read(node, 0, sizeof(ev),
                          (unsigned char *)(P6D_VA + P6D_OFF_EVENT)) !=
      sizeof(ev))
    return 0;
  memcpy(&ev, (char *)s->kva + P6D_OFF_EVENT, sizeof(ev));
  if (ev.base.type != DRM_EVENT_FLIP_COMPLETE)
    return 0;
  return ev.sequence ? ev.sequence : 1;
}

/* The vblank request selects its CRTC through the high bits of `type`
 * (`drm_wait_vblank_ioctl()`); without them it defaults to CRTC 0, which has
 * no stream once the commit landed on the primary plane's own pipe. */
static int p6d_wait_vblank(void *node, struct p6d_scratch *s,
                           uint32_t crtc_index) {
  union drm_wait_vblank *vbl = P6D_STRUCT(s);

  memset(vbl, 0, sizeof(*vbl));
  vbl->request.type = _DRM_VBLANK_RELATIVE |
                      (crtc_index << _DRM_VBLANK_HIGH_CRTC_SHIFT);
  vbl->request.sequence = 1;
  return p6d_ioctl(node, DRM_IOCTL_WAIT_VBLANK);
}

/* ── suite ──────────────────────────────────────────────────────────────── */

void linuxkpi_test_phase6_dcn(void) {
  struct p6d_scratch s;
  struct p6d_prop_set conn_props, crtc_props, plane_props, cursor_props;
  struct drm_mode_modeinfo mode;
  struct p6d_atomic_buf *ab;
  struct drm_mode_create_blob *blob;
  uint32_t crtcs[8], conns[8], n_crtcs = 0, n_conns = 0;
  uint32_t plane_ids[16], plane_count = 0, primary_plane = 0, cursor_plane = 0;
  uint32_t crtc_id = 0, conn_id = 0, mode_count = 0, crtc_index = 0;
  uint32_t conn_crtc_id, crtc_mode_id, crtc_active, plane_fb_id, plane_crtc_id;
  uint32_t p_src_x, p_src_y, p_src_w, p_src_h;
  uint32_t p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
  uint32_t dumb1 = 0, pitch1 = 0, fb1 = 0;
  uint32_t dumb2 = 0, pitch2 = 0, fb2 = 0;
  uint32_t cdumb = 0, cpitch = 0, cfb = 0;
  uint32_t blob_id = 0;
  struct drm_device *dev;
  struct drm_connector *aconnector = NULL;
  u8 edid[128];
  void *node;
  int card = -1;
  int ret;
  int forced = 0;

  p6d_failures = 0;
  memset(&mode, 0, sizeof(mode));

  p6d_scratch_alloc(&s);
  if (!s.phys) {
    p6d_fail("scratch allocation failed", 0);
    return;
  }

  node = p6d_find_card(&s, &card);
  if (!node) {
    klog_puts("[SKIP] LinuxKPI: dcn KMS suite (no amdgpu card node)\n");
    p6d_scratch_free(&s);
    return;
  }
  blob = P6D_STRUCT(&s);

  /* A physical monitor comes first: probe without force/override so the real
   * EDID (DDC) and link drive the modes.  Only when the connector reports
   * disconnected fall back to the igt-style EDID override + force, which is
   * what makes a headless boot exercise KMS through DM's emulated sink. */
  dev = linuxkpi_drm_find_dev("amdgpu");
  if (dev)
    aconnector = p6d_find_connector(dev);
  if (!aconnector) {
    klog_puts("[SKIP] LinuxKPI: dcn no amdgpu connector to force\n");
    goto out;
  }

  mutex_lock(&dev->mode_config.mutex);
  aconnector->force = DRM_FORCE_UNSPECIFIED;
  aconnector->funcs->fill_modes(aconnector, dev->mode_config.max_width,
                                dev->mode_config.max_height);
  mutex_unlock(&dev->mode_config.mutex);

  if (aconnector->status == connector_status_connected) {
    klogf("[  OK  ] LinuxKPI: dcn physical sink %s (card%d)\n",
          aconnector->name, card);
  } else {
    forced = 1;
    p6d_build_edid(edid);
    if (drm_edid_override_set(aconnector, edid, sizeof(edid))) {
      p6d_fail("EDID override rejected", 0);
      goto out;
    }
    mutex_lock(&dev->mode_config.mutex);
    aconnector->force = DRM_FORCE_ON;
    aconnector->funcs->fill_modes(aconnector, dev->mode_config.max_width,
                                  dev->mode_config.max_height);
    mutex_unlock(&dev->mode_config.mutex);
  }

  if (p6d_set_atomic_cap(node, &s) ||
      p6d_get_resources(node, &s, crtcs, &n_crtcs, conns, &n_conns) ||
      p6d_get_planes(node, &s, plane_ids, &plane_count)) {
    p6d_fail("resource discovery failed", 0);
    goto out;
  }
  if (p6d_pick_connector(node, &s, conns, n_conns, &conn_id, &mode,
                         &mode_count)) {
    p6d_fail(forced ? "forced connector has no modes"
                    : "physical connector has no modes", 0);
    goto out;
  }
  /* Primary plane: immutable "type" == 1; cursor plane: == 2.  DM gives
   * every primary plane its own pipe (possible_crtcs = 1 << pipe_index), and
   * drm_atomic_plane_check() rejects a plane paired with a CRTC outside that
   * mask with -EINVAL, long before amdgpu_dm_atomic_check() runs.  Pick the
   * CRTC from the chosen primary plane's mask instead of assuming crtcs[0]. */
  {
    for (uint32_t i = 0; i < plane_count; i++) {
      struct p6d_prop_set ps;
      uint64_t type_val = 0;
      uint32_t possible = 0;

      if (p6d_obj_props(node, &s, plane_ids[i], DRM_MODE_OBJECT_PLANE, &ps))
        continue;
      if (p6d_prop_find(node, &s, &ps, "type", 0, &type_val))
        continue;
      if (type_val == 1 && !primary_plane) {
        primary_plane = plane_ids[i];
        if (!p6d_get_plane(node, &s, plane_ids[i], &possible) && possible)
          crtc_index = (uint32_t)__builtin_ctz(possible);
        break;
      }
    }
    if (!primary_plane) {
      p6d_fail("no primary plane found", 0);
      goto out;
    }
    for (uint32_t i = 0; i < plane_count; i++) {
      struct p6d_prop_set ps;
      uint64_t type_val = 0;
      uint32_t possible = 0;

      if (p6d_obj_props(node, &s, plane_ids[i], DRM_MODE_OBJECT_PLANE, &ps))
        continue;
      if (p6d_prop_find(node, &s, &ps, "type", 0, &type_val) || type_val != 2)
        continue;
      if (!p6d_get_plane(node, &s, plane_ids[i], &possible) &&
          !(possible & (1u << crtc_index)))
        continue;
      cursor_plane = plane_ids[i];
      break;
    }
    if (crtc_index >= n_crtcs)
      crtc_index = 0;
    crtc_id = crtcs[crtc_index];
  }

  if (forced)
    klogf("[  OK  ] LinuxKPI: dcn forced %s (card%d, EDID override), "
          "connector=%u crtc=%u modes=%u (%ux%u)\n",
          aconnector->name, card, conn_id, crtc_id, mode_count, mode.hdisplay,
          mode.vdisplay);
  else
    klogf("[  OK  ] LinuxKPI: dcn physical %s (card%d), connector=%u crtc=%u "
          "modes=%u (%ux%u)\n",
          aconnector->name, card, conn_id, crtc_id, mode_count, mode.hdisplay,
          mode.vdisplay);

  if (p6d_obj_props(node, &s, conn_id, DRM_MODE_OBJECT_CONNECTOR,
                    &conn_props) ||
      p6d_obj_props(node, &s, crtc_id, DRM_MODE_OBJECT_CRTC, &crtc_props) ||
      p6d_prop_find(node, &s, &conn_props, "CRTC_ID", &conn_crtc_id, 0) ||
      p6d_prop_find(node, &s, &crtc_props, "MODE_ID", &crtc_mode_id, 0) ||
      p6d_prop_find(node, &s, &crtc_props, "ACTIVE", &crtc_active, 0)) {
    p6d_fail("connector/CRTC property discovery failed", 0);
    goto out;
  }

  if (p6d_obj_props(node, &s, primary_plane, DRM_MODE_OBJECT_PLANE,
                    &plane_props) ||
      p6d_prop_find(node, &s, &plane_props, "FB_ID", &plane_fb_id, 0) ||
      p6d_prop_find(node, &s, &plane_props, "CRTC_ID", &plane_crtc_id, 0) ||
      p6d_prop_find(node, &s, &plane_props, "SRC_X", &p_src_x, 0) ||
      p6d_prop_find(node, &s, &plane_props, "SRC_Y", &p_src_y, 0) ||
      p6d_prop_find(node, &s, &plane_props, "SRC_W", &p_src_w, 0) ||
      p6d_prop_find(node, &s, &plane_props, "SRC_H", &p_src_h, 0) ||
      p6d_prop_find(node, &s, &plane_props, "CRTC_X", &p_crtc_x, 0) ||
      p6d_prop_find(node, &s, &plane_props, "CRTC_Y", &p_crtc_y, 0) ||
      p6d_prop_find(node, &s, &plane_props, "CRTC_W", &p_crtc_w, 0) ||
      p6d_prop_find(node, &s, &plane_props, "CRTC_H", &p_crtc_h, 0)) {
    p6d_fail("primary plane property discovery failed", 0);
    goto out;
  }
  if (cursor_plane &&
      p6d_obj_props(node, &s, cursor_plane, DRM_MODE_OBJECT_PLANE,
                    &cursor_props))
    cursor_plane = 0;

  klogf("[  OK  ] LinuxKPI: dcn atomic objects plane=%u cursor=%u\n",
        primary_plane, cursor_plane);

  if (p6d_dumb_create(node, &s, mode.hdisplay, mode.vdisplay, &dumb1,
                      &pitch1) ||
      p6d_dumb_create(node, &s, mode.hdisplay, mode.vdisplay, &dumb2,
                      &pitch2) ||
      p6d_add_fb2(node, &s, dumb1, mode.hdisplay, mode.vdisplay, pitch1,
                  DRM_FORMAT_XRGB8888, &fb1) ||
      p6d_add_fb2(node, &s, dumb2, mode.hdisplay, mode.vdisplay, pitch2,
                  DRM_FORMAT_XRGB8888, &fb2)) {
    p6d_fail("dumb/framebuffer creation failed", 0);
    goto out;
  }

  memset(blob, 0, sizeof(*blob));
  memcpy((char *)s.kva + P6D_OFF_BLOB, &mode, sizeof(mode));
  blob->data = P6D_VA + P6D_OFF_BLOB;
  blob->length = sizeof(mode);
  if (p6d_ioctl(node, DRM_IOCTL_MODE_CREATEPROPBLOB) || !blob->blob_id) {
    p6d_fail("MODE_CREATEPROPBLOB failed", 0);
    goto out;
  }
  blob_id = blob->blob_id;

  /* ── atomic enable (fb1) with a page-flip event ─────────────────────── */
  ab = (struct p6d_atomic_buf *)((char *)s.kva + P6D_OFF_ATOMIC);
  memset(ab, 0, sizeof(*ab));
  ab->objs[0] = conn_id;
  ab->counts[0] = 1;
  ab->props[0] = conn_crtc_id;
  ab->values[0] = crtc_id;
  ab->objs[1] = crtc_id;
  ab->counts[1] = 2;
  ab->props[1] = crtc_mode_id;
  ab->values[1] = blob_id;
  ab->props[2] = crtc_active;
  ab->values[2] = 1;
  ab->objs[2] = primary_plane;
  ab->counts[2] = 10;
  ab->props[3] = plane_fb_id;
  ab->values[3] = fb1;
  ab->props[4] = plane_crtc_id;
  ab->values[4] = crtc_id;
  ab->props[5] = p_src_x;
  ab->values[5] = 0;
  ab->props[6] = p_src_y;
  ab->values[6] = 0;
  ab->props[7] = p_src_w;
  ab->values[7] = (uint64_t)mode.hdisplay << 16;
  ab->props[8] = p_src_h;
  ab->values[8] = (uint64_t)mode.vdisplay << 16;
  ab->props[9] = p_crtc_x;
  ab->values[9] = 0;
  ab->props[10] = p_crtc_y;
  ab->values[10] = 0;
  ab->props[11] = p_crtc_w;
  ab->values[11] = mode.hdisplay;
  ab->props[12] = p_crtc_h;
  ab->values[12] = mode.vdisplay;

  ret = p6d_atomic_commit(node, &s, 3,
                          DRM_MODE_ATOMIC_ALLOW_MODESET |
                              DRM_MODE_PAGE_FLIP_EVENT);
  if (ret) {
    p6d_fail("atomic enable commit failed", ret);
    goto out;
  }
  p6d_ok("atomic enable commit (forced connector, ALLOW_MODESET)");

  {
    uint32_t seq = p6d_wait_flip_event(node, &s);

    if (seq)
      klogf("[  OK  ] LinuxKPI: dcn page-flip event 1 (seq=%u)\n", seq);
    else
      p6d_fail("page-flip event 1 missing", 0);
  }

  /* True flip: change only the primary plane's FB_ID. */
  memset(ab, 0, sizeof(*ab));
  ab->objs[0] = primary_plane;
  ab->counts[0] = 1;
  ab->props[0] = plane_fb_id;
  ab->values[0] = fb2;
  ret = p6d_atomic_commit(node, &s, 1, DRM_MODE_PAGE_FLIP_EVENT);
  if (ret) {
    p6d_fail("atomic page flip to fb2 failed", ret);
  } else {
    uint32_t seq = p6d_wait_flip_event(node, &s);

    if (seq)
      klogf("[  OK  ] LinuxKPI: dcn page-flip event 2 (seq=%u)\n", seq);
    else
      p6d_fail("page-flip event 2 missing", 0);
  }

  if (p6d_wait_vblank(node, &s, crtc_index) == 0)
    p6d_ok("WAIT_VBLANK relative 1 returned");
  else
    p6d_fail("WAIT_VBLANK failed", 0);

  /* ── cursor plane ───────────────────────────────────────────────────── */
  if (cursor_plane) {
    uint32_t c_crtc, c_fb, c_cx, c_cy, c_cw, c_ch, c_sx, c_sy, c_sw, c_sh;

    if (p6d_dumb_create(node, &s, 64, 64, &cdumb, &cpitch) ||
        p6d_add_fb2(node, &s, cdumb, 64, 64, cpitch, DRM_FORMAT_ARGB8888,
                    &cfb)) {
      p6d_fail("cursor dumb/framebuffer creation failed", 0);
    } else if (p6d_prop_find(node, &s, &cursor_props, "CRTC_ID", &c_crtc, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "FB_ID", &c_fb, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "CRTC_X", &c_cx, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "CRTC_Y", &c_cy, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "CRTC_W", &c_cw, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "CRTC_H", &c_ch, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "SRC_X", &c_sx, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "SRC_Y", &c_sy, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "SRC_W", &c_sw, 0) ||
               p6d_prop_find(node, &s, &cursor_props, "SRC_H", &c_sh, 0)) {
      p6d_fail("cursor property discovery failed", 0);
    } else {
      memset(ab, 0, sizeof(*ab));
      ab->objs[0] = cursor_plane;
      ab->counts[0] = 10;
      ab->props[0] = c_crtc;
      ab->values[0] = crtc_id;
      ab->props[1] = c_fb;
      ab->values[1] = cfb;
      ab->props[2] = c_cx;
      ab->values[2] = 100;
      ab->props[3] = c_cy;
      ab->values[3] = 100;
      ab->props[4] = c_cw;
      ab->values[4] = 64;
      ab->props[5] = c_ch;
      ab->values[5] = 64;
      ab->props[6] = c_sx;
      ab->values[6] = 0;
      ab->props[7] = c_sy;
      ab->values[7] = 0;
      ab->props[8] = c_sw;
      ab->values[8] = 64ULL << 16;
      ab->props[9] = c_sh;
      ab->values[9] = 64ULL << 16;
      ret = p6d_atomic_commit(node, &s, 1, 0);
      if (ret)
        p6d_fail("cursor plane commit failed", ret);
      else
        p6d_ok("cursor plane commit");

      ab->counts[0] = 2;
      ab->props[0] = c_fb;
      ab->values[0] = 0;
      ab->props[1] = c_crtc;
      ab->values[1] = 0;
      ret = p6d_atomic_commit(node, &s, 1, 0);
      if (ret)
        p6d_fail("cursor plane off failed", ret);
      else
        p6d_ok("cursor plane off");
    }
  }

  /* ── atomic disable ─────────────────────────────────────────────────── */
  memset(ab, 0, sizeof(*ab));
  ab->objs[0] = conn_id;
  ab->counts[0] = 1;
  ab->props[0] = conn_crtc_id;
  ab->values[0] = 0;
  ab->objs[1] = crtc_id;
  ab->counts[1] = 2;
  ab->props[1] = crtc_mode_id;
  ab->values[1] = 0;
  ab->props[2] = crtc_active;
  ab->values[2] = 0;
  ab->objs[2] = primary_plane;
  ab->counts[2] = 2;
  ab->props[3] = plane_fb_id;
  ab->values[3] = 0;
  ab->props[4] = plane_crtc_id;
  ab->values[4] = 0;
  ret = p6d_atomic_commit(node, &s, 3, DRM_MODE_ATOMIC_ALLOW_MODESET);
  if (ret)
    p6d_fail("atomic disable commit failed", ret);
  else
    p6d_ok("atomic disable commit");

out:
  if (cfb)
    p6d_rmfb(node, &s, cfb);
  if (fb2)
    p6d_rmfb(node, &s, fb2);
  if (fb1)
    p6d_rmfb(node, &s, fb1);
  if (blob_id)
    p6d_destroy_blob(node, &s, blob_id);
  if (cdumb)
    p6d_dumb_destroy(node, &s, cdumb);
  if (dumb2)
    p6d_dumb_destroy(node, &s, dumb2);
  if (dumb1)
    p6d_dumb_destroy(node, &s, dumb1);
  if (aconnector) {
    aconnector->force = DRM_FORCE_UNSPECIFIED;
    drm_edid_override_reset(aconnector);
  }

  asc_vfs_kernel_close(node);
  p6d_scratch_free(&s);

  if (!p6d_failures)
    p6d_ok("suite complete");
  else
    klogf("[FAIL] LinuxKPI: dcn suite had %d failure(s)\n", p6d_failures);
}
