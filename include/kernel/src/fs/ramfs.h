#ifndef FS_RAMFS_H
#define FS_RAMFS_H

#include "vfs.h"
#include "../lock/spinlock.h"

typedef struct child_node {
  struct vfs_node *node;
  struct child_node *next;
} child_node_t;

// For files, device points to this
typedef struct {
  uint8_t *data;
  uint32_t capacity;
  uint8_t data_is_pmm;
} ramfs_file_t;

// For directories, device points to this
typedef struct {
  child_node_t *children;
  child_node_t *cursor;       // sequential readdir resume point
  uint32_t cursor_index;      // child index the cursor points at
} ramfs_dir_t;

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
