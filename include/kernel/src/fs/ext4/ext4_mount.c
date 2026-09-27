#include "ext4.h"
#include "ext4_extent.h"
#include "fs/ext2/ext2_internal.h"
#include "console/klog.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "arch/uaccess.h"

uint32_t ext4_read_impl(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (!mnt) return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(mnt, node->inode, &inode)) {
        static uint32_t inode_errs;
        if (__atomic_add_fetch(&inode_errs, 1, __ATOMIC_RELAXED) <= 8)
            klogf("[EXT4] inode %u read failed\n", node->inode);
        return 0;
    }

    if (!ext4_inode_has_extents(&inode))
        return ext2_read_impl(node, offset, size, buffer);

    if (offset >= inode.i_size) {
        static uint32_t eof_errs;
        if (__atomic_add_fetch(&eof_errs, 1, __ATOMIC_RELAXED) <= 8)
            klogf("[EXT4] read past EOF: off=%u i_size=%llu\n", offset,
                  (unsigned long long)inode.i_size);
        return 0;
    }
    if (offset + size > inode.i_size) size = inode.i_size - offset;

    uint32_t bytes_read = 0;
    uint8_t *block_buf = kmalloc(mnt->block_size);
    if (!block_buf) {
        static uint32_t alloc_errs;
        if (__atomic_add_fetch(&alloc_errs, 1, __ATOMIC_RELAXED) <= 8)
            klogf("[EXT4] block buffer allocation failed\n");
        return 0;
    }

    bool is_user = is_user_ptr((uint64_t)buffer);

    while (bytes_read < size) {
        uint32_t cur = offset + bytes_read;
        uint32_t lblock = cur / mnt->block_size;
        uint32_t off_in = cur % mnt->block_size;
        uint32_t run_blocks = 1;
        uint32_t disk_block = ext4_get_extent_run(mnt, &inode, lblock, &run_blocks);

        if (disk_block == 0) {
            uint32_t hole_bytes = (run_blocks * mnt->block_size) - off_in;
            if (hole_bytes > size - bytes_read)
                hole_bytes = size - bytes_read;
            if (is_user)
                clear_user(buffer + bytes_read, hole_bytes);
            else
                memset(buffer + bytes_read, 0, hole_bytes);
            bytes_read += hole_bytes;
            continue;
        }

        if (off_in != 0 || (size - bytes_read) < mnt->block_size) {
            uint32_t to_copy = mnt->block_size - off_in;
            if (to_copy > size - bytes_read) to_copy = size - bytes_read;

            ext2_read_block(mnt, disk_block, block_buf);
            if (is_user) {
                unsigned long uncopied = copy_to_user(buffer + bytes_read, block_buf + off_in, to_copy);
                if (uncopied > 0) {
                    bytes_read += (to_copy - (uint32_t)uncopied);
                    break;
                }
            } else {
                memcpy(buffer + bytes_read, block_buf + off_in, to_copy);
            }
            bytes_read += to_copy;
            continue;
        }

        uint32_t blocks_wanted = (size - bytes_read) / mnt->block_size;
        uint32_t to_read_blocks = (run_blocks < blocks_wanted) ? run_blocks : blocks_wanted;
        if (to_read_blocks == 0) to_read_blocks = 1;

        if (!is_user) {
            uint64_t lba = (uint64_t)disk_block * (mnt->block_size / 512);
            uint32_t sectors = to_read_blocks * (mnt->block_size / 512);
            int err = mnt->dev->read_sectors(mnt->dev, lba, sectors, buffer + bytes_read);
            if (err != 0) {
                static uint32_t data_errs;
                if (__atomic_add_fetch(&data_errs, 1, __ATOMIC_RELAXED) <= 8)
                    klogf("[EXT4] data read failed: off=%u lba=%llu sectors=%u err=%d\n",
                          offset + bytes_read, (unsigned long long)lba,
                          sectors, err);
                break;
            }
            bytes_read += to_read_blocks * mnt->block_size;
        } else {
            uint32_t done_in_run = 0;
            while (done_in_run < to_read_blocks) {
                ext2_read_block(mnt, disk_block + done_in_run, block_buf);
                unsigned long uncopied = copy_to_user(buffer + bytes_read, block_buf, mnt->block_size);
                if (uncopied > 0) {
                    bytes_read += (mnt->block_size - (uint32_t)uncopied);
                    break;
                }
                bytes_read += mnt->block_size;
                done_in_run++;
            }
            if (done_in_run < to_read_blocks)
                break;
        }
    }

    kfree(block_buf);
    return bytes_read;
}

uint32_t ext4_write_impl(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (!mnt) return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(mnt, node->inode, &inode)) return 0;
    if (!ext4_inode_has_extents(&inode))
        return ext2_write_impl(node, offset, size, buffer);

    uint8_t *block_buf = kmalloc(mnt->block_size);
    if (!block_buf) return 0;
    if (ext3_journal_start(mnt) != 0) {
        kfree(block_buf);
        return 0;
    }

    uint32_t bytes_written = 0;
    uint32_t blocks_in_batch = 0;
    while (bytes_written < size) {
        if (blocks_in_batch >= 32) {
            if (offset + bytes_written > inode.i_size)
                inode.i_size = offset + bytes_written;
            uint32_t now = ext2_current_time();
            inode.i_mtime = now;
            inode.i_ctime = now;
            ext2_write_inode(mnt, node->inode, &inode);
            node->length = inode.i_size;
            ext3_journal_stop(mnt);
            if (ext3_journal_start(mnt) != 0)
                break;
            blocks_in_batch = 0;
        }

        uint32_t current = offset + bytes_written;
        uint32_t logical = current / mnt->block_size;
        uint32_t in_block = current % mnt->block_size;
        uint32_t count = mnt->block_size - in_block;
        if (count > size - bytes_written) count = size - bytes_written;

        uint32_t disk_block = ext4_get_block_num(mnt, &inode, logical);
        if (!disk_block) {
            uint64_t allocated = 0;
            uint32_t remaining = size - bytes_written;
            uint32_t blocks_needed = (remaining + in_block + mnt->block_size - 1) / mnt->block_size;
            if (blocks_needed > 32) blocks_needed = 32;
            if (blocks_needed < 1) blocks_needed = 1;

            if (ext4_alloc_extent(mnt, &inode, node->inode, logical, blocks_needed,
                                  &allocated) != 0) {
                klog_puts("[EXT4] extent allocation failed inode=");
                klog_uint64(node->inode);
                klog_puts(" logical=");
                klog_uint64(logical);
                klog_puts(" size=");
                klog_uint64(inode.i_size);
                klog_puts("\n");
                break;
            }
            disk_block = (uint32_t)allocated;
            memset(block_buf, 0, mnt->block_size);
        } else if (in_block || count < mnt->block_size) {
            if (ext2_read_block(mnt, disk_block, block_buf) != 0)
                break;
        }

        if (is_user_ptr((uint64_t)buffer)) {
            unsigned long uncopied = copy_from_user(block_buf + in_block, buffer + bytes_written, count);
            if (uncopied > 0) {
                bytes_written += (count - (uint32_t)uncopied);
                ext2_write_block(mnt, disk_block, block_buf);
                break;
            }
        } else {
            memcpy(block_buf + in_block, buffer + bytes_written, count);
        }
        if (ext2_write_block(mnt, disk_block, block_buf) != 0) {
            klog_puts("[EXT4] data write failed inode=");
            klog_uint64(node->inode);
            klog_puts(" logical=");
            klog_uint64(logical);
            klog_puts(" physical=");
            klog_uint64(disk_block);
            klog_puts("\n");
            break;
        }
        bytes_written += count;
        blocks_in_batch++;
    }

    if (offset + bytes_written > inode.i_size)
        inode.i_size = offset + bytes_written;
    uint32_t now = ext2_current_time();
    inode.i_mtime = now;
    inode.i_ctime = now;
    ext2_write_inode(mnt, node->inode, &inode);
    ext3_journal_stop(mnt);
    node->length = inode.i_size;
    kfree(block_buf);
    return bytes_written;
}

int ext4_create_impl(vfs_node_t *node, char *name, uint16_t permission) {
    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (!mnt || ext3_journal_start(mnt) != 0)
        return -1;
    int result = ext2_create_impl(node, name, permission);
    ext3_journal_stop(mnt);
    return result;
}

void ext4_log_flags(ext4_mount_t *mnt) {
    klog_puts("[EXT4] compat=0x");
    klog_uint64(mnt->base.sb.s_feature_compat);
    klog_puts(" incompat=0x");
    klog_uint64(mnt->base.sb.s_feature_incompat);
    klog_puts("\n");
}

static ext4_mount_t *ext4_init_mount(struct block_device *dev) {
    uint8_t sb_buf[1024];
    if (dev->read_sectors(dev, 2, 2, sb_buf)) {
        klog_puts("[EXT4] Failed to read superblock sectors.\n");
        return NULL;
    }

    ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) {
        klog_puts("[EXT4] Invalid magic number.\n");
        return NULL;
    }

    if (!(sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        return NULL;
    }

    ext4_mount_t *mnt = kmalloc(sizeof(ext4_mount_t));
    if (!mnt)
        return NULL;
    memset(mnt, 0, sizeof(ext4_mount_t));

    ext2_mount_t *base = &mnt->base;
    spinlock_init(&base->cache_lock);

    base->dev = dev;
    memcpy(&base->sb, sb, sizeof(ext2_superblock_t));
    base->block_size      = 1024 << base->sb.s_log_block_size;
    base->inodes_per_group = base->sb.s_inodes_per_group;
    base->inode_size      = (base->sb.s_rev_level >= 1)
                                ? base->sb.s_inode_size
                                : EXT2_GOOD_OLD_INODE_SIZE;
    base->groups_count    =
        (base->sb.s_blocks_count + base->sb.s_blocks_per_group - 1) /
        base->sb.s_blocks_per_group;

    klog_puts("[EXT4] Mounting... INCOMPAT: ");
    if (base->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) klog_puts("extents ");
    if (base->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)   klog_puts("64bit ");
    klog_puts("\n");
    ext4_log_flags(mnt);

    klog_puts("[EXT4] Superblock validated:\n");
    klog_puts("       Block size:   "); klog_uint64(base->block_size);  klog_puts(" bytes\n");
    klog_puts("       Total blocks: "); klog_uint64(base->sb.s_blocks_count); klog_puts("\n");
    klog_puts("       Total inodes: "); klog_uint64(base->sb.s_inodes_count); klog_puts("\n");
    klog_puts("       Block groups: "); klog_uint64(base->groups_count); klog_puts("\n");
    klog_puts("       Inode size:   "); klog_uint64(base->inode_size);  klog_puts(" bytes\n");

    uint32_t bgdt_block  = base->sb.s_first_data_block + 1;
    uint32_t bgdt_size   = base->groups_count * sizeof(ext2_bgd_t);
    uint32_t bgdt_blocks = (bgdt_size + base->block_size - 1) / base->block_size;

    base->bgdt = kmalloc(bgdt_blocks * base->block_size);
    if (!base->bgdt) {
        kfree(mnt);
        return NULL;
    }

    for (uint32_t i = 0; i < bgdt_blocks; i++) {
        if (ext2_read_block(base, bgdt_block + i,
                            (uint8_t *)base->bgdt + i * base->block_size)) {
            klog_puts("[EXT4] Failed to read BGDT.\n");
            kfree(base->bgdt);
            kfree(mnt);
            return NULL;
        }
    }

    ext3_init_journal(base);
    klog_puts("[EXT4] Block Group Descriptor Table loaded.\n");
    return mnt;
}

int ext4_mount(struct block_device *dev, vfs_node_t *mountpoint) {
    if (!dev || !mountpoint)
        return -1;

    klog_puts("[EXT4] Probing block device '");
    klog_puts(dev->name);
    klog_puts("' for ext4 filesystem...\n");

    ext4_mount_t *mnt = ext4_init_mount(dev);
    if (!mnt)
        return -1;

    ext2_mount_t *base = &mnt->base;

    ext2_inode_t root_inode;
    if (ext2_read_inode(base, EXT2_ROOT_INODE, &root_inode)) {
        klog_puts("[EXT4] Failed to read root inode.\n");
        kfree(base->bgdt);
        kfree(mnt);
        return -1;
    }

    if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
        klog_puts("[EXT4] Root inode is not a directory!\n");
        kfree(base->bgdt);
        kfree(mnt);
        return -1;
    }

    vfs_node_t *root_vfs = ext2_make_vfs_node(base, EXT2_ROOT_INODE, &root_inode);
    if (!root_vfs) {
        kfree(base->bgdt);
        kfree(mnt);
        return -1;
    }

    strcpy(root_vfs->name, "mnt");
    base->root_node = root_vfs;

    mountpoint->flags   = FS_DIRECTORY;
    mountpoint->inode   = EXT2_ROOT_INODE;
    mountpoint->length  = root_inode.i_size;
    mountpoint->device  = base;
    mountpoint->read    = ext4_read_impl;
    mountpoint->write   = ext4_write_impl;
    mountpoint->readdir = ext2_readdir_impl;
    mountpoint->finddir = ext2_finddir_impl;
    mountpoint->create  = ext4_create_impl;
    mountpoint->mkdir   = ext4_mkdir_impl;
    mountpoint->unlink  = ext2_unlink_impl;
    mountpoint->rmdir   = ext2_rmdir_impl;
    mountpoint->symlink = ext2_symlink_impl;
    mountpoint->rename  = ext2_rename_impl;
    mountpoint->chmod   = ext2_chmod_impl;
    mountpoint->chown   = ext2_chown_impl;
    mountpoint->statfs  = ext4_statfs_impl;

    return 0;
}

int ext4_mount_root(struct block_device *dev) {
    if (!dev)
        return -1;

    klog_puts("[EXT4] Probing block device '");
    klog_puts(dev->name);
    klog_puts("' for ext4 filesystem (as root)...\n");

    ext4_mount_t *mnt = ext4_init_mount(dev);
    if (!mnt)
        return -1;

    ext2_mount_t *base = &mnt->base;

    ext2_inode_t root_inode;
    if (ext2_read_inode(base, EXT2_ROOT_INODE, &root_inode)) {
        klog_puts("[EXT4] Failed to read root inode.\n");
        kfree(base->bgdt);
        kfree(mnt);
        return -1;
    }

    if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
        klog_puts("[EXT4] Root inode is not a directory!\n");
        kfree(base->bgdt);
        kfree(mnt);
        return -1;
    }

    vfs_node_t *root_vfs = ext2_make_vfs_node(base, EXT2_ROOT_INODE, &root_inode);
    if (!root_vfs) {
        kfree(base->bgdt);
        kfree(mnt);
        return -1;
    }

    strcpy(root_vfs->name, "/");
    base->root_node = root_vfs;
    root_vfs->statfs = ext4_statfs_impl;

    fs_root = root_vfs;

    if (vfs_mount_ex(NULL, root_vfs, dev->name, "ext4") != 0)
        klog_puts("[WARN] Failed to register ext4 root mount metadata.\n");

    klog_puts("[OK] Ext4 filesystem mounted as root (/)\n");
    return 0;
}
int ext4_mkdir_impl(vfs_node_t *node, char *name, uint16_t permission) {
    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (!mnt || ext3_journal_start(mnt) != 0)
        return -1;
    int result = ext2_mkdir_impl(node, name, permission);
    ext3_journal_stop(mnt);
    return result;
}

int ext4_truncate_impl(vfs_node_t *node, uint32_t new_len) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE || !node->device)
        return -1;

    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    ext2_inode_t inode;
    if (ext2_read_inode(mnt, node->inode, &inode) != 0)
        return -1;
    if (!ext4_inode_has_extents(&inode))
        return ext2_truncate_impl(node, new_len);
    if (ext3_journal_start(mnt) != 0)
        return -1;

    int result = ext4_extent_truncate(mnt, &inode, new_len);
    if (result == 0) {
        uint32_t now = ext2_current_time();
        inode.i_mtime = now;
        inode.i_ctime = now;
        result = ext2_write_inode(mnt, node->inode, &inode);
    }
    ext3_journal_stop(mnt);
    if (result == 0)
        node->length = new_len;
    return result;
}

int ext4_statfs_impl(vfs_node_t *node, struct statfs_buf *buf) {
    if (!node || !node->device || !buf)
        return -1;

    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    /* Free counts are maintained in memory between flushes; re-reading the
     * on-disk superblock here would resurrect stale values. */
    uint64_t block_size = 1024ULL << mnt->sb.s_log_block_size;
    buf->f_type = 0xEF53;
    buf->f_bsize = block_size;
    buf->f_blocks = mnt->sb.s_blocks_count;
    buf->f_bfree = mnt->sb.s_free_blocks_count;
    buf->f_bavail =
        mnt->sb.s_free_blocks_count > mnt->sb.s_r_blocks_count
            ? mnt->sb.s_free_blocks_count - mnt->sb.s_r_blocks_count
            : 0;
    buf->f_files = mnt->sb.s_inodes_count;
    buf->f_ffree = mnt->sb.s_free_inodes_count;
    buf->f_fsid[0] = 0;
    buf->f_fsid[1] = 0;
    buf->f_namelen = 255;
    buf->f_frsize = block_size;
    buf->f_flags = 0;
    return 0;
}
