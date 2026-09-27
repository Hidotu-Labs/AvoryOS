#ifndef FS_DEVFS_H
#define FS_DEVFS_H

#include "../fs/vfs.h"
#include <stdint.h>

/**
 * devfs - Device filesystem registry
 *
 * Tracks character device nodes so they persist across VFS lookups and
 * provides helpers for registering devices into /dev.
 */

// Register a device node in the registry and mount it under /dev.
void devfs_register_node(const char *name, vfs_node_t *node);

// Look up a device node by name (without the /dev/ prefix).
vfs_node_t *devfs_lookup(const char *name);

/**
 * devfs_setup_chardev() - Allocate and register a character device node.
 *
 * Creates a virtual in-memory node, registers it in the device registry, and
 * mounts it in @dir so it appears in directory listings.
 */
void devfs_setup_chardev(
    vfs_node_t *dir, const char *name,
    uint32_t (*read_fn)(struct vfs_node *, uint32_t, uint32_t, uint8_t *),
    uint32_t (*write_fn)(struct vfs_node *, uint32_t, uint32_t, uint8_t *),
    void (*open_fn)(struct vfs_node *), void (*close_fn)(struct vfs_node *),
    int (*poll_fn)(struct vfs_node *, int),
    int (*ioctl_fn)(struct vfs_node *, uint32_t, uint64_t),
    uint64_t (*mmap_fn)(struct vfs_node *, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t),
    void *device, uint32_t length, uint32_t rdev);

/**
 * devfs_init() - Register all core /dev nodes.
 *
 * Must be called after the root filesystem is mounted and /dev has been
 * created as a ramfs mount point.  Registers fb0, console, tty*, null, zero,
 * stdin/stdout/stderr, apm_bios, and PTY devices.
 */
void devfs_init(void);

#endif /* FS_DEVFS_H */
