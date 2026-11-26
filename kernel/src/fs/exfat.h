#ifndef FS_EXFAT_H
#define FS_EXFAT_H

#include "drivers/storage/block.h"
#include "sched/wait.h"
#include "vfs.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct exfat_mount {
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
  bool writable;
  bool overwrite_only;
  bool writing;
  bool write_failed;
  bool dirty;
  unsigned orphans;
  uint32_t alloc_hint;
  uint32_t *bitmap_chain;
  uint16_t *upcase;
  struct exfat_node *nodes;
  spinlock_t nodes_lock;
  spinlock_t write_lock;
  wait_queue_t write_waiters;
  struct exfat_mount *next;
} exfat_mount_t;

int exfat_mount(struct block_device *dev, vfs_node_t *mountpoint,
                bool read_only, bool overwrite_only);
int exfat_rename_cross(vfs_node_t *old_dir, char *old_name,
                       vfs_node_t *new_dir, char *new_name);
int exfat_rename_impl(vfs_node_t *dir, char *old_name, char *new_name);
void exfat_unmount(vfs_node_t *root);
int exfat_live_nodes(vfs_node_t *root);
int exfat_statfs_impl(vfs_node_t *node, struct statfs_buf *buf);
int exfat_chmod_impl(vfs_node_t *node, uint16_t permission);
int exfat_utime_impl(vfs_node_t *node, uint32_t atime, uint32_t mtime);

#endif // FS_EXFAT_H
