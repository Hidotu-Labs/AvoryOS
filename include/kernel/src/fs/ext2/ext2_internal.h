#ifndef FS_EXT2_INTERNAL_H
#define FS_EXT2_INTERNAL_H

#include "ext2.h"
#include "fs/ext3/ext3.h"
#include "console/klog.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "apic/lapic_timer.h"
#include <stdbool.h>

int  ext2_write_superblock(ext2_mount_t *mnt);
int  ext2_write_bgdt(ext2_mount_t *mnt);

/* Lazy metadata counters: allocators mark the mount dirty, the journal commit
 * (or an explicit flush when no transaction is active) writes bgdt + sb. */
void ext2_mark_metadata_dirty(ext2_mount_t *mnt);
void ext2_flush_metadata(ext2_mount_t *mnt);

uint32_t ext2_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                            uint32_t logical_block);
int      ext2_set_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                            uint32_t logical_block, uint32_t disk_block);

uint32_t ext2_alloc_block(ext2_mount_t *mnt);
uint32_t ext2_alloc_block_hint(ext2_mount_t *mnt, uint32_t goal);
uint32_t ext2_alloc_inode(ext2_mount_t *mnt);
int      ext2_free_block(ext2_mount_t *mnt, uint32_t block_num);
int      ext2_free_inode(ext2_mount_t *mnt, uint32_t inode_num);

void ext2_free_indirect(ext2_mount_t *mnt, uint32_t indirect_block);
void ext2_free_dindirect(ext2_mount_t *mnt, uint32_t dindirect_block);
void ext2_free_tindirect(ext2_mount_t *mnt, uint32_t tindirect_block);
void ext2_free_all_blocks(ext2_mount_t *mnt, ext2_inode_t *inode);

int          ext2_write_inode(ext2_mount_t *mnt, uint32_t inode_num,
                              const ext2_inode_t *inode);
vfs_node_t  *ext2_make_vfs_node(ext2_mount_t *mnt, uint32_t inode_num,
                                ext2_inode_t *inode);

uint32_t ext2_current_time(void);

int              ext2_add_dir_entry(ext2_mount_t *mnt, uint32_t dir_inode_num,
                                    uint32_t child_inode_num, const char *name,
                                    uint8_t file_type);
int              ext2_remove_dir_entry(ext2_mount_t *mnt, uint32_t dir_inode_num,
                                       const char *name);
bool             ext2_dir_is_empty(ext2_mount_t *mnt, uint32_t inode_num);

struct dirent   *ext2_readdir_impl(vfs_node_t *node, uint32_t index);
vfs_node_t      *ext2_finddir_impl(vfs_node_t *node, char *name);
int              ext2_create_impl(vfs_node_t *node, char *name, uint16_t permission);
int              ext2_mkdir_impl(vfs_node_t *node, char *name, uint16_t permission);

uint32_t ext2_read_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer);
uint32_t ext2_write_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer);
int      ext2_truncate_impl(vfs_node_t *node, uint32_t new_len);
uint64_t ext2_mmap_impl(vfs_node_t *node, uint64_t addr, uint64_t length,
                        uint64_t prot, uint64_t flags, uint64_t offset);

int ext2_unlink_impl(vfs_node_t *node, char *name);
int ext2_rmdir_impl(vfs_node_t *node, char *name);
int ext2_readlink_impl(vfs_node_t *node, char *buf, uint32_t size);
int ext2_symlink_impl(vfs_node_t *node, char *name, char *target);
int ext2_rename_impl(vfs_node_t *node, char *old_name, char *new_name);
int ext2_chmod_impl(vfs_node_t *node, uint16_t permission);
int ext2_chown_impl(vfs_node_t *node, uint32_t uid, uint32_t gid);
int ext2_mknod_impl(vfs_node_t *node, char *name, uint16_t permission,
                    uint32_t flags, void *device);

ext2_mount_t *ext2_init_mount(struct block_device *dev);

#endif
