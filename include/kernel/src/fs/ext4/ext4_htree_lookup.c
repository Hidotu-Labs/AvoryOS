#include "ext4_htree.h"
#include "fs/ext2/ext2_internal.h"
#include "lib/string.h"
#include "mm/heap.h"

static int search_leaf_block(
    ext2_mount_t *mount,
    ext2_inode_t *dir_inode,
    uint8_t *buffer,
    uint32_t logical_block,
    const char *name,
    uint32_t name_length)
{
    uint32_t phys_block = ext2_get_block_num(mount, dir_inode, logical_block);
    if (phys_block == 0 || ext2_read_block(mount, phys_block, buffer) != 0)
        return -1;

    for (uint32_t offset = 0; offset < mount->block_size;) {
        ext2_dirent_t *entry = (ext2_dirent_t *)(buffer + offset);

        if (entry->rec_len < 8 || offset + entry->rec_len > mount->block_size)
            break;

        if (entry->inode != 0 &&
            entry->name_len == name_length &&
            memcmp(entry->name, name, name_length) == 0) {
            return (int)entry->inode;
        }

        offset += entry->rec_len;
    }

    return 0;
}

int ext4_htree_find_entry(
    ext2_mount_t *mount,
    ext2_inode_t *dir_inode,
    const char *name,
    uint32_t name_length)
{
    if (!mount || !dir_inode || !name || name_length == 0 ||
        name_length > 255 || !(dir_inode->i_flags & EXT2_INDEX_FL)) {
        return -1;
    }

    uint8_t *root_buffer = kmalloc(mount->block_size);
    uint8_t *leaf_buffer = kmalloc(mount->block_size);
    if (!root_buffer || !leaf_buffer) {
        if (root_buffer) kfree(root_buffer);
        if (leaf_buffer) kfree(leaf_buffer);
        return -1;
    }

    int found_inode = -1;

    /*
     * 1. Read HTree root (logical block 0).
     */
    uint32_t root_phys = ext2_get_block_num(mount, dir_inode, 0);
    if (root_phys == 0 || ext2_read_block(mount, root_phys, root_buffer) != 0)
        goto cleanup;

    /*
     * 2. Inspect root block's first 24 bytes (contains '.' and '..').
     */
    for (uint32_t offset = 0; offset < 24;) {
        ext2_dirent_t *entry = (ext2_dirent_t *)(root_buffer + offset);
        if (entry->rec_len < 8 || offset + entry->rec_len > 24)
            break;

        if (entry->inode != 0 &&
            entry->name_len == name_length &&
            memcmp(entry->name, name, name_length) == 0) {
            found_inode = (int)entry->inode;
            goto cleanup;
        }

        offset += entry->rec_len;
    }

    /*
     * 3. Validate HTree root info and count/limit table.
     */
    ext4_dx_root_info_t *root_info =
        (ext4_dx_root_info_t *)(root_buffer + 24);

    ext4_dx_count_limit_t *count_limit =
        (ext4_dx_count_limit_t *)(root_buffer + 32);

    ext4_dx_entry_t *entries =
        (ext4_dx_entry_t *)(root_buffer + 32);

    if (root_info->zero != 0 ||
        root_info->info_length != 8 ||
        root_info->indirect_levels > 1 ||
        count_limit->count == 0 ||
        count_limit->count > count_limit->limit ||
        32u + (uint32_t)count_limit->limit * sizeof(ext4_dx_entry_t) >
            mount->block_size) {
        found_inode = -1;
        goto cleanup;
    }

    /*
     * 4. Compute directory name hash.
     */
    uint32_t name_hash;
    if (ext4_htree_hash(
            mount,
            root_info->hash_version,
            name,
            (int)name_length,
            &name_hash) != 0) {
        found_inode = -1;
        goto cleanup;
    }

    /*
     * 5. Probe the tree to reach the target leaf block.
     */
    if (root_info->indirect_levels == 0) {
        /*
         * Single-level index: root points directly to leaf blocks.
         */
        uint16_t target_index = 0;
        for (uint16_t i = 1; i < count_limit->count; i++) {
            if (name_hash < (entries[i].hash & ~1u))
                break;
            target_index = i;
        }

        uint32_t leaf_logical = entries[target_index].block & 0x0fffffffu;
        int res = search_leaf_block(
            mount,
            dir_inode,
            leaf_buffer,
            leaf_logical,
            name,
            name_length
        );

        if (res > 0) {
            found_inode = res;
            goto cleanup;
        } else if (res < 0) {
            found_inode = -1;
            goto cleanup;
        }

        /*
         * Check adjacent leaf if continuation bit is set on next entry.
         */
        if (target_index + 1 < count_limit->count &&
            ((entries[target_index + 1].hash & 1u) ||
             (entries[target_index + 1].hash & ~1u) == (name_hash & ~1u))) {
            uint32_t next_leaf = entries[target_index + 1].block & 0x0fffffffu;
            res = search_leaf_block(
                mount,
                dir_inode,
                leaf_buffer,
                next_leaf,
                name,
                name_length
            );
            if (res > 0) {
                found_inode = res;
                goto cleanup;
            } else if (res < 0) {
                found_inode = -1;
                goto cleanup;
            }
        }

        found_inode = 0; /* Confirmed not found */
    } else {
        /*
         * Two-level index: root points to intermediate node block.
         */
        uint16_t target_index = 0;
        for (uint16_t i = 1; i < count_limit->count; i++) {
            if (name_hash < (entries[i].hash & ~1u))
                break;
            target_index = i;
        }

        uint32_t node_logical = entries[target_index].block & 0x0fffffffu;
        uint32_t node_phys = ext2_get_block_num(mount, dir_inode, node_logical);
        if (node_phys == 0 || ext2_read_block(mount, node_phys, leaf_buffer) != 0) {
            found_inode = -1;
            goto cleanup;
        }

        ext4_dx_count_limit_t *node_cl =
            (ext4_dx_count_limit_t *)(leaf_buffer + 8);
        ext4_dx_entry_t *node_entries =
            (ext4_dx_entry_t *)(leaf_buffer + 8);

        if (node_cl->count == 0 ||
            node_cl->count > node_cl->limit ||
            8u + (uint32_t)node_cl->limit * sizeof(ext4_dx_entry_t) > mount->block_size) {
            found_inode = -1;
            goto cleanup;
        }

        uint16_t leaf_idx = 0;
        for (uint16_t j = 1; j < node_cl->count; j++) {
            if (name_hash < (node_entries[j].hash & ~1u))
                break;
            leaf_idx = j;
        }

        uint32_t leaf_logical = node_entries[leaf_idx].block & 0x0fffffffu;
        int res = search_leaf_block(
            mount,
            dir_inode,
            root_buffer,
            leaf_logical,
            name,
            name_length
        );

        if (res > 0) {
            found_inode = res;
            goto cleanup;
        } else if (res < 0) {
            found_inode = -1;
            goto cleanup;
        }

        if (leaf_idx + 1 < node_cl->count &&
            ((node_entries[leaf_idx + 1].hash & 1u) ||
             (node_entries[leaf_idx + 1].hash & ~1u) == (name_hash & ~1u))) {
            uint32_t next_leaf = node_entries[leaf_idx + 1].block & 0x0fffffffu;
            res = search_leaf_block(
                mount,
                dir_inode,
                root_buffer,
                next_leaf,
                name,
                name_length
            );
            if (res > 0) {
                found_inode = res;
                goto cleanup;
            } else if (res < 0) {
                found_inode = -1;
                goto cleanup;
            }
        }

        found_inode = 0; /* Confirmed not found */
    }

cleanup:
    kfree(root_buffer);
    kfree(leaf_buffer);
    return found_inode;
}
