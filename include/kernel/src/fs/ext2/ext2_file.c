#include "ext2_internal.h"
#include "mm/vmm.h"
#include "syscalls/syscall.h"
#include "arch/uaccess.h"

uint32_t ext2_read_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return 0;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return 0;

  if (offset >= inode.i_size)
    return 0;
  if (offset + size > inode.i_size)
    size = inode.i_size - offset;

  uint32_t bytes_read = 0;
  uint8_t *block_buf  = NULL;
  bool is_user = is_user_ptr((uint64_t)buffer);

  while (bytes_read < size) {
    uint32_t current_offset  = offset + bytes_read;
    uint32_t logical_block   = current_offset / mnt->block_size;
    uint32_t offset_in_block = current_offset % mnt->block_size;
    uint32_t to_copy         = mnt->block_size - offset_in_block;
    if (to_copy > size - bytes_read)
      to_copy = size - bytes_read;

    uint32_t disk_block = ext2_get_block_num(mnt, &inode, logical_block);
    if (disk_block == 0) {
      if (is_user)
        clear_user(buffer + bytes_read, to_copy);
      else
        memset(buffer + bytes_read, 0, to_copy);
    } else {
      // Direct zero-copy: if reading a full block directly into kernel memory, read directly into destination
      if (!is_user && offset_in_block == 0 && to_copy == mnt->block_size) {
        ext2_read_block(mnt, disk_block, buffer + bytes_read);
      } else {
        if (!block_buf) {
          block_buf = kmalloc(mnt->block_size);
          if (!block_buf)
            break;
        }
        ext2_read_block(mnt, disk_block, block_buf);
        if (is_user) {
          unsigned long uncopied = copy_to_user(buffer + bytes_read, block_buf + offset_in_block, to_copy);
          if (uncopied > 0) {
            bytes_read += (to_copy - (uint32_t)uncopied);
            break;
          }
        } else {
          memcpy(buffer + bytes_read, block_buf + offset_in_block, to_copy);
        }
      }
    }

    bytes_read += to_copy;
  }

  if (block_buf)
    kfree(block_buf);
  return bytes_read;
}

int ext2_truncate_impl(vfs_node_t *node, uint32_t new_len) {
  if (!node || node->flags != FS_FILE || !node->device)
    return -1;

  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode) != 0)
    return -1;

  inode.i_size = new_len;
  node->length = new_len;

  return ext2_write_inode(mnt, node->inode, &inode);
}

uint32_t ext2_write_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return 0;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return 0;

  uint32_t bytes_written = 0;
  uint8_t *block_buf     = kmalloc(mnt->block_size);
  if (!block_buf)
    return 0;

  while (bytes_written < size) {
    uint32_t current_offset  = offset + bytes_written;
    uint32_t logical_block   = current_offset / mnt->block_size;
    uint32_t offset_in_block = current_offset % mnt->block_size;
    uint32_t to_write        = mnt->block_size - offset_in_block;
    if (to_write > size - bytes_written)
      to_write = size - bytes_written;

    uint32_t disk_block = ext2_get_block_num(mnt, &inode, logical_block);

    if (disk_block == 0) {
      disk_block = ext2_alloc_block(mnt);
      if (disk_block == 0)
        break;
      ext2_set_block_num(mnt, &inode, logical_block, disk_block);
      inode.i_blocks += mnt->block_size / 512;
      memset(block_buf, 0, mnt->block_size);
    } else if (offset_in_block != 0 || to_write < mnt->block_size) {
      ext2_read_block(mnt, disk_block, block_buf);
    }

    if (is_user_ptr((uint64_t)buffer)) {
      unsigned long uncopied = copy_from_user(block_buf + offset_in_block, buffer + bytes_written, to_write);
      if (uncopied > 0) {
        bytes_written += (to_write - (uint32_t)uncopied);
        ext2_write_block(mnt, disk_block, block_buf);
        break;
      }
    } else {
      memcpy(block_buf + offset_in_block, buffer + bytes_written, to_write);
    }
    ext2_write_block(mnt, disk_block, block_buf);

    bytes_written += to_write;
  }

  if (offset + bytes_written > inode.i_size)
    inode.i_size = offset + bytes_written;

  uint32_t now  = ext2_current_time();
  inode.i_mtime = now;
  inode.i_ctime = now;

  ext3_journal_start(mnt);
  ext2_write_inode(mnt, node->inode, &inode);
  ext3_journal_stop(mnt);
  node->length = inode.i_size;

  kfree(block_buf);
  return bytes_written;
}

#define EXT2_MAP_FIXED 0x10

uint64_t ext2_mmap_impl(vfs_node_t *node, uint64_t addr, uint64_t length,
                        uint64_t prot, uint64_t flags, uint64_t offset) {
  (void)prot; (void)offset;

  if (length == 0)
    return (uint64_t)-1;
  if ((node->flags & FS_TYPE_MASK) != FS_FILE) {
    klog_puts("[EXT2_MMAP] Error: not a regular file\n");
    return (uint64_t)-1;
  }

  uint64_t aligned_len = (length + 4095) & ~4095ULL;
  uint64_t vaddr       = addr;
  if (!(flags & EXT2_MAP_FIXED) || vaddr == 0)
    vaddr = mm_alloc_mmap_region(aligned_len);

  if (vaddr == 0) {
    klog_puts("[EXT2_MMAP] Error: mmap region exhausted\n");
    return (uint64_t)-1;
  }

  return vaddr;
}

int ext2_readlink_impl(vfs_node_t *node, char *buf, uint32_t size) {
  if (!node || !buf || size == 0)
    return -1;

  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return -1;

  if ((inode.i_mode & 0xF000) != EXT2_S_IFLNK)
    return -1;

  uint32_t link_len = inode.i_size;
  if (link_len == 0)
    return -1;

  uint32_t copy_len = (link_len < size) ? link_len : size;
  if (inode.i_blocks == 0 && link_len <= 60) {
    memcpy(buf, (const char *)inode.i_block, copy_len);
    return (int)copy_len;
  }

  uint32_t bytes_read = ext2_read_impl(node, 0, copy_len, (uint8_t *)buf);
  return (int)bytes_read;
}

int ext2_symlink_impl(vfs_node_t *node, char *name, char *target) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  if (ext2_finddir_impl(node, name) != NULL)
    return -1;

  uint32_t new_ino = ext2_alloc_inode(mnt);
  if (!new_ino)
    return -1;

  uint32_t target_len = strlen(target);

  ext2_inode_t new_inode;
  memset(&new_inode, 0, sizeof(ext2_inode_t));
  new_inode.i_mode        = EXT2_S_IFLNK | 0777;
  new_inode.i_size        = target_len;
  new_inode.i_links_count = 1;

  uint32_t now      = ext2_current_time();
  new_inode.i_atime = now;
  new_inode.i_ctime = now;
  new_inode.i_mtime = now;

  if (target_len <= 60) {
    memcpy((char *)new_inode.i_block, target, target_len);
    new_inode.i_blocks = 0;
  } else {
    uint32_t data_block = ext2_alloc_block(mnt);
    if (!data_block)
      return -1;

    new_inode.i_block[0] = data_block;
    new_inode.i_blocks   = mnt->block_size / 512;

    uint8_t *block_buf = kcalloc(1, mnt->block_size);
    if (!block_buf)
      return -1;
    memcpy(block_buf, target, target_len);
    ext2_write_block(mnt, data_block, block_buf);
    kfree(block_buf);
  }

  if (ext2_write_inode(mnt, new_ino, &new_inode))
    return -1;

  if (ext2_add_dir_entry(mnt, node->inode, new_ino, name, EXT2_FT_SYMLINK))
    return -1;

  return 0;
}

int ext2_rename_impl(vfs_node_t *node, char *old_name, char *new_name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  vfs_node_t *src_node = ext2_finddir_impl(node, old_name);
  if (!src_node)
    return -1;

  vfs_node_t *dst_node = ext2_finddir_impl(node, new_name);
  if (dst_node) {
    if ((dst_node->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
      kfree(dst_node);
      kfree(src_node);
      return -1;
    }
    kfree(dst_node);
    if (ext2_unlink_impl(node, new_name) != 0) {
      kfree(src_node);
      return -1;
    }
  }

  uint32_t target_inode_num = src_node->inode;
  uint32_t file_type        = EXT2_FT_UNKNOWN;
  uint32_t ftype            = src_node->flags & FS_TYPE_MASK;
  if (ftype == FS_FILE)
    file_type = EXT2_FT_REG_FILE;
  else if (ftype == FS_DIRECTORY)
    file_type = EXT2_FT_DIR;
  else if (ftype == FS_SYMLINK)
    file_type = EXT2_FT_SYMLINK;

  kfree(src_node);

  if (ext2_add_dir_entry(mnt, node->inode, target_inode_num, new_name,
                         file_type))
    return -1;

  if (ext2_remove_dir_entry(mnt, node->inode, old_name))
    return -1;

  return 0;
}

int ext2_chmod_impl(vfs_node_t *node, uint16_t permission) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return -1;

  inode.i_mode  = (inode.i_mode & 0xF000) | (permission & 0x0FFF);
  inode.i_ctime = ext2_current_time();

  if (ext2_write_inode(mnt, node->inode, &inode))
    return -1;

  node->mask = permission & 0x0FFF;
  return 0;
}

int ext2_chown_impl(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return -1;

  inode.i_uid   = uid;
  inode.i_gid   = gid;
  inode.i_ctime = ext2_current_time();

  if (ext2_write_inode(mnt, node->inode, &inode))
    return -1;

  node->uid = uid;
  node->gid = gid;
  return 0;
}

int ext2_unlink_impl(vfs_node_t *node, char *name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext3_journal_start(mnt);

  vfs_node_t *target = ext2_finddir_impl(node, name);
  if (!target)
    return -1;

  uint32_t target_ino = target->inode;
  kfree(target);

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, target_ino, &inode))
    return -1;

  if ((inode.i_mode & 0xF000) == EXT2_S_IFDIR)
    return -1;

  if (ext2_remove_dir_entry(mnt, node->inode, name))
    return -1;

  inode.i_links_count--;

  if (inode.i_links_count == 0) {
    ext2_free_all_blocks(mnt, &inode);
    inode.i_dtime = ext2_current_time();
    ext2_write_inode(mnt, target_ino, &inode);
    ext2_free_inode(mnt, target_ino);
  } else {
    ext2_write_inode(mnt, target_ino, &inode);
  }

  ext3_journal_stop(mnt);
  return 0;
}

int ext2_rmdir_impl(vfs_node_t *node, char *name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return -1;

  vfs_node_t *target = ext2_finddir_impl(node, name);
  if (!target)
    return -1;

  uint32_t target_ino = target->inode;
  kfree(target);

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, target_ino, &inode))
    return -1;

  if ((inode.i_mode & 0xF000) != EXT2_S_IFDIR)
    return -1;

  if (!ext2_dir_is_empty(mnt, target_ino))
    return -1;

  /* This path used to update the bitmaps/inode and the group descriptor
   * outside any transaction.  Run it as one transaction so the removal is
   * journaled as a unit and the lazy allocator counters are flushed once by
   * ext3_journal_stop(). */
  if (ext3_journal_start(mnt) != 0)
    return -1;

  if (ext2_remove_dir_entry(mnt, node->inode, name)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  ext2_free_all_blocks(mnt, &inode);
  inode.i_links_count = 0;
  inode.i_dtime       = ext2_current_time();
  ext2_write_inode(mnt, target_ino, &inode);
  ext2_free_inode(mnt, target_ino);

  ext2_inode_t parent_inode;
  if (ext2_read_inode(mnt, node->inode, &parent_inode) == 0) {
    if (parent_inode.i_links_count > 0)
      parent_inode.i_links_count--;
    ext2_write_inode(mnt, node->inode, &parent_inode);
  }

  uint32_t group = (target_ino - 1) / mnt->inodes_per_group;
  if (mnt->bgdt[group].bg_used_dirs_count > 0)
    mnt->bgdt[group].bg_used_dirs_count--;
  ext2_mark_metadata_dirty(mnt);

  ext3_journal_stop(mnt);
  return 0;
}

int ext2_mknod_impl(vfs_node_t *node, char *name, uint16_t permission,
                    uint32_t flags, void *device) {
  if (flags != FS_SOCKET)
    return -1;

  if (ext2_create_impl(node, name, permission) < 0)
    return -1;

  vfs_node_t *new_node = ext2_finddir_impl(node, name);
  if (!new_node)
    return -1;

  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, new_node->inode, &inode) != 0) {
    kfree(new_node);
    return -1;
  }

  inode.i_mode = (inode.i_mode & 0x0FFF) | EXT2_S_IFSOCK;
  ext2_write_inode(mnt, new_node->inode, &inode);

  new_node->flags  = FS_SOCKET;
  new_node->device = device;
  kfree(new_node);
  return 0;
}