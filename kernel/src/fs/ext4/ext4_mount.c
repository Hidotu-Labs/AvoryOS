#include "ext4.h"
#include "ext4_extent.h"
#include "fs/ext2/ext2_internal.h"
#include "console/klog.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "arch/uaccess.h"

int ext4_check_feature_flags(const ext2_superblock_t *sb,
                             bool require_extents, bool *read_only) {
    if (!sb || !read_only)
        return -1;
    if (sb->s_log_block_size > 6) {
        klog_puts("[EXT4] Unsupported block size in superblock.\n");
        return -1;
    }

    const uint32_t supported_incompat =
        EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_RECOVER |
        EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT;
    uint32_t unknown_incompat =
        sb->s_feature_incompat & ~supported_incompat;
    if (unknown_incompat) {
        klogf("[EXT4] Refusing unsupported incompat features: 0x%x\n",
              unknown_incompat);
        return -1;
    }
    if (require_extents &&
        !(sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        klog_puts("[EXT4] Refusing filesystem without extents support.\n");
        return -1;
    }
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        uint32_t block_size = 1024u << sb->s_log_block_size;
        if (sb->s_desc_size < 64 || sb->s_desc_size > block_size ||
            (sb->s_desc_size & 7u)) {
            klog_puts("[EXT4] Invalid 64-bit group descriptor size.\n");
            return -1;
        }
    }
    if ((sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) &&
        !(sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)) {
        klog_puts("[EXT4] RECOVER is set but the journal feature is absent.\n");
        return -1;
    }

    /* Sparse superblocks are understood: primary metadata locations come
     * from the primary superblock and group descriptors, and this driver does
     * not update backup superblocks. The legacy BTREE_DIR ro-compat flag uses
     * the HTree directory format handled by ext4_htree_*. */
    const uint32_t supported_ro_compat =
        EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |
        EXT4_FEATURE_RO_COMPAT_BTREE_DIR;
    uint32_t unknown_ro_compat =
        sb->s_feature_ro_compat & ~supported_ro_compat;
    *read_only = unknown_ro_compat != 0;
    if (*read_only) {
        klogf("[EXT4] Mounting read-only for unsupported ro-compat features: "
              "0x%x\n", unknown_ro_compat);
    }
    return 0;
}

uint32_t ext4_read_impl(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (!mnt) return (uint32_t)-5;

    ext2_inode_t inode;
    if (ext2_read_inode(mnt, node->inode, &inode)) {
        static uint32_t inode_errs;
        if (__atomic_add_fetch(&inode_errs, 1, __ATOMIC_RELAXED) <= 8)
            klogf("[EXT4] inode %u read failed\n", node->inode);
        return (uint32_t)-5;
    }

    if (!ext4_inode_has_extents(&inode))
        return ext2_read_impl(node, offset, size, buffer);

    /* The VFS node size is normally the size visible to concurrent readers.
     * An extent-backed file can be extended while a page fault or readahead is
     * loading it, so never shrink the visible size from a separately-read
     * inode snapshot.  But dentry aliases can leave a VFS node behind after a
     * committed extension; in that case the on-disk inode is newer and must
     * repair the stale zero/small node length or mmap page fills fail at EOF. */
    uint32_t vnode_size = node->length;
    uint32_t file_size = vnode_size;
    if (inode.i_size > file_size) {
        file_size = inode.i_size;
        node->length = file_size;
        static uint32_t size_repairs;
        if (__atomic_add_fetch(&size_repairs, 1, __ATOMIC_RELAXED) <= 8)
            klogf("[EXT4] repaired stale node size: inode=%u name='%s' "
                  "vnode_size=%u inode_size=%u blocks=%u block_size=%u\n",
                  node->inode, node->name[0] ? node->name : "?", vnode_size,
                  inode.i_size, inode.i_blocks, mnt->block_size);
    }
    if (offset >= file_size) {
        static uint32_t eof_errs;
        if (__atomic_add_fetch(&eof_errs, 1, __ATOMIC_RELAXED) <= 8)
            klogf("[EXT4] read past EOF: inode=%u name='%s' "
                  "range=[%u,%llu) vnode_size=%u inode_size=%u "
                  "blocks=%u block_size=%u flags=0x%x\n",
                  node->inode, node->name[0] ? node->name : "?", offset,
                  (unsigned long long)offset + size, vnode_size, inode.i_size,
                  inode.i_blocks, mnt->block_size, inode.i_flags);
        return 0;
    }
    if (size > file_size - offset) size = file_size - offset;

    uint32_t bytes_read = 0;
    uint8_t *block_buf = kmalloc(mnt->block_size);
    if (!block_buf) {
        static uint32_t alloc_errs;
        if (__atomic_add_fetch(&alloc_errs, 1, __ATOMIC_RELAXED) <= 8)
            klogf("[EXT4] block buffer allocation failed\n");
        return (uint32_t)-12;
    }

    bool is_user = is_user_ptr((uint64_t)buffer);

    int error = 0;
    while (bytes_read < size) {
        uint32_t cur = offset + bytes_read;
        uint32_t lblock = cur / mnt->block_size;
        uint32_t off_in = cur % mnt->block_size;
        uint32_t run_blocks = 1;
        uint64_t disk_block = ext4_get_extent_run(mnt, &inode, lblock, &run_blocks);

        if (disk_block == 0) {
            uint32_t hole_bytes = (run_blocks * mnt->block_size) - off_in;
            if (hole_bytes > size - bytes_read)
                hole_bytes = size - bytes_read;
            unsigned long uncopied = 0;
            if (is_user)
                uncopied = clear_user(buffer + bytes_read, hole_bytes);
            else
                memset(buffer + bytes_read, 0, hole_bytes);
            bytes_read += hole_bytes - (uint32_t)uncopied;
            if (uncopied) {
                error = 14;
                break;
            }
            continue;
        }

        if (off_in != 0 || (size - bytes_read) < mnt->block_size) {
            uint32_t to_copy = mnt->block_size - off_in;
            if (to_copy > size - bytes_read) to_copy = size - bytes_read;

            if (ext2_read_block(mnt, disk_block, block_buf)) {
                error = 5;
                break;
            }
            if (is_user) {
                unsigned long uncopied = copy_to_user(buffer + bytes_read, block_buf + off_in, to_copy);
                if (uncopied > 0) {
                    error = 14;
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
                error = 5;
                break;
            }
            bytes_read += to_read_blocks * mnt->block_size;
        } else {
            uint32_t done_in_run = 0;
            while (done_in_run < to_read_blocks) {
                if (ext2_read_block(mnt, disk_block + done_in_run, block_buf)) {
                    error = 5;
                    break;
                }
                unsigned long uncopied = copy_to_user(buffer + bytes_read, block_buf, mnt->block_size);
                if (uncopied > 0) {
                    error = 14;
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
    return bytes_read ? bytes_read : (error ? (uint32_t)-error : 0);
}

static uint32_t ext4_write_kernel(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (!mnt || !buffer)
        return (uint32_t)-14;
    if (mnt->read_only)
        return (uint32_t)-30;
    if (size == 0)
        return 0;
    if (size > UINT32_MAX - offset)
        return (uint32_t)-27;

    ext2_inode_t inode;
    uint8_t *block_buf = kmalloc(mnt->block_size);
    if (!block_buf) return (uint32_t)-12;
    if (ext3_journal_start(mnt) != 0) {
        kfree(block_buf);
        return (uint32_t)-5;
    }
    /* Every inode snapshot belongs to the transaction that will publish it.
     * Taking it before journal ownership loses another writer's mappings. */
    mnt->journal.phase = "write-inode-read";
    if (ext2_read_inode(mnt, node->inode, &inode)) {
        ext3_journal_stop(mnt);
        kfree(block_buf);
        return (uint32_t)-5;
    }
    if (!ext4_inode_has_extents(&inode)) {
        ext3_journal_stop(mnt);
        kfree(block_buf);
        return ext2_write_impl(node, offset, size, buffer);
    }

    uint32_t bytes_written = 0;
    uint32_t blocks_in_batch = 0;
    int error = 0;
    bool transaction_active = true;
    bool inode_valid = true;
    while (bytes_written < size) {
        if (blocks_in_batch >= 32) {
            if (offset + bytes_written > inode.i_size)
                inode.i_size = offset + bytes_written;
            uint32_t now = ext2_current_time();
            inode.i_mtime = now;
            inode.i_ctime = now;
            mnt->journal.phase = "write-inode-update";
            if (ext2_write_inode(mnt, node->inode, &inode)) {
                error = 5;
                break;
            }
            node->length = inode.i_size;
            transaction_active = false;
            if (ext3_journal_stop(mnt) || ext3_journal_start(mnt)) {
                error = 5;
                break;
            }
            transaction_active = true;
            inode_valid = false;
            mnt->journal.phase = "write-inode-read";
            if (ext2_read_inode(mnt, node->inode, &inode)) {
                error = 5;
                break;
            }
            inode_valid = true;
            blocks_in_batch = 0;
        }

        uint32_t current = offset + bytes_written;
        uint32_t logical = current / mnt->block_size;
        uint32_t in_block = current % mnt->block_size;
        uint32_t count = mnt->block_size - in_block;
        if (count > size - bytes_written) count = size - bytes_written;

        mnt->journal.phase = "write-mapping";
        uint64_t disk_block = ext4_get_block_num(mnt, &inode, logical);
        if (!disk_block) {
            uint64_t allocated = 0;
            uint32_t remaining = size - bytes_written;
            uint32_t blocks_needed = (remaining + in_block + mnt->block_size - 1) / mnt->block_size;
            if (blocks_needed > 32) blocks_needed = 32;
            if (blocks_needed < 1) blocks_needed = 1;

            int allocation_error = ext4_alloc_extent(mnt, &inode, node->inode,
                                                      logical, blocks_needed,
                                                      &allocated);
            if (allocation_error) {
                error = allocation_error == -28 ? 28 : 5;
                klog_puts("[EXT4] extent allocation failed inode=");
                klog_uint64(node->inode);
                klog_puts(" logical=");
                klog_uint64(logical);
                klog_puts(" size=");
                klog_uint64(inode.i_size);
                klog_puts("\n");
                break;
            }
            disk_block = allocated;
            memset(block_buf, 0, mnt->block_size);
        } else if (in_block || count < mnt->block_size) {
            mnt->journal.phase = "write-data-read";
            if (ext2_read_block(mnt, disk_block, block_buf) != 0) {
                error = 5;
                break;
            }
        }

        /* The staged source is kernel memory. Batch only complete blocks
         * within one verified extent and the current transaction budget. */
        if (!in_block && count == mnt->block_size) {
            uint32_t run = 0;
            uint64_t mapped = ext4_get_extent_run(mnt, &inode, logical, &run);
            uint32_t wanted = (size - bytes_written) / mnt->block_size;
            uint32_t budget = 32 - blocks_in_batch;
            if (run > wanted) run = wanted;
            if (run > budget) run = budget;
            if (mapped != disk_block || !run) {
                error = 5;
                break;
            }
            mnt->journal.phase = "write-data-run";
            if (ext2_write_blocks(mnt, disk_block, run,
                                   buffer + bytes_written) != 0) {
                error = 5;
                break;
            }
            bytes_written += run * mnt->block_size;
            blocks_in_batch += run;
            continue;
        }

        memcpy(block_buf + in_block, buffer + bytes_written, count);
        mnt->journal.phase = "write-data";
        if (ext2_write_block(mnt, disk_block, block_buf) != 0) {
            error = 5;
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

    if (bytes_written && offset + bytes_written > inode.i_size)
        inode.i_size = offset + bytes_written;
    uint32_t now = ext2_current_time();
    inode.i_mtime = now;
    inode.i_ctime = now;
    if (transaction_active) {
        mnt->journal.phase = "write-inode-update";
        if (inode_valid && ext2_write_inode(mnt, node->inode, &inode)) error = 5;
        if (ext3_journal_stop(mnt)) error = 5;
        if (error != 5) node->length = inode.i_size;
    }
    kfree(block_buf);
    if (error == 5 || !bytes_written)
        return (uint32_t)-(error ? error : 5);
    return bytes_written;
}

uint32_t ext4_write_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
    return ext2_write_staged(node, offset, size, buffer, ext4_write_kernel);
}

int ext4_create_impl(vfs_node_t *node, char *name, uint16_t permission) {
    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (!mnt)
        return -1;
    if (mnt->read_only)
        return -30;
    if (ext3_journal_start(mnt) != 0)
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
    klog_puts(" ro_compat=0x");
    klog_uint64(mnt->base.sb.s_feature_ro_compat);
    klog_puts(mnt->base.read_only ? " (read-only)\n" : "\n");
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

    bool read_only = false;
    if (ext4_check_feature_flags(sb, true, &read_only) != 0) {
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
    base->read_only = read_only;
    base->block_size      = 1024 << base->sb.s_log_block_size;
    base->inodes_per_group = base->sb.s_inodes_per_group;
    base->inode_size      = (base->sb.s_rev_level >= 1)
                                ? base->sb.s_inode_size
                                : EXT2_GOOD_OLD_INODE_SIZE;
    base->blocks_count = ext2_sb_blocks_count(&base->sb);
    base->free_blocks_count = ext2_sb_free_blocks_count(&base->sb);
    base->reserved_blocks_count = ext2_sb_reserved_blocks_count(&base->sb);
    uint32_t sectors_per_block = base->block_size / 512;
    if (!base->sb.s_blocks_per_group ||
        base->blocks_count <= base->sb.s_first_data_block ||
        !sectors_per_block || base->block_size % 512 ||
        (dev->total_sectors &&
         base->blocks_count > dev->total_sectors / sectors_per_block)) {
        kfree(mnt);
        return NULL;
    }
    uint32_t bitmap_capacity = base->block_size * 8u;
    bool has_64bit =
        (base->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
    if (base->sb.s_blocks_per_group > bitmap_capacity ||
        !base->inodes_per_group || base->inodes_per_group > bitmap_capacity ||
        (!has_64bit && (base->sb.s_blocks_per_group > UINT16_MAX ||
                        base->inodes_per_group > UINT16_MAX))) {
        klog_puts("[EXT4] Invalid group geometry for descriptor format.\n");
        kfree(mnt);
        return NULL;
    }
    uint64_t data_blocks =
        base->blocks_count - base->sb.s_first_data_block;
    uint64_t groups_count = 1 +
        (data_blocks - 1) / base->sb.s_blocks_per_group;
    if (!groups_count || groups_count > UINT32_MAX) {
        kfree(mnt);
        return NULL;
    }
    base->groups_count = (uint32_t)groups_count;

    klog_puts("[EXT4] Mounting... INCOMPAT: ");
    if (base->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) klog_puts("extents ");
    klog_puts("\n");
    ext4_log_flags(mnt);

    klog_puts("[EXT4] Superblock validated:\n");
    klog_puts("       Block size:   "); klog_uint64(base->block_size);  klog_puts(" bytes\n");
    klog_puts("       Total blocks: "); klog_uint64(base->blocks_count); klog_puts("\n");
    klog_puts("       Total inodes: "); klog_uint64(base->sb.s_inodes_count); klog_puts("\n");
    klog_puts("       Block groups: "); klog_uint64(base->groups_count); klog_puts("\n");
    klog_puts("       Inode size:   "); klog_uint64(base->inode_size);  klog_puts(" bytes\n");

    if (ext2_load_bgdt(base) != 0) {
        klog_puts("[EXT4] Failed to read BGDT.\n");
        kfree(mnt);
        return NULL;
    }

    if (ext3_init_journal(base) != 0) {
        klog_puts("[EXT4] Journal setup/recovery failed; refusing mount.\n");
        ext2_free_bgdt(base);
        kfree(mnt);
        return NULL;
    }
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
        ext2_free_bgdt(base);
        kfree(mnt);
        return -1;
    }

    if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
        klog_puts("[EXT4] Root inode is not a directory!\n");
        ext2_free_bgdt(base);
        kfree(mnt);
        return -1;
    }

    vfs_node_t *root_vfs = ext2_make_vfs_node(base, EXT2_ROOT_INODE, &root_inode);
    if (!root_vfs) {
        ext2_free_bgdt(base);
        kfree(mnt);
        return -1;
    }

    strcpy(root_vfs->name, "mnt");
    base->root_node = root_vfs;

    mountpoint->flags   = FS_DIRECTORY |
                          (base->read_only ? FS_READONLY : 0);
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

    ext2_register_mount(base);
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
        ext2_free_bgdt(base);
        kfree(mnt);
        return -1;
    }

    if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
        klog_puts("[EXT4] Root inode is not a directory!\n");
        ext2_free_bgdt(base);
        kfree(mnt);
        return -1;
    }

    vfs_node_t *root_vfs = ext2_make_vfs_node(base, EXT2_ROOT_INODE, &root_inode);
    if (!root_vfs) {
        ext2_free_bgdt(base);
        kfree(mnt);
        return -1;
    }

    strcpy(root_vfs->name, "/");
    base->root_node = root_vfs;
    root_vfs->statfs = ext4_statfs_impl;

    fs_root = root_vfs;

    if (vfs_mount_ex(NULL, root_vfs, dev->name, "ext4", "/") != 0)
        klog_puts("[WARN] Failed to register ext4 root mount metadata.\n");

    ext2_register_mount(base);

    klog_puts("[OK] Ext4 filesystem mounted as root (/)\n");
    return 0;
}
int ext4_mkdir_impl(vfs_node_t *node, char *name, uint16_t permission) {
    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (!mnt)
        return -1;
    if (mnt->read_only)
        return -30;
    if (ext3_journal_start(mnt) != 0)
        return -1;
    int result = ext2_mkdir_impl(node, name, permission);
    ext3_journal_stop(mnt);
    return result;
}

int ext4_truncate_impl(vfs_node_t *node, uint32_t new_len) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE || !node->device)
        return -1;

    ext2_mount_t *mnt = (ext2_mount_t *)node->device;
    if (mnt->read_only)
        return -30;
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
    buf->f_blocks = mnt->blocks_count;
    buf->f_bfree = mnt->free_blocks_count;
    buf->f_bavail =
        mnt->free_blocks_count > mnt->reserved_blocks_count
            ? mnt->free_blocks_count - mnt->reserved_blocks_count
            : 0;
    buf->f_files = mnt->sb.s_inodes_count;
    buf->f_ffree = mnt->sb.s_free_inodes_count;
    buf->f_fsid[0] = 0;
    buf->f_fsid[1] = 0;
    buf->f_namelen = 255;
    buf->f_frsize = block_size;
    buf->f_flags = mnt->read_only ? 1 : 0; /* ST_RDONLY */
    return 0;
}
