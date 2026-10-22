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
#include <linux/vmalloc.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_vfs.h>
#include <linuxkpi/probe_diag.h>

static unsigned long firmware_live_headers, firmware_live_buffers, firmware_live_bytes;
static unsigned long firmware_load_count;
unsigned long linuxkpi_firmware_load_count(void) {
  return __atomic_load_n(&firmware_load_count, __ATOMIC_ACQUIRE);
}

void linuxkpi_firmware_usage(struct linuxkpi_firmware_usage *usage) {
  usage->headers = __atomic_load_n(&firmware_live_headers, __ATOMIC_ACQUIRE);
  usage->buffers = __atomic_load_n(&firmware_live_buffers, __ATOMIC_ACQUIRE);
  usage->bytes = __atomic_load_n(&firmware_live_bytes, __ATOMIC_ACQUIRE);
}

static void firmware_free_data(void *data, size_t size) {
  if (!data)
    return;
  memset(data, 0, size);
  kvfree(data);
  __atomic_sub_fetch(&firmware_live_buffers, 1, __ATOMIC_RELEASE);
  __atomic_sub_fetch(&firmware_live_bytes, size, __ATOMIC_RELEASE);
}

static void firmware_free_header(struct firmware *firmware) {
  if (!firmware)
    return;
  kfree(firmware);
  __atomic_sub_fetch(&firmware_live_headers, 1, __ATOMIC_RELEASE);
}

#define LINUX_FIRMWARE_PREFIX "/lib/firmware/"
#define LINUX_FIRMWARE_NAME_MAX 192u
/* AD106's pinned GSP image is 38,061,600 bytes. Keep a bounded CPU file
 * buffer; imported drivers allocate their DMA buffers separately. */
#define LINUX_FIRMWARE_SIZE_MAX (64u * 1024u * 1024u)
#define LINUX_FIRMWARE_READ_CHUNK (1024u * 1024u)

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

static int kpi_request_firmware(const struct firmware **firmware_p, const char *name,
                                struct device *device, bool warn) {
  struct firmware *firmware;
  unsigned char *data;
  char path[sizeof(LINUX_FIRMWARE_PREFIX) + LINUX_FIRMWARE_NAME_MAX];
  size_t prefix_length = sizeof(LINUX_FIRMWARE_PREFIX) - 1;
  size_t name_length = 0;
  unsigned int size, read, offset;
  unsigned long long file_size;
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
    if (warn)
      klogf("[INFO] LinuxKPI: firmware '%s' not found\n", name);
    return -ENOENT;
  }

  file_size = asc_vfs_kernel_size64(node);
  if (!file_size) {
    asc_vfs_kernel_close(node);
    return -EINVAL;
  }
  if (file_size > LINUX_FIRMWARE_SIZE_MAX) {
    asc_vfs_kernel_close(node);
    return -EFBIG;
  }
  size = (unsigned int)file_size;

  firmware = kzalloc(sizeof(*firmware), GFP_KERNEL);
  if (firmware)
    __atomic_add_fetch(&firmware_live_headers, 1, __ATOMIC_RELEASE);
  data = size > LINUX_FIRMWARE_READ_CHUNK ? vmalloc(size) : kmalloc(size, GFP_KERNEL);
  if (data) {
    __atomic_add_fetch(&firmware_live_buffers, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&firmware_live_bytes, size, __ATOMIC_RELEASE);
  }
  if (!firmware || !data) {
    firmware_free_data(data, size);
    firmware_free_header(firmware);
    asc_vfs_kernel_close(node);
    return -ENOMEM;
  }

  for (offset = 0; offset < size; offset += read) {
    unsigned int count = min(size - offset, LINUX_FIRMWARE_READ_CHUNK);
    read = asc_vfs_kernel_read(node, offset, count, data + offset);
    if (!read || read > count)
      break;
  }
  asc_vfs_kernel_close(node);
  if (offset != size) {
    firmware_free_data(data, size);
    firmware_free_header(firmware);
    return -EIO;
  }

  firmware->size = size;
  firmware->data = data;
  firmware->priv = NULL;
  *firmware_p = firmware;
  __atomic_add_fetch(&firmware_load_count, 1, __ATOMIC_RELEASE);
  klogf("[INFO] LinuxKPI: firmware '%s' loaded %u bytes\n", name, size);
  return 0;
}

int request_firmware(const struct firmware **firmware_p, const char *name,
                     struct device *device) {
  return kpi_request_firmware(firmware_p, name, device, true);
}

int firmware_request_nowarn(const struct firmware **firmware_p, const char *name,
                            struct device *device) {
  return kpi_request_firmware(firmware_p, name, device, false);
}

void release_firmware(const struct firmware *firmware) {
  if (!firmware)
    return;
  firmware_free_data((void *)firmware->data, firmware->size);
  firmware_free_header((struct firmware *)firmware);
}

/* Static kernel: there is no usermode-helper fallback, so the "direct"
 * flavor is the same synchronous VFS read as request_firmware(). */
int request_firmware_direct(const struct firmware **firmware_p,
                            const char *name, struct device *device) {
  return request_firmware(firmware_p, name, device);
}
