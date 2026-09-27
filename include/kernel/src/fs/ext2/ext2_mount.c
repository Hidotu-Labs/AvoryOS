#include "ext2_internal.h"

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

  ext2_mount_t *mnt = kmalloc(sizeof(ext2_mount_t));
  if (!mnt)
    return NULL;
  memset(mnt, 0, sizeof(ext2_mount_t));
  spinlock_init(&mnt->cache_lock);

  mnt->dev             = dev;
  memcpy(&mnt->sb, sb, sizeof(ext2_superblock_t));
  mnt->block_size      = 1024 << mnt->sb.s_log_block_size;
  mnt->inodes_per_group = mnt->sb.s_inodes_per_group;
  mnt->inode_size      = (mnt->sb.s_rev_level >= 1)
                             ? mnt->sb.s_inode_size
                             : EXT2_GOOD_OLD_INODE_SIZE;
  mnt->groups_count    =
      (mnt->sb.s_blocks_count + mnt->sb.s_blocks_per_group - 1) /
      mnt->sb.s_blocks_per_group;

  klog_puts("[EXT2] Superblock validated:\n");
  klog_puts("       Block size:   "); klog_uint64(mnt->block_size);  klog_puts(" bytes\n");
  klog_puts("       Total blocks: "); klog_uint64(mnt->sb.s_blocks_count); klog_puts("\n");
  klog_puts("       Total inodes: "); klog_uint64(mnt->sb.s_inodes_count); klog_puts("\n");
  klog_puts("       Block groups: "); klog_uint64(mnt->groups_count); klog_puts("\n");
  klog_puts("       Inode size:   "); klog_uint64(mnt->inode_size);  klog_puts(" bytes\n");

  uint32_t bgdt_block  = mnt->sb.s_first_data_block + 1;
  uint32_t bgdt_size   = mnt->groups_count * sizeof(ext2_bgd_t);
  uint32_t bgdt_blocks = (bgdt_size + mnt->block_size - 1) / mnt->block_size;

  mnt->bgdt = kmalloc(bgdt_blocks * mnt->block_size);
  if (!mnt->bgdt) {
    kfree(mnt);
    return NULL;
  }

  for (uint32_t i = 0; i < bgdt_blocks; i++) {
    if (ext2_read_block(mnt, bgdt_block + i,
                        (uint8_t *)mnt->bgdt + i * mnt->block_size)) {
      klog_puts("[EXT2] Failed to read BGDT.\n");
      kfree(mnt->bgdt);
      kfree(mnt);
      return NULL;
    }
  }

  klog_puts("[EXT2] Block Group Descriptor Table loaded.\n");
  return mnt;
}

static int ext2_statfs_impl(vfs_node_t *node, struct statfs_buf *buf) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  /* Free counts are maintained in memory between flushes; re-reading the
   * on-disk superblock here would resurrect stale values. */
  uint64_t block_size = (uint64_t)(1024 << mnt->sb.s_log_block_size);

  buf->f_type    = 0xEF53;
  buf->f_bsize   = block_size;
  buf->f_blocks  = (uint64_t)mnt->sb.s_blocks_count;
  buf->f_bfree   = (uint64_t)mnt->sb.s_free_blocks_count;
  buf->f_bavail  = (uint64_t)(mnt->sb.s_free_blocks_count > mnt->sb.s_r_blocks_count
                      ? mnt->sb.s_free_blocks_count - mnt->sb.s_r_blocks_count
                      : 0);
  buf->f_files   = (uint64_t)mnt->sb.s_inodes_count;
  buf->f_ffree   = (uint64_t)mnt->sb.s_free_inodes_count;
  buf->f_fsid[0] = 0;
  buf->f_fsid[1] = 0;
  buf->f_namelen = 255;
  buf->f_frsize  = (int64_t)block_size;
  buf->f_flags   = 0;
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
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
    klog_puts("[EXT2] Root inode is not a directory!\n");
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  vfs_node_t *root_vfs =
      ext2_make_vfs_node(mnt, EXT2_ROOT_INODE, &root_inode);
  if (!root_vfs) {
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  strcpy(root_vfs->name, "mnt");
  mnt->root_node = root_vfs;

  mountpoint->flags   = FS_DIRECTORY;
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

  ext3_init_journal(mnt);
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
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
    klog_puts("[EXT2] Root inode is not a directory!\n");
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  vfs_node_t *root_vfs =
      ext2_make_vfs_node(mnt, EXT2_ROOT_INODE, &root_inode);
  if (!root_vfs) {
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  strcpy(root_vfs->name, "/");
  mnt->root_node = root_vfs;
  root_vfs->statfs = ext2_statfs_impl;

  fs_root = root_vfs;

  if (vfs_mount_ex(NULL, root_vfs, dev->name, "ext2") != 0)
    klog_puts("[WARN] Failed to register ext2 root mount metadata.\n");

  ext3_init_journal(mnt);

  klog_puts("[OK] Ext2/3 filesystem mounted as root (/)\n");
  return 0;
}
