#ifndef FS_EXT4_EXTENT_H
#define FS_EXT4_EXTENT_H

#include "ext4.h"
#include <stdint.h>
#include <stdbool.h>

#define EXT4_EXT_MAGIC 0xF30A

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed)) ext4_extent_header_t;

typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed)) ext4_extent_idx_t;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed)) ext4_extent_t;

uint64_t ext4_extent_get_pblock(ext4_extent_t *ex);
bool     ext4_inode_has_extents(ext2_inode_t *inode);
uint32_t ext4_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode, uint32_t logical_block);
uint32_t ext4_get_extent_run(ext2_mount_t *mnt, ext2_inode_t *inode,
                             uint32_t logical_block, uint32_t *out_run_len);
void     ext4_extent_init_inode(ext2_inode_t *inode);
int      ext4_extent_insert(ext2_mount_t *mnt, ext2_inode_t *inode,
                            uint32_t inode_num, uint32_t logical_block,
                            uint64_t phys_block, uint16_t len);
int      ext4_alloc_extent(ext2_mount_t *mnt, ext2_inode_t *inode,
                           uint32_t inode_num, uint32_t logical_block,
                           uint32_t num_blocks, uint64_t *out_phys);
int      ext4_extent_free_all(ext2_mount_t *mnt, ext2_inode_t *inode);
int      ext4_extent_truncate(ext2_mount_t *mnt, ext2_inode_t *inode,
                              uint32_t new_len);

#endif
