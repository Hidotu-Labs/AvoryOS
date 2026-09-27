// SPDX-License-Identifier: GPL-2.0
/* Minimal, synchronous Linux firmware-loader compatibility for kernel drivers. */

#include <linux/firmware.h>

#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINUX_FIRMWARE_PREFIX "/lib/firmware/"
#define LINUX_FIRMWARE_NAME_MAX 192u
#define LINUX_FIRMWARE_SIZE_MAX (16u * 1024u * 1024u)

#define LINUX_EINVAL 22
#define LINUX_ENOENT 2
#define LINUX_ENOMEM 12
#define LINUX_EFBIG 27
#define LINUX_EIO 5

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
                     const struct device *device) {
  (void)device;
  if (!firmware_p)
    return -LINUX_EINVAL;
  *firmware_p = NULL;

  size_t name_length = 0;
  if (!firmware_name_valid(name, &name_length))
    return -LINUX_EINVAL;

  char path[sizeof(LINUX_FIRMWARE_PREFIX) + LINUX_FIRMWARE_NAME_MAX];
  size_t prefix_length = sizeof(LINUX_FIRMWARE_PREFIX) - 1;
  memcpy(path, LINUX_FIRMWARE_PREFIX, prefix_length);
  memcpy(path + prefix_length, name, name_length + 1);

  vfs_node_t *node = vfs_resolve_path(path);
  if (!node)
    return -LINUX_ENOENT;
  if ((node->flags & FS_TYPE_MASK) != FS_FILE || !node->length) {
    vfs_close(node);
    return -LINUX_EINVAL;
  }
  if (node->length > LINUX_FIRMWARE_SIZE_MAX) {
    vfs_close(node);
    return -LINUX_EFBIG;
  }

  struct firmware *firmware = kmalloc(sizeof(*firmware));
  uint8_t *data = kmalloc(node->length);
  if (!firmware || !data) {
    if (data)
      kfree(data);
    if (firmware)
      kfree(firmware);
    vfs_close(node);
    return -LINUX_ENOMEM;
  }

  uint32_t size = node->length;
  uint32_t read = vfs_read(node, 0, size, data);
  vfs_close(node);
  if (read != size) {
    memset(data, 0, size);
    kfree(data);
    kfree(firmware);
    return -LINUX_EIO;
  }

  firmware->size = size;
  firmware->data = data;
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
