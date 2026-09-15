/* Linux DRM devnode bridge for AvoryOS.
 *
 * The imported DRM core calls device_add() for each minor it registers
 * (drm_minor_register()).  The DRM class's devnode() callback names those
 * devices "dri/cardN" / "dri/renderD128"; device.c forwards matching paths
 * here, and this file registers a native devfs node whose per-open callback
 * builds a real Linux `struct file`:
 *
 *   open:  synthesize an inode carrying the minor's dev_t, allocate a file
 *          with the driver's fops (vgem_driver_fops, ...), and run its
 *          ->open (drm_open), which creates the per-open drm_file.
 *   ioctl/read/mmap/poll: the native node's callbacks forward through the
 *          Phase 2 file bridge (linuxkpi_file_*) into the same fops.
 *   close: fput() runs ->release (drm_release) and drops the inode.
 *
 * The native card0 node belongs to ascentdrm and is deliberately not
 * registered here; imported primaries start at card1. */

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <linux/anon_inodes.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xarray.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_vfs.h>

/* Defined in drm_drv.c; the DRM-internal minor xarray is not exported through
 * a public header, but drm_open()/drm_minor_acquire() are the intended users. */
extern struct xarray drm_minors_xa;

static void *linuxkpi_drm_dev_open(void *metadata) {
  struct drm_minor *minor;
  struct inode *inode;
  struct file *file;
  /* The native node stores the userspace ABI encoding (what stat reports as
   * st_rdev); the Linux inode needs the kernel dev_t for iminor(). */
  dev_t devt = new_decode_dev(asc_vfs_node_inode(metadata));
  int ret;

  /* DRM's open path only needs the inode's dev_t (drm_open -> iminor), and
   * drm_release() gets the drm_file back through the file, so an anonymous
   * inode is enough. */
  inode = alloc_anon_inode(NULL);
  if (IS_ERR(inode) || !inode)
    return NULL;
  inode->i_rdev = devt;

  minor = drm_minor_acquire(&drm_minors_xa, iminor(inode));
  if (IS_ERR(minor)) {
    klogf("[DRM] open minor %u: lookup failed (%ld)\n", iminor(inode),
          (long)PTR_ERR(minor));
    iput(inode);
    return NULL;
  }

  file = anon_inode_getfile("drm", minor->dev->driver->fops, NULL, O_RDWR);
  drm_minor_release(minor);
  if (IS_ERR_OR_NULL(file)) {
    klogf("[DRM] open minor %u: file allocation failed\n", iminor(inode));
    iput(inode);
    return NULL;
  }

  file->f_inode = inode;
  ret = (file->f_op && file->f_op->open) ? file->f_op->open(inode, file) : 0;
  if (ret) {
    klogf("[DRM] open minor %u: driver open failed (%d)\n", iminor(inode), ret);
    /* drm_open() has already undone its own state; convert the file into a
     * plain allocation so the bridge does not run ->release on it. */
    file->f_op = NULL;
    fput(file);
    return NULL;
  }

  /* anon_inode_getfile() created the native node bound to this file; it is
   * the per-open descriptor sys_open() will install. */
  return file->f_asc_node;
}

/* Find a registered DRM device by driver name.  Used by the Phase 6 DCN
 * self-test to reach the amdgpu connectors for the EDID-override + force
 * combination igt uses on headless boots; the device stays alive while the
 * driver is bound, so no minor reference is taken. */
struct drm_device *linuxkpi_drm_find_dev(const char *name) {
  struct drm_minor *minor;
  unsigned long index;

  if (!name)
    return NULL;
  xa_for_each(&drm_minors_xa, index, minor) {
    if (!minor || !minor->dev || !minor->dev->driver)
      continue;
    if (minor->dev->driver->name &&
        strcmp(minor->dev->driver->name, name) == 0)
      return minor->dev;
  }
  return NULL;
}

/* Split a class devnode path ("dri/card1") into directory and leaf name.
 * Returns 0 or -EINVAL. */
static int drm_split_devnode_path(const char *path, char *dir, size_t dirsz,
                                  char *name, size_t namesz) {
  const char *slash;
  size_t dirlen;

  if (!path || !dir || !name || dirsz == 0 || namesz == 0)
    return -22;
  slash = strchr(path, '/');
  if (!slash || slash == path || !slash[1])
    return -22;

  dirlen = (size_t)(slash - path);
  if (dirlen >= dirsz || strlen(slash + 1) >= namesz)
    return -22;
  memcpy(dir, path, dirlen);
  dir[dirlen] = '\0';
  strcpy(name, slash + 1);
  return 0;
}

int linuxkpi_drm_devnode_register(struct device *dev, const char *path) {
  char dir[32];
  char name[64];
  int ret;

  if (!dev)
    return -22;
  ret = drm_split_devnode_path(path, dir, sizeof(dir), name, sizeof(name));
  if (ret)
    return ret;

  /* Native ascentdrm owns /dev/dri/card0.  Imported DRM devices still get
   * their render node and any later primary (card1, card2, ...). */
  if (strcmp(dir, "dri") == 0 && strcmp(name, "card0") == 0)
    return 0;

  ret = asc_vfs_register_devnode_at(dir, name, (uint32_t)dev->devt,
                                    linuxkpi_drm_dev_open);
  if (ret == 0)
    klogf("[DRM] Registered /dev/%s\n", path);
  else if (ret != -17)
    klogf("[DRM] devnode /dev/%s registration failed (%d)\n", path, ret);
  return ret;
}

void linuxkpi_drm_devnode_unregister(const char *path) {
  char dir[32];
  char name[64];

  if (drm_split_devnode_path(path, dir, sizeof(dir), name, sizeof(name)))
    return;
  if (strcmp(dir, "dri") == 0 && strcmp(name, "card0") == 0)
    return;

  asc_vfs_unregister_devnode_at(dir, name);
  klogf("[DRM] Removed /dev/%s\n", path);
}
