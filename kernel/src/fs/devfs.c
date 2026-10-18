#include "devfs.h"
#include "../console/klog.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"

// Device Node Registry
// Keeps track of character device nodes so they persist across VFS lookups.

#define MAX_DEVICES 256

typedef struct {
  char name[64];
  vfs_node_t *node;
} device_entry_t;

static device_entry_t device_registry[MAX_DEVICES];
static int device_count = 0;

void devfs_register_node(const char *name, vfs_node_t *node) {
  if (!node)
    return;

  // Mark node as persistent so it doesn't get kfree'd during path resolution.
  node->flags |= FS_PERSISTENT;

  // Update existing entry if the name is already registered.
  for (int i = 0; i < device_count; i++) {
    if (strcmp(device_registry[i].name, name) == 0) {
      device_registry[i].node = node;
      goto mount_vfs;
    }
  }

  // Add new entry.
  if (device_count >= MAX_DEVICES)
    return;
  strncpy(device_registry[device_count].name, name, 63);
  device_registry[device_count].name[63] = '\0';
  device_registry[device_count].node = node;
  device_count++;

mount_vfs:
  // Also mount it in /dev so it appears in directory listings.
  if (fs_root) {
    vfs_node_t *dev_dir = vfs_resolve_path("/dev");
    if (dev_dir) {
      klog_puts("[VFS] Registering '");
      klog_puts((char *)name);
      klog_puts("' in /dev\n");
      ramfs_mount_node(dev_dir, node);
      vfs_close(dev_dir);
    } else {
      klog_puts("[VFS] Warning: /dev not found during registration of '");
      klog_puts((char *)name);
      klog_puts("'\n");
    }
  }
}

vfs_node_t *devfs_lookup(const char *name) {
  for (int i = 0; i < device_count; i++) {
    if (strcmp(device_registry[i].name, name) == 0) {
      return device_registry[i].node;
    }
  }
  return NULL;
}

// Character device helper
// Character devices are always created as virtual in-memory nodes, not
// persisted to ext2.

void devfs_setup_chardev(
    vfs_node_t *dir, const char *name,
    uint32_t (*read_fn)(struct vfs_node *, uint32_t, uint32_t, uint8_t *),
    uint32_t (*write_fn)(struct vfs_node *, uint32_t, uint32_t, uint8_t *),
    void (*open_fn)(struct vfs_node *), void (*close_fn)(struct vfs_node *),
    int (*poll_fn)(struct vfs_node *, int),
    int (*ioctl_fn)(struct vfs_node *, uint32_t, uint64_t),
    uint64_t (*mmap_fn)(struct vfs_node *, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t),
    void *device, uint32_t length, uint32_t rdev) {

  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;

  vfs_node_init(node);
  strncpy(node->name, name, 127);
  node->flags = FS_CHARDEV | FS_PERSISTENT;
  node->mask = 0666;
  node->length = length;
  node->device = device;
  node->read = read_fn;
  node->write = write_fn;
  node->open = open_fn;
  node->close = close_fn;
  node->poll = poll_fn;
  node->ioctl = ioctl_fn;
  node->mmap = mmap_fn;
  node->inode = rdev;

  // Register in device registry for persistent lookups.
  devfs_register_node(name, node);

  // Also mount it in the VFS directory so it appears in ls.
  if (dir) {
    ramfs_mount_node(dir, node);
  }
}

// /dev/kmsg: accept kernel-log writes, serve the klog ring on read.
// Writes are "<prio>text" on Linux; AvoryOS logs the payload verbatim and
// returns size so writev() never sees a zero-write -> ENOSPC.
static uint32_t kmsg_write(struct vfs_node *node, uint32_t offset,
                           uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (!buffer || !size)
    return 0;
  for (uint32_t i = 0; i < size; i++)
    klog_putchar((char)buffer[i]);
  return size;
}

static uint32_t kmsg_read(struct vfs_node *node, uint32_t offset,
                          uint32_t size, uint8_t *buffer) {
  (void)node;
  if (!buffer || !size)
    return 0;
  return klog_ring_read(offset, size, buffer);
}

void devfs_init(void) {
  if (!fs_root)
    return;
  vfs_node_t *dev_dir = vfs_resolve_path("/dev");
  if (!dev_dir)
    return;

  // Pre-populate standard /dev symlinks
  vfs_symlink(dev_dir, "fd", "/proc/self/fd");
  vfs_symlink(dev_dir, "stdin", "/proc/self/fd/0");
  vfs_symlink(dev_dir, "stdout", "/proc/self/fd/1");
  vfs_symlink(dev_dir, "stderr", "/proc/self/fd/2");

  // Pre-populate /dev/shm for POSIX shared memory and Chromium/Electron
  vfs_node_t *shm = vfs_finddir(dev_dir, "shm");
  if (!shm) {
    vfs_mkdir(dev_dir, "shm", 01777);
  } else {
    vfs_close(shm);
  }

  // /dev/kmsg: Linux accepts writes (e.g. "<6>message") into the kernel
  // ring buffer. Without a write handler vfs_write() returns 0 and writev()
  // reports "[WRITEV] zero write ... node=kmsg" + ENOSPC. Pre-register so
  // userspace mknod gets EEXIST and open/write hits this node.
  devfs_setup_chardev(dev_dir, "kmsg", kmsg_read, kmsg_write, NULL, NULL,
                      NULL, NULL, NULL, NULL, 0, 0);

  vfs_close(dev_dir);
}
