#ifndef FS_EXFAT_H
#define FS_EXFAT_H

#include "drivers/storage/block.h"
#include "vfs.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  struct block_device *dev; // Must stay first (sys_stat.c st_dev lookup)
  uint32_t bytes_per_sector;
  uint32_t sectors_per_cluster;
  uint32_t bytes_per_cluster;
  uint32_t fat_offset;
  uint32_t fat_length;
  uint32_t cluster_heap_offset;
  uint32_t cluster_count;
  uint32_t root_cluster;
  uint64_t volume_length;
  uint32_t bitmap_cluster;
  uint64_t bitmap_length;
  vfs_node_t *root_node;
  uint32_t refs;
} exfat_mount_t;

int exfat_mount(struct block_device *dev, vfs_node_t *mountpoint);
void exfat_unmount(vfs_node_t *root);
int exfat_live_nodes(vfs_node_t *root);
int exfat_statfs_impl(vfs_node_t *node, struct statfs_buf *buf);

#endif // FS_EXFAT_H
