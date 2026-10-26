#include "ext2_internal.h"
#include "fs/ext4/ext4.h"

ext2_mount_t *ext2_active_mounts = NULL;
spinlock_t ext2_mounts_lock = SPINLOCK_INIT;

int ext2_load_bgdt(ext2_mount_t *mnt) {
  if (!mnt || !mnt->groups_count || !mnt->block_size)
    return -1;

  bool has_64bit =
      (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
  mnt->bgdt_desc_size = has_64bit ? mnt->sb.s_desc_size : 32;
  if (mnt->bgdt_desc_size < (has_64bit ? 64 : 32) ||
      mnt->bgdt_desc_size > mnt->block_size)
    return -1;

  uint64_t raw_size = (uint64_t)mnt->groups_count * mnt->bgdt_desc_size;
  uint64_t raw_alloc = (raw_size + mnt->block_size - 1) /
                       mnt->block_size * mnt->block_size;
  uint64_t decoded_size = (uint64_t)mnt->groups_count * sizeof(ext2_bgd_t);
  if (raw_alloc > SIZE_MAX || decoded_size > SIZE_MAX ||
      raw_alloc / mnt->block_size > UINT32_MAX)
    return -1;

  mnt->bgdt_raw = kmalloc((size_t)raw_alloc);
  mnt->bgdt = kmalloc((size_t)decoded_size);
  if (!mnt->bgdt_raw || !mnt->bgdt) {
    kfree(mnt->bgdt_raw);
    kfree(mnt->bgdt);
    mnt->bgdt_raw = NULL;
    mnt->bgdt = NULL;
    return -1;
  }
  memset(mnt->bgdt, 0, (size_t)decoded_size);

  uint64_t bgdt_block = (uint64_t)mnt->sb.s_first_data_block + 1;
  uint32_t bgdt_blocks = (uint32_t)(raw_alloc / mnt->block_size);
  for (uint32_t i = 0; i < bgdt_blocks; i++) {
    if (ext2_read_block(mnt, bgdt_block + i,
                        mnt->bgdt_raw + (size_t)i * mnt->block_size) != 0) {
      kfree(mnt->bgdt_raw);
      kfree(mnt->bgdt);
      mnt->bgdt_raw = NULL;
      mnt->bgdt = NULL;
      return -1;
    }
  }

  for (uint32_t i = 0; i < mnt->groups_count; i++) {
    size_t copy_size = mnt->bgdt_desc_size;
    if (copy_size > sizeof(ext2_bgd_t))
      copy_size = sizeof(ext2_bgd_t);
    memcpy(&mnt->bgdt[i], mnt->bgdt_raw +
               (size_t)i * mnt->bgdt_desc_size, copy_size);
  }
  return 0;
}

void ext2_free_bgdt(ext2_mount_t *mnt) {
  if (!mnt)
    return;
  kfree(mnt->bgdt_raw);
  kfree(mnt->bgdt);
  mnt->bgdt_raw = NULL;
  mnt->bgdt = NULL;
}

void ext2_register_mount(ext2_mount_t *mnt) {
  if (!mnt)
    return;
  spinlock_acquire(&ext2_mounts_lock);
  mnt->next = ext2_active_mounts;
  ext2_active_mounts = mnt;
  spinlock_release(&ext2_mounts_lock);
}

void ext2_unregister_mount(ext2_mount_t *mnt) {
  if (!mnt)
    return;
  spinlock_acquire(&ext2_mounts_lock);
  ext2_mount_t **curr = &ext2_active_mounts;
  while (*curr) {
    if (*curr == mnt) {
      *curr = mnt->next;
      mnt->next = NULL;
      break;
    }
    curr = &(*curr)->next;
  }
  spinlock_release(&ext2_mounts_lock);
}

ext2_mount_t *ext2_init_mount(struct block_device *dev) {
  uint8_t sb_buf[1024];
  if (dev->read_sectors(dev, 2, 2, sb_buf)) {
    klog_puts("[EXT2] Failed to read superblock sectors.\n");
    return NULL;
  }

  ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
  if (sb->s_magic != EXT2_MAGIC) {
    klog_puts("[EXT2] Invalid magic number. Not an ext2 filesystem.\n");
    return NULL;
  }

  bool read_only = false;
  if (ext4_check_feature_flags(sb, false, &read_only) != 0)
    return NULL;

  ext2_mount_t *mnt = kmalloc(sizeof(ext2_mount_t));
  if (!mnt)
    return NULL;
  memset(mnt, 0, sizeof(ext2_mount_t));
  spinlock_init(&mnt->cache_lock);

  mnt->dev             = dev;
  memcpy(&mnt->sb, sb, sizeof(ext2_superblock_t));
  mnt->read_only       = read_only;
  mnt->block_size      = 1024 << mnt->sb.s_log_block_size;
  mnt->inodes_per_group = mnt->sb.s_inodes_per_group;
  mnt->inode_size      = (mnt->sb.s_rev_level >= 1)
                             ? mnt->sb.s_inode_size
                             : EXT2_GOOD_OLD_INODE_SIZE;
  mnt->blocks_count = ext2_sb_blocks_count(&mnt->sb);
  mnt->free_blocks_count = ext2_sb_free_blocks_count(&mnt->sb);
  mnt->reserved_blocks_count = ext2_sb_reserved_blocks_count(&mnt->sb);
  uint32_t sectors_per_block = mnt->block_size / 512;
  if (!mnt->sb.s_blocks_per_group ||
      mnt->blocks_count <= mnt->sb.s_first_data_block ||
      !sectors_per_block || mnt->block_size % 512 ||
      (dev->total_sectors &&
       mnt->blocks_count > dev->total_sectors / sectors_per_block)) {
    kfree(mnt);
    return NULL;
  }
  uint32_t bitmap_capacity = mnt->block_size * 8u;
  bool has_64bit =
      (mnt->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_64BIT) != 0;
  if (mnt->sb.s_blocks_per_group > bitmap_capacity ||
      !mnt->inodes_per_group || mnt->inodes_per_group > bitmap_capacity ||
      (!has_64bit && (mnt->sb.s_blocks_per_group > UINT16_MAX ||
                      mnt->inodes_per_group > UINT16_MAX))) {
    klog_puts("[EXT2] Invalid group geometry for descriptor format.\n");
    kfree(mnt);
    return NULL;
  }
  uint64_t data_blocks = mnt->blocks_count - mnt->sb.s_first_data_block;
  uint64_t groups_count =
      1 + (data_blocks - 1) / mnt->sb.s_blocks_per_group;
  if (!groups_count || groups_count > UINT32_MAX) {
    kfree(mnt);
    return NULL;
  }
  mnt->groups_count = (uint32_t)groups_count;

  klog_puts("[EXT2] Superblock validated:\n");
  klog_puts("       Block size:   "); klog_uint64(mnt->block_size);  klog_puts(" bytes\n");
  klog_puts("       Total blocks: "); klog_uint64(mnt->blocks_count); klog_puts("\n");
  klog_puts("       Total inodes: "); klog_uint64(mnt->sb.s_inodes_count); klog_puts("\n");
  klog_puts("       Block groups: "); klog_uint64(mnt->groups_count); klog_puts("\n");
  klog_puts("       Inode size:   "); klog_uint64(mnt->inode_size);  klog_puts(" bytes\n");

  if (ext2_load_bgdt(mnt) != 0) {
    klog_puts("[EXT2] Failed to read BGDT.\n");
    kfree(mnt);
    return NULL;
  }

  /* Replay a cleanly supported journal before exposing any inode or mount
   * callbacks. A read-only fallback never replays a dirty journal because
   * doing so would write to a filesystem mounted read-only. */
  if (ext3_init_journal(mnt) != 0) {
    klog_puts("[EXT2] Journal setup/recovery failed; refusing mount.\n");
    ext2_free_bgdt(mnt);
    kfree(mnt);
    return NULL;
  }

  klog_puts("[EXT2] Block Group Descriptor Table loaded.\n");
  return mnt;
}

int ext2_statfs_impl(vfs_node_t *node, struct statfs_buf *buf) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  /* Free counts are maintained in memory between flushes; re-reading the
   * on-disk superblock here would resurrect stale values. */
  uint64_t block_size = (uint64_t)(1024 << mnt->sb.s_log_block_size);

  buf->f_type    = 0xEF53;
  buf->f_bsize   = block_size;
  buf->f_blocks  = mnt->blocks_count;
  buf->f_bfree   = mnt->free_blocks_count;
  buf->f_bavail  = (uint64_t)(mnt->free_blocks_count > mnt->reserved_blocks_count
                      ? mnt->free_blocks_count - mnt->reserved_blocks_count
                      : 0);
  buf->f_files   = (uint64_t)mnt->sb.s_inodes_count;
  buf->f_ffree   = (uint64_t)mnt->sb.s_free_inodes_count;
  buf->f_fsid[0] = 0;
  buf->f_fsid[1] = 0;
  buf->f_namelen = 255;
  buf->f_frsize  = (int64_t)block_size;
  buf->f_flags   = mnt->read_only ? 1 : 0; /* ST_RDONLY */
  return 0;
}

int ext2_mount(struct block_device *dev, vfs_node_t *mountpoint) {
  if (!dev || !mountpoint)
    return -1;

  klog_puts("[EXT2] Probing block device '");
  klog_puts(dev->name);
  klog_puts("' for ext2 filesystem...\n");

  ext2_mount_t *mnt = ext2_init_mount(dev);
  if (!mnt)
    return -1;

  ext2_inode_t root_inode;
  if (ext2_read_inode(mnt, EXT2_ROOT_INODE, &root_inode)) {
    klog_puts("[EXT2] Failed to read root inode.\n");
    ext2_free_bgdt(mnt);
    kfree(mnt);
    return -1;
  }

  if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
    klog_puts("[EXT2] Root inode is not a directory!\n");
    ext2_free_bgdt(mnt);
    kfree(mnt);
    return -1;
  }

  vfs_node_t *root_vfs =
      ext2_make_vfs_node(mnt, EXT2_ROOT_INODE, &root_inode);
  if (!root_vfs) {
    ext2_free_bgdt(mnt);
    kfree(mnt);
    return -1;
  }

  strcpy(root_vfs->name, "mnt");
  mnt->root_node = root_vfs;

  mountpoint->flags   = FS_DIRECTORY |
                        (mnt->read_only ? FS_READONLY : 0);
  mountpoint->inode   = EXT2_ROOT_INODE;
  mountpoint->length  = root_inode.i_size;
  mountpoint->device  = mnt;
  mountpoint->readdir = ext2_readdir_impl;
  mountpoint->finddir = ext2_finddir_impl;
  mountpoint->create  = ext2_create_impl;
  mountpoint->mkdir   = ext2_mkdir_impl;
  mountpoint->unlink  = ext2_unlink_impl;
  mountpoint->rmdir   = ext2_rmdir_impl;
  mountpoint->symlink = ext2_symlink_impl;
  mountpoint->rename  = ext2_rename_impl;
  mountpoint->chmod   = ext2_chmod_impl;
  mountpoint->chown   = ext2_chown_impl;
  mountpoint->statfs  = ext2_statfs_impl;

  ext2_register_mount(mnt);
  return 0;
}

int ext2_mount_root(struct block_device *dev) {
  if (!dev)
    return -1;

  klog_puts("[EXT2] Probing block device '");
  klog_puts(dev->name);
  klog_puts("' for ext2 filesystem (as root)...\n");

  ext2_mount_t *mnt = ext2_init_mount(dev);
  if (!mnt)
    return -1;

  ext2_inode_t root_inode;
  if (ext2_read_inode(mnt, EXT2_ROOT_INODE, &root_inode)) {
    klog_puts("[EXT2] Failed to read root inode.\n");
    ext2_free_bgdt(mnt);
    kfree(mnt);
    return -1;
  }

  if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
    klog_puts("[EXT2] Root inode is not a directory!\n");
    ext2_free_bgdt(mnt);
    kfree(mnt);
    return -1;
  }

  vfs_node_t *root_vfs =
      ext2_make_vfs_node(mnt, EXT2_ROOT_INODE, &root_inode);
  if (!root_vfs) {
    ext2_free_bgdt(mnt);
    kfree(mnt);
    return -1;
  }

  strcpy(root_vfs->name, "/");
  mnt->root_node = root_vfs;
  root_vfs->statfs = ext2_statfs_impl;

  fs_root = root_vfs;

  if (vfs_mount_ex(NULL, root_vfs, dev->name, "ext2", "/") != 0)
    klog_puts("[WARN] Failed to register ext2 root mount metadata.\n");

  ext2_register_mount(mnt);

  klog_puts("[OK] Ext2/3 filesystem mounted as root (/)\n");
  return 0;
}
