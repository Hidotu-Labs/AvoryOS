#include "ext2_internal.h"
#include "mm/vmm.h"
#include "syscalls/syscall.h"
#include "arch/uaccess.h"
#include "sched/sched.h"

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
    if (disk_block != 0 && disk_block >= mnt->sb.s_blocks_count) {
      union { uint32_t u; char s[4]; } alias = { .u = disk_block };
      struct thread *ct = sched_get_current();
      int hit_fd = -1;
      char hit_path[64] = {0};
      if (ct && ct->files) {
        spinlock_acquire(&ct->files->lock);
        for (uint32_t fd = 0; fd < ct->files->capacity; fd++) {
          if (ct->files->fds[fd] == node) {
            hit_fd = fd;
            struct fd_path *fp = ct->files->fd_paths[fd];
            if (fp && fp->value[0]) {
              size_t n = 0;
              while (n < sizeof(hit_path)-1 && fp->value[n]) {
                hit_path[n] = fp->value[n];
                n++;
              }
              hit_path[n] = '\0';
            }
            break;
          }
        }
        spinlock_release(&ct->files->lock);
      }
      klogf("[EXT2] bad block ref: ino=%u logical=%u disk=%u ascii='%c%c%c%c'"
            " size=%u fd=%d path='%s' comm='%s' tid=%u\n",
            node->inode, logical_block, disk_block,
            alias.s[0] >= 32 && alias.s[0] < 127 ? alias.s[0] : '.',
            alias.s[1] >= 32 && alias.s[1] < 127 ? alias.s[1] : '.',
            alias.s[2] >= 32 && alias.s[2] < 127 ? alias.s[2] : '.',
            alias.s[3] >= 32 && alias.s[3] < 127 ? alias.s[3] : '.',
            inode.i_size, hit_fd, hit_path,
            (ct && ct->comm[0]) ? ct->comm : "?",
            ct ? ct->tid : 0);
      if (is_user)
        clear_user(buffer + bytes_read, to_copy);
      else
        memset(buffer + bytes_read, 0, to_copy);
    } else if (disk_block == 0) {
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
  if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE || !node->device)
    return -1;

  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode) != 0)
    return -1;

  inode.i_size = new_len;
  node->length = new_len;

  return ext2_write_inode(mnt, node->inode, &inode);
}

/* Bound lock hold time and journal descriptor usage for large writes. A write
 * of four 1 KiB blocks shares one transaction instead of four allocator
 * commits plus one inode commit. Data blocks retain their existing home-write
 * policy; this does not turn the journal into a data journal. */
#define EXT2_WRITE_BATCH_BLOCKS 32

uint32_t ext2_write_staged(vfs_node_t *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer,
                         uint32_t (*write_kernel)(vfs_node_t *, uint32_t,
                                                  uint32_t, uint8_t *)) {
  ext2_mount_t *mnt = node ? (ext2_mount_t *)node->device : NULL;
  if (!mnt || !buffer)
    return (uint32_t)-14;
  if (!size)
    return 0;
  if (size > UINT32_MAX - offset)
    return (uint32_t)-27;
  if (!is_user_ptr((uint64_t)buffer))
    return write_kernel(node, offset, size, buffer);

  /* Faults can need reclaim or file I/O. Never hold the mount-wide journal
   * while copying user memory: other writers must be able to progress.
   * Reuse bounded scratch space rather than copying/pinning the whole write. */
  uint32_t capacity = mnt->block_size * EXT2_WRITE_BATCH_BLOCKS;
  if (!mnt->block_size || capacity / EXT2_WRITE_BATCH_BLOCKS != mnt->block_size)
    return (uint32_t)-5;
  if (capacity > size)
    capacity = size;
  uint8_t *staged = kmalloc(capacity);
  if (!staged)
    return (uint32_t)-12;
  uint32_t total = 0;
  uint32_t result = 0;
  while (total < size) {
    uint32_t count = mnt->block_size * EXT2_WRITE_BATCH_BLOCKS -
                     (offset + total) % mnt->block_size;
    if (count > size - total)
      count = size - total;
    unsigned long uncopied = copy_from_user(staged, buffer + total, count);
    if (uncopied > count)
      uncopied = count;
    uint32_t copied = count - (uint32_t)uncopied;
    if (!copied) {
      result = total ? total : (uint32_t)-14;
      break;
    }
    uint32_t written = write_kernel(node, offset + total, copied, staged);
    if (written >= (uint32_t)-4095) {
      /* A commit/I/O failure must not masquerade as a successful prefix. */
      result = written == (uint32_t)-5 || !total ? written : total;
      break;
    }
    total += written;
    result = total;
    if (written < copied || uncopied)
      break;
  }
  kfree(staged);
  return result;
}

static uint32_t ext2_write_kernel(vfs_node_t *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt || !buffer)
    return (uint32_t)-14;
  if (size == 0)
    return 0;
  if (size > UINT32_MAX - offset)
    return (uint32_t)-27;

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return (uint32_t)-12;
  uint32_t bytes_written = 0;
  int error = 0;

  while (bytes_written < size && !error) {
    /* Acquire before reading the inode, not merely before publishing it:
     * another writer may have extended its map during the previous batch. */
    if (ext3_journal_start(mnt)) {
      error = 5;
      break;
    }
    ext2_inode_t inode;
    uint32_t batch_start = bytes_written;
    bool mapping_changed = false;
    mnt->journal.phase = "write-inode-read";
    if (ext2_read_inode(mnt, node->inode, &inode)) {
      error = 5;
    } else {
      for (unsigned touched = 0;
           touched < EXT2_WRITE_BATCH_BLOCKS && bytes_written < size;
           touched++) {
        uint32_t current_offset = offset + bytes_written;
        uint32_t logical_block = current_offset / mnt->block_size;
        uint32_t offset_in_block = current_offset % mnt->block_size;
        uint32_t to_write = mnt->block_size - offset_in_block;
        if (to_write > size - bytes_written)
          to_write = size - bytes_written;

        mnt->journal.phase = "write-mapping";
        uint32_t disk_block;
        if (ext2_get_block_num_checked(mnt, &inode, logical_block, &disk_block)) {
          error = 5;
          break;
        }
        if (!disk_block) {
          disk_block = ext2_alloc_block(mnt);
          if (!disk_block) {
            error = 28;
            break;
          }
          /* Mapping helpers can install indirect roots before a later I/O
           * failure. Preserve their inode changes even on that error path. */
          mapping_changed = true;
          if (ext2_set_block_num(mnt, &inode, logical_block, disk_block)) {
            error = 5;
            break;
          }
          inode.i_blocks += mnt->block_size / 512;
          memset(block_buf, 0, mnt->block_size);
        } else if (offset_in_block || to_write < mnt->block_size) {
          mnt->journal.phase = "write-data-read";
          if (ext2_read_block(mnt, disk_block, block_buf)) {
            error = 5;
            break;
          }
        }

        memcpy(block_buf + offset_in_block, buffer + bytes_written, to_write);
        mnt->journal.phase = "write-data";
        if (ext2_write_block(mnt, disk_block, block_buf)) {
          error = 5;
          break;
        }
        bytes_written += to_write;
      }

      if (bytes_written > batch_start || mapping_changed) {
        if (offset + bytes_written > inode.i_size && bytes_written > batch_start)
          inode.i_size = offset + bytes_written;
        if (bytes_written > batch_start) {
          uint32_t now = ext2_current_time();
          inode.i_mtime = now;
          inode.i_ctime = now;
        }
        mnt->journal.phase = "write-inode-update";
        if (ext2_write_inode(mnt, node->inode, &inode))
          error = 5;
      }
    }
    /* Always release ownership, including read, allocation, and
     * inode errors. The existing journal cannot roll back prior home writes. */
    if (ext3_journal_stop(mnt))
      error = 5;
    if (error != 5 && (bytes_written > batch_start || mapping_changed)) {
      node->length = inode.i_size;
      node->mtime = inode.i_mtime;
      node->ctime = inode.i_ctime;
    }
  }

  kfree(block_buf);
  /* Exhaustion retains traditional short-write behavior;
   * failed metadata or commit I/O must never masquerade as byte success. */
  if (error == 5 || !bytes_written)
    return (uint32_t)-(error ? error : 28);
  return bytes_written;
}

uint32_t ext2_write_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  return ext2_write_staged(node, offset, size, buffer, ext2_write_kernel);
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

static int ext2_rename_in_transaction(vfs_node_t *node, char *old_name,
                                     char *new_name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  vfs_node_t *src_node = ext2_finddir_impl(node, old_name);
  if (!src_node)
    return -1;

  vfs_node_t *dst_node = ext2_finddir_impl(node, new_name);
  if (dst_node) {
    if (dst_node->inode == src_node->inode) {
      kfree(dst_node);
      kfree(src_node);
      return 0;
    }
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

int ext2_rename_impl(vfs_node_t *node, char *old_name, char *new_name) {
  if (!node || !node->device)
    return -1;
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  /* Both names share directory blocks. Serialize the full read/modify/write
   * sequence, including source lookup, so parallel renames cannot overwrite
   * each other's directory images or join another thread's transaction. */
  if (ext3_journal_start(mnt) != 0)
    return -1;
  int result = ext2_rename_in_transaction(node, old_name, new_name);
  int stop_result = ext3_journal_stop(mnt);
  return result != 0 ? result : stop_result;
}

/* Cache downloads move regular files between staging and final directories.
 * Keep source inode/link count intact and serialize both directory mutations
 * in one transaction. Directory moves require '..' and parent-link updates
 * and are deliberately rejected until those semantics are implemented. */
int ext2_rename_cross(vfs_node_t *old_parent, char *old_name,
                      vfs_node_t *new_parent, char *new_name) {
  if (!old_parent || !new_parent || !old_parent->device ||
      old_parent->device != new_parent->device)
    return -18;
  if (old_parent->inode == new_parent->inode)
    return ext2_rename_impl(old_parent, old_name, new_name);
  if ((old_parent->flags & FS_TYPE_MASK) != FS_DIRECTORY ||
      (new_parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return -20;
  ext2_mount_t *mnt = old_parent->device;
  if (ext3_journal_start(mnt))
    return -5;
  int result = -2;
  vfs_node_t *source = ext2_finddir_impl(old_parent, old_name);
  vfs_node_t *dest = NULL;
  if (!source)
    goto out;
  uint32_t type = source->flags & FS_TYPE_MASK;
  if (type != FS_FILE && type != FS_SYMLINK) {
    result = -95;
    goto out;
  }
  dest = ext2_finddir_impl(new_parent, new_name);
  if (dest && (dest->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
    result = -21;
    goto out;
  }
  if (dest && (dest->flags & FS_TYPE_MASK) != FS_FILE &&
      (dest->flags & FS_TYPE_MASK) != FS_SYMLINK) {
    result = -95;
    goto out;
  }
  if (dest && source->inode == dest->inode) {
    result = 0;
    goto out;
  }
  ext2_inode_t replaced_inode;
  if (dest && ext2_read_inode(mnt, dest->inode, &replaced_inode)) {
    result = -5;
    goto out;
  }
  uint8_t dtype = type == FS_SYMLINK ? EXT2_FT_SYMLINK : EXT2_FT_REG_FILE;
  result = dest ? ext2_replace_dir_entry(mnt, new_parent->inode, new_name,
                                         source->inode, dtype)
                : ext2_add_dir_entry(mnt, new_parent->inode, source->inode,
                                      new_name, dtype);
  if (result) {
    result = -5;
    goto out;
  }
  if (ext2_remove_dir_entry(mnt, old_parent->inode, old_name)) {
    /* Preserve the original destination if source removal fails. Rollback
     * can itself fail on device I/O; never report success in either case. */
    if (dest) {
      uint8_t old_dtype = (dest->flags & FS_TYPE_MASK) == FS_SYMLINK
                             ? EXT2_FT_SYMLINK : EXT2_FT_REG_FILE;
      ext2_replace_dir_entry(mnt, new_parent->inode, new_name,
                             dest->inode, old_dtype);
    } else {
      ext2_remove_dir_entry(mnt, new_parent->inode, new_name);
    }
    result = -5;
    goto out;
  }
  if (dest) {
    if (replaced_inode.i_links_count)
      replaced_inode.i_links_count--;
    if (!replaced_inode.i_links_count)
      replaced_inode.i_dtime = ext2_current_time();
    if (ext2_write_inode(mnt, dest->inode, &replaced_inode))
      result = -5;
  }
out:
  kfree(dest);
  kfree(source);
  int stop_result = ext3_journal_stop(mnt);
  return result ? result : (stop_result ? -5 : 0);
}

int ext2_chmod_impl(vfs_node_t *node, uint16_t permission) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;
  if (ext3_journal_start(mnt) != 0)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  inode.i_mode  = (inode.i_mode & 0xF000) | (permission & 0x0FFF);
  inode.i_ctime = ext2_current_time();

  if (ext2_write_inode(mnt, node->inode, &inode)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  node->mask = permission & 0x0FFF;
  return ext3_journal_stop(mnt);
}

int ext2_chown_impl(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;
  if (ext3_journal_start(mnt) != 0)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  uint32_t now = ext2_current_time();
  if (inode.i_uid == uid && inode.i_gid == gid && inode.i_ctime == now) {
    node->uid = uid;
    node->gid = gid;
    return ext3_journal_stop(mnt);
  }

  inode.i_uid   = uid;
  inode.i_gid   = gid;
  inode.i_ctime = now;

  if (ext2_write_inode(mnt, node->inode, &inode)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  node->uid = uid;
  node->gid = gid;
  return ext3_journal_stop(mnt);
}

int ext2_utime_impl(vfs_node_t *node, uint32_t atime, uint32_t mtime) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return -1;

  inode.i_atime = atime;
  inode.i_mtime = mtime;
  inode.i_ctime = ext2_current_time();

  ext3_journal_start(mnt);
  if (ext2_write_inode(mnt, node->inode, &inode)) {
    ext3_journal_stop(mnt);
    return -1;
  }
  ext3_journal_stop(mnt);

  node->atime = atime;
  node->mtime = mtime;
  node->ctime = inode.i_ctime;
  return 0;
}

int ext2_unlink_impl(vfs_node_t *node, char *name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext3_journal_start(mnt);

  vfs_node_t *target = ext2_finddir_impl(node, name);
  if (!target) {
    ext3_journal_stop(mnt);
    return -1;
  }

  uint32_t target_ino = target->inode;
  kfree(target);

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, target_ino, &inode)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  if ((inode.i_mode & 0xF000) == EXT2_S_IFDIR) {
    ext3_journal_stop(mnt);
    return -1;
  }

  if (ext2_remove_dir_entry(mnt, node->inode, name)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  inode.i_links_count--;

  if (inode.i_links_count == 0) {
    inode.i_dtime = ext2_current_time();
    ext2_write_inode(mnt, target_ino, &inode);
  } else {
    ext2_write_inode(mnt, target_ino, &inode);
  }

  ext3_journal_stop(mnt);
  return 0;
}

/* ext2 has no persistent open-file table, so defer reclaiming an unlinked
 * regular inode until the VFS has dropped its final descriptor/mapping/cache
 * reference.  Otherwise a concurrent create can reuse its inode while a
 * mapped VMA still faults pages from that file. */
void ext2_destroy_unlinked_node(vfs_node_t *node) {
  if (!node || !node->device || (node->flags & FS_TYPE_MASK) != FS_FILE)
    return;
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (ext3_journal_start(mnt) != 0)
    return;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode) == 0 &&
      inode.i_links_count == 0 && inode.i_dtime != 0) {
    ext2_free_all_blocks(mnt, &inode);
    ext2_write_inode(mnt, node->inode, &inode);
    ext2_free_inode(mnt, node->inode);
  }
  ext3_journal_stop(mnt);
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
  ext2_mark_group_dirty(mnt, group);

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
