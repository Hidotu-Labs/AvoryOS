#ifndef FS_EXT4_DIR_INDEX_H
#define FS_EXT4_DIR_INDEX_H

#include "ext4_htree.h"

static inline int ext4_dx_add_entry(
    ext2_mount_t *mnt,
    uint32_t dir_inode_num,
    ext2_inode_t *dir_inode,
    uint32_t child_inode_num,
    const char *name,
    uint8_t file_type)
{
    return ext4_htree_add_entry(
        mnt,
        dir_inode_num,
        dir_inode,
        child_inode_num,
        name,
        file_type
    );
}

static inline int ext4_dx_find_entry(
    ext2_mount_t *mnt,
    ext2_inode_t *dir_inode,
    const char *name,
    uint32_t name_length)
{
    return ext4_htree_find_entry(
        mnt,
        dir_inode,
        name,
        name_length
    );
}

#endif /* FS_EXT4_DIR_INDEX_H */
