/* Phase 3 devnode self-test: open the imported DRM core's minors through the
 * native devfs bridge and run a DRM_IOCTL_VERSION + DRM_IOCTL_GET_CAP
 * roundtrip, then close cleanly.
 *
 * DRM's ioctl wrapper copies its argument with copy_from_user(), so the test
 * maps one scratch page in the user range of the current address space and
 * passes that user VA.  The kernel reads back through the HHDM alias of the
 * same physical page.  asc_vfs_kernel_open() runs the same per-open path
 * sys_open() uses (open_instance -> linuxkpi_drm_dev_open -> fops->open), so
 * the whole C2 bridge is exercised. */

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>
#include <linuxkpi/native_vfs.h>

/* User-range scratch VA; kernel threads share the kernel PML4, so the page is
 * unmapped again before returning. */
#define PHASE3_SCRATCH_VA 0x0000000010000000ULL

struct phase3_scratch {
  void *phys;
  void *kva;
};

static int phase3_scratch_alloc(struct phase3_scratch *s) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  s->phys = asc_pmm_alloc_pages(1);
  if (!s->phys)
    return -1;
  s->kva = (void *)((uint64_t)s->phys + asc_pmm_get_hhdm_offset());
  memset(s->kva, 0, 4096);

  if (!asc_vmm_map_page(pml4, PHASE3_SCRATCH_VA, (uint64_t)s->phys,
                        ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_USER)) {
    asc_pmm_free_pages(s->phys, 1);
    return -1;
  }
  return 0;
}

static void phase3_scratch_free(struct phase3_scratch *s) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  asc_vmm_unmap_page(pml4, PHASE3_SCRATCH_VA);
  asc_invlpg(PHASE3_SCRATCH_VA);
  asc_pmm_free_pages(s->phys, 1);
}

static int phase3_check_devnode(const char *path) {
  struct phase3_scratch scratch;
  struct drm_version *ver;
  struct drm_get_cap *cap;
  int vmajor, vminor, vpatch;
  int vname_len;
  int revents;
  void *node;
  int ret;

  node = asc_vfs_kernel_open(path);
  if (!node) {
    klogf("[SKIP] LinuxKPI: %s not present\n", path);
    return 0;
  }

  /* The per-open descriptor must carry the registered node's dev_t.  libdrm
   * identifies a DRM node from fstat(st_rdev) (drmGetDevice2 -> major/minor
   * -> /sys/dev/char/...), and Mesa's loader/GBM/EGL use that to pick the
   * DRI driver; an instance without it silently falls back to llvmpipe
   * (Phase 6 C7). */
  if (asc_vfs_node_inode(node) == 0) {
    klogf("[ FAIL ] LinuxKPI: %s per-open node lost its dev_t\n", path);
    asc_vfs_kernel_close(node);
    return -1;
  }

  /* poll(2) path: a fresh DRM file has no events queued, so drm_poll() must
   * return 0.  Running it also records the file's native poll queue so a
   * later wake_up(event_wait) can wake sys_poll() waiters. */
  revents = asc_vfs_kernel_poll(node, 0x1 /* POLLIN */ | 0x4 /* POLLOUT */);
  if (revents != 0) {
    klogf("[ FAIL ] LinuxKPI: %s poll -> %d (expected 0)\n", path, revents);
    asc_vfs_kernel_close(node);
    return -1;
  }

  if (phase3_scratch_alloc(&scratch)) {
    klogf("[ FAIL ] LinuxKPI: %s scratch page allocation failed\n", path);
    asc_vfs_kernel_close(node);
    return -1;
  }

  ver = scratch.kva;
  memset(ver, 0, sizeof(*ver));
  ret = asc_vfs_kernel_ioctl(node, DRM_IOCTL_VERSION, PHASE3_SCRATCH_VA);
  if (ret) {
    klogf("[ FAIL ] LinuxKPI: %s DRM_IOCTL_VERSION -> %d\n", path, ret);
    goto fail;
  }
  /* Copy the results out before GET_CAP reuses the scratch page. */
  vmajor = ver->version_major;
  vminor = ver->version_minor;
  vpatch = ver->version_patchlevel;
  vname_len = (int)ver->name_len;

  cap = scratch.kva;
  memset(cap, 0, sizeof(*cap));
  cap->capability = DRM_CAP_TIMESTAMP_MONOTONIC;
  ret = asc_vfs_kernel_ioctl(node, DRM_IOCTL_GET_CAP, PHASE3_SCRATCH_VA);
  if (ret || cap->value != 1) {
    klogf("[ FAIL ] LinuxKPI: %s DRM_IOCTL_GET_CAP -> %d value=%llu\n", path,
          ret, (unsigned long long)cap->value);
    goto fail;
  }

  klogf("[  OK  ] LinuxKPI: %s VERSION %d.%d.%d name_len=%d, GET_CAP ok\n",
        path, vmajor, vminor, vpatch, vname_len);

  phase3_scratch_free(&scratch);
  asc_vfs_kernel_close(node);
  return 1;

fail:
  phase3_scratch_free(&scratch);
  asc_vfs_kernel_close(node);
  return -1;
}

static void *phase3_dummy_open(void *metadata) {
  (void)metadata;
  return NULL;
}

/* device_del()'s hook is only exercised when a device is deleted; assert the
 * native registry primitive it relies on here. */
static void phase3_check_devnode_removal(void) {
  if (asc_vfs_register_devnode_at("dri", "testrm", 0, phase3_dummy_open)) {
    klog_puts("[ FAIL ] LinuxKPI: devnode register-for-removal failed\n");
    return;
  }
  if (!asc_vfs_devnode_lookup("dri", "testrm")) {
    klog_puts("[ FAIL ] LinuxKPI: devnode not found after register\n");
    asc_vfs_unregister_devnode_at("dri", "testrm");
    return;
  }
  asc_vfs_unregister_devnode_at("dri", "testrm");
  if (asc_vfs_devnode_lookup("dri", "testrm")) {
    klog_puts("[ FAIL ] LinuxKPI: devnode still present after unregister\n");
    return;
  }
  klog_puts("[  OK  ] LinuxKPI: devnode register/unregister correct\n");
}

/* Dynamic sysfs: class dirs/device dirs and real attribute show/store. */
static void phase3_check_sysfs(void) {
  static const char *paths[] = {
      "/sys/class/drm/card1",
      "/sys/class/drm/card1-Virtual-1",
  };
  char buf[64];
  void *node;
  int len;

  for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    node = asc_vfs_kernel_open(paths[i]);
    if (!node) {
      klogf("[ FAIL ] LinuxKPI: %s missing\n", paths[i]);
      return;
    }
    asc_vfs_kernel_close(node);
  }

  /* DRM's class attribute must be materialized with a working show(). */
  node = asc_vfs_kernel_open("/sys/class/drm/version");
  if (!node) {
    klog_puts("[ FAIL ] LinuxKPI: /sys/class/drm/version missing\n");
    return;
  }
  len = (int)asc_vfs_kernel_read(node, 0, sizeof(buf) - 1, (unsigned char *)buf);
  asc_vfs_kernel_close(node);
  if (len <= 0) {
    klog_puts("[ FAIL ] LinuxKPI: /sys/class/drm/version read failed\n");
    return;
  }
  buf[len] = '\0';
  klogf("[  OK  ] LinuxKPI: dynamic sysfs ok (/sys/class/drm/version = %s)",
        buf);
}

/* GEM runtime through the DRM_GEM_SHMEM helper path.  Dumb buffers are a
 * primary-node interface (the core denies them on render nodes without
 * DRM_RENDER_ALLOW), so this runs on card1/vkms; vgem's render node only
 * exposes the render surface (VERSION/GET_CAP and the fence ioctls). */
static void phase3_check_gem_dumb(const char *path, const char *label) {
  struct phase3_scratch scratch;
  struct drm_mode_create_dumb *cd;
  struct drm_mode_map_dumb *md;
  struct drm_gem_close *gc;
  uint32_t handle, pitch;
  void *node;
  int ret;

  node = asc_vfs_kernel_open(path);
  if (!node) {
    klogf("[SKIP] LinuxKPI: %s GEM test (node absent)\n", label);
    return;
  }
  if (phase3_scratch_alloc(&scratch)) {
    klogf("[ FAIL ] LinuxKPI: %s GEM scratch allocation failed\n", label);
    asc_vfs_kernel_close(node);
    return;
  }

  cd = scratch.kva;
  memset(cd, 0, sizeof(*cd));
  cd->width = 64;
  cd->height = 64;
  cd->bpp = 32;
  ret = asc_vfs_kernel_ioctl(node, DRM_IOCTL_MODE_CREATE_DUMB, PHASE3_SCRATCH_VA);
  if (ret || !cd->handle || cd->pitch < 64 * 4 || !cd->size) {
    klogf("[ FAIL ] LinuxKPI: %s CREATE_DUMB ret=%d handle=%u pitch=%u\n",
          label, ret, cd->handle, cd->pitch);
    goto out;
  }
  handle = cd->handle;
  pitch = cd->pitch;

  md = scratch.kva;
  memset(md, 0, sizeof(*md));
  md->handle = handle;
  ret = asc_vfs_kernel_ioctl(node, DRM_IOCTL_MODE_MAP_DUMB, PHASE3_SCRATCH_VA);
  if (ret || !md->offset) {
    klogf("[ FAIL ] LinuxKPI: %s MAP_DUMB ret=%d offset=%llu\n", label, ret,
          (unsigned long long)md->offset);
    goto out;
  }

  gc = scratch.kva;
  memset(gc, 0, sizeof(*gc));
  gc->handle = handle;
  ret = asc_vfs_kernel_ioctl(node, DRM_IOCTL_GEM_CLOSE, PHASE3_SCRATCH_VA);
  if (ret) {
    klogf("[ FAIL ] LinuxKPI: %s GEM_CLOSE ret=%d\n", label, ret);
    goto out;
  }

  klogf("[  OK  ] LinuxKPI: %s GEM create/map/close (handle=%u pitch=%u)\n",
        label, handle, pitch);

out:
  phase3_scratch_free(&scratch);
  asc_vfs_kernel_close(node);
}

/* vkms modeset resource discovery: GETRESOURCES + GETCONNECTOR must answer on
 * card1 (the atomic commit path itself is exercised from userland in C5). */
static void phase3_check_vkms_resources(void) {
  struct phase3_scratch scratch;
  struct drm_mode_card_res *res;
  struct drm_mode_get_connector *con;
  uint32_t *ids;
  uint32_t crtcs, connectors;
  void *node;
  int ret;

  node = asc_vfs_kernel_open("/dev/dri/card1");
  if (!node) {
    klog_puts("[SKIP] LinuxKPI: card1 resource test (node absent)\n");
    return;
  }
  if (phase3_scratch_alloc(&scratch)) {
    klog_puts("[ FAIL ] LinuxKPI: vkms resource scratch allocation failed\n");
    asc_vfs_kernel_close(node);
    return;
  }

  res = scratch.kva;
  memset(res, 0, sizeof(*res));
  ret = asc_vfs_kernel_ioctl(node, DRM_IOCTL_MODE_GETRESOURCES,
                             PHASE3_SCRATCH_VA);
  if (ret || !res->count_crtcs || !res->count_connectors) {
    klogf("[ FAIL ] LinuxKPI: vkms GETRESOURCES ret=%d crtcs=%u conns=%u\n",
          ret, res->count_crtcs, res->count_connectors);
    goto out;
  }
  crtcs = res->count_crtcs;
  connectors = res->count_connectors;

  /* Re-query with an id array (the ioctl copies the struct from user memory,
   * so publish the array pointer through the same scratch page). */
  res = scratch.kva;
  memset(res, 0, sizeof(*res));
  res->count_connectors = connectors < 8 ? connectors : 8;
  res->connector_id_ptr = PHASE3_SCRATCH_VA + 128;
  ret = asc_vfs_kernel_ioctl(node, DRM_IOCTL_MODE_GETRESOURCES,
                             PHASE3_SCRATCH_VA);
  if (ret) {
    klogf("[ FAIL ] LinuxKPI: vkms GETRESOURCES(ids) ret=%d\n", ret);
    goto out;
  }
  ids = (uint32_t *)((char *)scratch.kva + 128);

  con = scratch.kva;
  memset(con, 0, sizeof(*con));
  con->connector_id = ids[0];
  ret = asc_vfs_kernel_ioctl(node, DRM_IOCTL_MODE_GETCONNECTOR,
                             PHASE3_SCRATCH_VA);
  if (ret || !con->connector_id) {
    klogf("[ FAIL ] LinuxKPI: vkms GETCONNECTOR ret=%d id=%u\n", ret,
          con->connector_id);
    goto out;
  }

  klogf("[  OK  ] LinuxKPI: vkms resources crtcs=%u connectors=%u "
        "connector_id=%u modes=%u connection=%u\n",
        crtcs, connectors, con->connector_id, con->count_modes,
        con->connection);

out:
  phase3_scratch_free(&scratch);
  asc_vfs_kernel_close(node);
}

void linuxkpi_test_phase3_drm(void) {
  klog_puts("[LINUXKPI] Phase 3 DRM devnode self-test\n");

  phase3_check_devnode_removal();
  phase3_check_sysfs();
  phase3_check_devnode("/dev/dri/renderD128");
  /* Present once the vkms canary finishes coming up (its initcall currently
   * returns -EINVAL after registering the minor), so the suite starts
   * covering it without further changes. */
  phase3_check_devnode("/dev/dri/card1");
  phase3_check_gem_dumb("/dev/dri/card1", "vkms");
  phase3_check_vkms_resources();
}
