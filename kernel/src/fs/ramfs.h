#ifndef FS_RAMFS_H
#define FS_RAMFS_H

#include "vfs.h"
#include "dirindex.h"
#include "../lock/spinlock.h"

/* ramfs stores its children in the shared directory index (dirindex.h):
 * an order list for readdir plus a name hash for O(1) lookup.  The types are
 * aliases so the backend code reads exactly as before - ramfs has no
 * per-directory state beyond the index itself. */
typedef vfs_child_t child_node_t;
typedef vfs_dirindex_t ramfs_dir_t;

// For files, device points to this
typedef struct {
  uint8_t *data;
  uint32_t capacity;
  uint8_t data_is_pmm;
} ramfs_file_t;

// Initialize the root ramfs and mount it to fs_root
void ramfs_init(void);

// Adds a pre-existing vfs_node_t to the root ramfs directory directly
// (Useful for mounting block devices into /dev early on)
void ramfs_mount_node(vfs_node_t *root, vfs_node_t *node);
void ramfs_mount_on(vfs_node_t *node);
void ramfs_mount_at(char *path);

// Exposed ramfs read/write for kernel-internal pipe buffers
uint32_t ramfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                    uint8_t *buffer);
uint32_t ramfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                     uint8_t *buffer);
int ramfs_truncate(vfs_node_t *node, uint32_t new_len);
int ramfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len);
void ramfs_free_file_data(ramfs_file_t *file);

#endif
