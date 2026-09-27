#ifndef FS_EXT4_HTREE_H
#define FS_EXT4_HTREE_H

#include "ext4.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define EXT4_HTREE_HASH_LEGACY              0
#define EXT4_HTREE_HASH_HALF_MD4            1
#define EXT4_HTREE_HASH_TEA                 2
#define EXT4_HTREE_HASH_LEGACY_UNSIGNED     3
#define EXT4_HTREE_HASH_HALF_MD4_UNSIGNED   4
#define EXT4_HTREE_HASH_TEA_UNSIGNED        5

typedef struct {
    uint32_t zero;
    uint8_t  hash_version;
    uint8_t  info_length;
    uint8_t  indirect_levels;
    uint8_t  unused_flags;
} __attribute__((packed)) ext4_dx_root_info_t;

typedef struct {
    uint16_t limit;
    uint16_t count;
} __attribute__((packed)) ext4_dx_count_limit_t;

typedef struct {
    uint32_t hash;
    uint32_t block;
} __attribute__((packed)) ext4_dx_entry_t;

typedef struct {
    uint32_t hash;
    uint32_t inode;
    uint16_t size;
    uint8_t  name_len;
    uint8_t  type;
    char     name[256];
} ext4_dx_item_t;

static inline uint32_t ext4_dirent_min_size(uint32_t name_length) {
    return (8u + name_length + 3u) & ~3u;
}

int ext4_htree_hash(ext2_mount_t *mount, uint8_t version,
                    const char *name, int name_length,
                    uint32_t *result_hash);

int ext4_htree_find_entry(ext2_mount_t *mount, ext2_inode_t *dir_inode,
                         const char *name, uint32_t name_length);

int ext4_htree_add_entry(ext2_mount_t *mount, uint32_t dir_inode_num,
                        ext2_inode_t *dir_inode, uint32_t child_inode_num,
                        const char *name, uint8_t file_type);

#endif /* FS_EXT4_HTREE_H */
