#ifndef FS_PROCFS_H
#define FS_PROCFS_H

#include <stdint.h>

struct vfs_node;

void procfs_init(void);
void procfs_release_pid_dir(uint32_t pid);
uint32_t procfs_meminfo_read(struct vfs_node *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer);
uint32_t procfs_cpuinfo_read(struct vfs_node *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer);
uint32_t procfs_partitions_read(struct vfs_node *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer);
uint32_t procfs_mounts_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer);
uint32_t procfs_uptime_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer);
uint32_t procfs_stat_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer);
uint32_t procfs_version_read(struct vfs_node *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer);
uint32_t procfs_filesystems_read(struct vfs_node *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer);

void procfs_net_init(struct vfs_node *procfs_root);
void procfs_sys_init(struct vfs_node *procfs_root);

#endif
