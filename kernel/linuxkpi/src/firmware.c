/* Linux firmware loader for LinuxKPI (Phase 5 C3).
 *
 * Synchronous request_firmware() over the native VFS: it reads
 * /lib/firmware/<name> through the kernel-side open/size/read/close bridge
 * (asc_vfs_kernel_*).  This is the path amdgpu's PSP/SMU/DMCUB blobs will
 * use from Phase 6c on; the disk image installs them with the
 * linux-firmware-install.sh manifest.
 *
 * Deliberate simplifications (docs/linuxkpi-gaps.md):
 *   - synchronous only: request_firmware_nowait/direct/into_buf are not
 *     implemented until a compiled driver needs them,
 *   - no firmware caching layer, no sysfs fallback, no uevents,
 *   - the `priv` field is unused (NULL). */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_vfs.h>

#define LINUX_FIRMWARE_PREFIX "/lib/firmware/"
#define LINUX_FIRMWARE_NAME_MAX 192u
#define LINUX_FIRMWARE_SIZE_MAX (16u * 1024u * 1024u)

/* Reject absolute paths, empty components, "." and ".." components and
 * backslashes, matching the old loader's validation. */
static bool firmware_name_valid(const char *name, size_t *length_out) {
  if (!name || !name[0] || name[0] == '/')
    return false;

  size_t length = 0;
  size_t component_length = 0;
  bool component_is_dotdot = true;
  while (name[length]) {
    char c = name[length];
    if (length >= LINUX_FIRMWARE_NAME_MAX || c == '\\')
      return false;
    if (c == '/') {
      if (!component_length || component_is_dotdot)
        return false;
      component_length = 0;
      component_is_dotdot = true;
    } else {
      if (component_length >= 2 || c != '.')
        component_is_dotdot = false;
      component_length++;
    }
    length++;
  }
  if (!component_length || component_is_dotdot)
    return false;
  if (length_out)
    *length_out = length;
  return true;
}

int request_firmware(const struct firmware **firmware_p, const char *name,
                     struct device *device) {
  struct firmware *firmware;
  unsigned char *data;
  char path[sizeof(LINUX_FIRMWARE_PREFIX) + LINUX_FIRMWARE_NAME_MAX];
  size_t prefix_length = sizeof(LINUX_FIRMWARE_PREFIX) - 1;
  size_t name_length = 0;
  unsigned int size, read;
  void *node;

  (void)device;
  if (!firmware_p)
    return -EINVAL;
  *firmware_p = NULL;
  if (!firmware_name_valid(name, &name_length))
    return -EINVAL;

  memcpy(path, LINUX_FIRMWARE_PREFIX, prefix_length);
  memcpy(path + prefix_length, name, name_length + 1);

  node = asc_vfs_kernel_open(path);
  if (!node) {
    /* Phase 6 C4 relies on this line to list every blob a driver asked for
     * and did not find (the amdgpu early-init requests included). */
    klogf("[INFO] LinuxKPI: firmware '%s' not found\n", name);
    return -ENOENT;
  }

  size = asc_vfs_kernel_size(node);
  if (!size) {
    asc_vfs_kernel_close(node);
    return -EINVAL;
  }
  if (size > LINUX_FIRMWARE_SIZE_MAX) {
    asc_vfs_kernel_close(node);
    return -EFBIG;
  }

  firmware = kzalloc(sizeof(*firmware), GFP_KERNEL);
  data = kmalloc(size, GFP_KERNEL);
  if (!firmware || !data) {
    if (data)
      kfree(data);
    if (firmware)
      kfree(firmware);
    asc_vfs_kernel_close(node);
    return -ENOMEM;
  }

  read = asc_vfs_kernel_read(node, 0, size, data);
  asc_vfs_kernel_close(node);
  if (read != size) {
    memset(data, 0, size);
    kfree(data);
    kfree(firmware);
    return -EIO;
  }

  firmware->size = size;
  firmware->data = data;
  firmware->priv = NULL;
  *firmware_p = firmware;
  return 0;
}

void release_firmware(const struct firmware *firmware) {
  if (!firmware)
    return;
  if (firmware->data) {
    memset((void *)firmware->data, 0, firmware->size);
    kfree((void *)firmware->data);
  }
  kfree((void *)firmware);
}

/* Static kernel: there is no usermode-helper fallback, so the "direct"
 * flavor is the same synchronous VFS read as request_firmware(). */
int request_firmware_direct(const struct firmware **firmware_p,
                            const char *name, struct device *device) {
  return request_firmware(firmware_p, name, device);
}
