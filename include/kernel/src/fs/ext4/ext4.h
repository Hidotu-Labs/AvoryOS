#ifndef FS_EXT4_H
#define FS_EXT4_H

#include "fs/ext2/ext2.h"
#include <stdint.h>

#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT   0x0080
#define EXT4_EXTENTS_FL               0x00080000

typedef struct {
    ext2_mount_t base;
} ext4_mount_t;

int ext4_mount(struct block_device *dev, vfs_node_t *mountpoint);
int ext4_mount_root(struct block_device *dev);
uint32_t ext4_read_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer);
uint32_t ext4_write_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer);
int ext4_create_impl(vfs_node_t *node, char *name, uint16_t permission);
int ext4_mkdir_impl(vfs_node_t *node, char *name, uint16_t permission);
int ext4_truncate_impl(vfs_node_t *node, uint32_t new_len);
int ext4_statfs_impl(vfs_node_t *node, struct statfs_buf *buf);

#endif
