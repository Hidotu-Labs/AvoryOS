#include "ext2_internal.h"
#include "fs/ext4/ext4_dir_index.h"
#include "fs/ext4/ext4_extent.h"

static uint8_t ext2_dirent_dtype(uint8_t file_type) {
  switch (file_type) {
  case 1: return DT_REG;  /* EXT2_FT_REG_FILE */
  case 2: return DT_DIR;
  case 3: return DT_CHR;
  case 4: return DT_BLK;
  case 5: return DT_FIFO;
  case 6: return DT_SOCK;
  case 7: return DT_LNK;
  default: return DT_UNKNOWN;
  }
}

struct dirent *ext2_readdir_impl(vfs_node_t *node, uint32_t index) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return NULL;

  spinlock_acquire(&node->readdir_cursor_lock);

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode)) {
    spinlock_release(&node->readdir_cursor_lock);
    return NULL;
  }

  static struct dirent d;
  memset(&d, 0, sizeof(d));

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf) {
    spinlock_release(&node->readdir_cursor_lock);
    return NULL;
  }

  uint32_t dir_size   = inode.i_size;
  uint32_t byte_pos   = 0;
  uint32_t entry_idx  = 0;
  uint32_t loaded_logical_block = UINT32_MAX;

  /*
   * getdents asks for monotonically increasing indices. Resume at the byte
   * following the previous result instead of rescanning the directory from
   * byte zero. Interleaved or random access safely falls back to a full scan.
   */
  if (index != 0 && index == node->readdir_cursor_index) {
    byte_pos = node->readdir_cursor_offset;
    entry_idx = index;
  }

  while (byte_pos < dir_size) {
    uint32_t logical_block   = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (logical_block != loaded_logical_block) {
      uint32_t disk_block = ext2_get_block_num(mnt, &inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
      loaded_logical_block = logical_block;
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);

    if (entry->inode != 0 && entry->rec_len > 0) {
      if (entry_idx == index) {
        uint32_t name_len = entry->name_len;
        if (name_len > 127)
          name_len = 127;
        memcpy(d.name, entry->name, name_len);
        d.name[name_len] = '\0';
        d.ino = entry->inode;
        d.d_type = ext2_dirent_dtype(entry->file_type);
        node->readdir_cursor_index = index + 1;
        node->readdir_cursor_offset = byte_pos + entry->rec_len;
        kfree(block_buf);
        spinlock_release(&node->readdir_cursor_lock);
        return &d;
      }
      entry_idx++;
    }

    if (entry->rec_len == 0)
      break;
    byte_pos += entry->rec_len;
  }

  kfree(block_buf);
  node->readdir_cursor_index = 0;
  node->readdir_cursor_offset = 0;
  spinlock_release(&node->readdir_cursor_lock);
  return NULL;
}

vfs_node_t *ext2_finddir_impl(vfs_node_t *node, char *name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return NULL;

  ext2_inode_t dir_inode;
  if (ext2_read_inode(mnt, node->inode, &dir_inode))
    return NULL;

  uint32_t name_len = strlen(name);

  /* Fast HTree directory index lookup for ext4 directories */
  if (dir_inode.i_flags & EXT2_INDEX_FL) {
    int found_ino = ext4_htree_find_entry(mnt, &dir_inode, name, name_len);
    if (found_ino > 0) {
      ext2_inode_t target_inode;
      if (ext2_read_inode(mnt, (uint32_t)found_ino, &target_inode))
        return NULL;

      vfs_node_t *result =
          ext2_make_vfs_node(mnt, (uint32_t)found_ino, &target_inode);
      if (result) {
        uint32_t copy_len = name_len > 127 ? 127 : name_len;
        memcpy(result->name, name, copy_len);
        result->name[copy_len] = '\0';
      }
      return result;
    } else if (found_ino == 0) {
      return NULL;
    }
    /* found_ino < 0: fallback to linear scan */
  }

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return NULL;

  uint32_t dir_size = dir_inode.i_size;
  uint32_t byte_pos = 0;

  while (byte_pos < dir_size) {
    uint32_t logical_block   = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block =
          ext2_get_block_num(mnt, &dir_inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);

    if (entry->inode != 0 && entry->name_len == name_len) {
      bool match = true;
      for (uint32_t i = 0; i < name_len; i++) {
        if (entry->name[i] != name[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        uint32_t found_ino = entry->inode;
        kfree(block_buf);

        ext2_inode_t target_inode;
        if (ext2_read_inode(mnt, found_ino, &target_inode))
          return NULL;

        vfs_node_t *result =
            ext2_make_vfs_node(mnt, found_ino, &target_inode);
        if (result) {
          uint32_t copy_len = name_len > 127 ? 127 : name_len;
          memcpy(result->name, name, copy_len);
          result->name[copy_len] = '\0';
        }
        return result;
      }
    }

    if (entry->rec_len == 0)
      break;
    byte_pos += entry->rec_len;
  }

  kfree(block_buf);
  return NULL;
}

int ext2_add_dir_entry(ext2_mount_t *mnt, uint32_t dir_inode_num,
                       uint32_t child_inode_num, const char *name,
                       uint8_t file_type) {
  ext2_inode_t dir_inode;
  if (ext2_read_inode(mnt, dir_inode_num, &dir_inode))
    return -1;
  if (dir_inode.i_flags & EXT2_INDEX_FL)
    return ext4_dx_add_entry(mnt, dir_inode_num, &dir_inode,
                             child_inode_num, name, file_type);

  uint32_t name_len = strlen(name);
  uint32_t needed   = ((8 + name_len) + 3) & ~3u;

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return -1;

  uint32_t dir_size = dir_inode.i_size;
  uint32_t byte_pos = 0;

  while (byte_pos < dir_size) {
    uint32_t logical_block   = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block =
          ext2_get_block_num(mnt, &dir_inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);
    if (entry->rec_len == 0)
      break;

    uint32_t real_size = ((8 + entry->name_len) + 3) & ~3u;
    uint32_t slack     = entry->rec_len - real_size;

    if (slack >= needed) {
      uint32_t old_rec_len = entry->rec_len;
      entry->rec_len       = (uint16_t)real_size;

      ext2_dirent_t *new_entry =
          (ext2_dirent_t *)(block_buf + offset_in_block + real_size);
      new_entry->inode     = child_inode_num;
      new_entry->rec_len   = (uint16_t)(old_rec_len - real_size);
      new_entry->name_len  = (uint8_t)name_len;
      new_entry->file_type = file_type;
      memcpy(new_entry->name, name, name_len);

      uint32_t disk_block =
          ext2_get_block_num(mnt, &dir_inode, byte_pos / mnt->block_size);
      ext3_journal_block(mnt, disk_block, block_buf);
      kfree(block_buf);
      return 0;
    }

    byte_pos += entry->rec_len;
  }

  uint32_t logical_block = dir_size / mnt->block_size;
  uint32_t new_block = 0;
  if (dir_inode.i_flags & EXT4_EXTENTS_FL) {
    uint64_t allocated = 0;
    if (ext4_alloc_extent(mnt, &dir_inode, dir_inode_num, logical_block, 1,
                          &allocated) != 0 || allocated > UINT32_MAX) {
      kfree(block_buf);
      return -1;
    }
    new_block = (uint32_t)allocated;
  } else {
    new_block = ext2_alloc_block(mnt);
    if (!new_block ||
        ext2_set_block_num(mnt, &dir_inode, logical_block, new_block) != 0) {
      if (new_block)
        ext2_free_block(mnt, new_block);
      kfree(block_buf);
      return -1;
    }
  }
  dir_inode.i_size   += mnt->block_size;
  dir_inode.i_blocks += mnt->block_size / 512;

  memset(block_buf, 0, mnt->block_size);
  ext2_dirent_t *new_entry = (ext2_dirent_t *)block_buf;
  new_entry->inode     = child_inode_num;
  new_entry->rec_len   = (uint16_t)mnt->block_size;
  new_entry->name_len  = (uint8_t)name_len;
  new_entry->file_type = file_type;
  memcpy(new_entry->name, name, name_len);

  ext3_journal_block(mnt, new_block, block_buf);
  ext2_write_inode(mnt, dir_inode_num, &dir_inode);

  kfree(block_buf);
  return 0;
}

int ext2_remove_dir_entry(ext2_mount_t *mnt, uint32_t dir_inode_num,
                          const char *name) {
  ext2_inode_t dir_inode;
  if (ext2_read_inode(mnt, dir_inode_num, &dir_inode))
    return -1;

  uint32_t name_len = strlen(name);
  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return -1;

  uint32_t dir_size            = dir_inode.i_size;
  uint32_t byte_pos            = 0;
  uint32_t prev_offset         = 0;
  bool     prev_valid          = false;
  uint32_t current_block_start = 0;

  while (byte_pos < dir_size) {
    uint32_t logical_block   = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block =
          ext2_get_block_num(mnt, &dir_inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
      prev_valid           = false;
      current_block_start  = byte_pos;
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);
    if (entry->rec_len == 0)
      break;

    if (entry->inode != 0 && entry->name_len == name_len) {
      bool match = true;
      for (uint32_t i = 0; i < name_len; i++) {
        if (entry->name[i] != name[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        if (prev_valid) {
          ext2_dirent_t *prev =
              (ext2_dirent_t *)(block_buf + prev_offset);
          prev->rec_len += entry->rec_len;
        } else {
          entry->inode = 0;
        }
        uint32_t disk_block = ext2_get_block_num(
            mnt, &dir_inode, current_block_start / mnt->block_size);
        ext3_journal_block(mnt, disk_block, block_buf);
        kfree(block_buf);
        return 0;
      }
    }

    prev_offset = offset_in_block;
    prev_valid  = (entry->inode != 0);
    byte_pos   += entry->rec_len;
  }

  kfree(block_buf);
  return -1;
}

bool ext2_dir_is_empty(ext2_mount_t *mnt, uint32_t inode_num) {
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, inode_num, &inode))
    return false;

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return false;

  uint32_t dir_size = inode.i_size;
  uint32_t byte_pos = 0;

  while (byte_pos < dir_size) {
    uint32_t logical_block   = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block = ext2_get_block_num(mnt, &inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);
    if (entry->rec_len == 0)
      break;

    if (entry->inode != 0) {
      bool is_dot    = (entry->name_len == 1 && entry->name[0] == '.');
      bool is_dotdot = (entry->name_len == 2 && entry->name[0] == '.' &&
                        entry->name[1] == '.');
      if (!is_dot && !is_dotdot) {
        kfree(block_buf);
        return false;
      }
    }

    byte_pos += entry->rec_len;
  }

  kfree(block_buf);
  return true;
}

int ext2_create_impl(vfs_node_t *node, char *name, uint16_t permission) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  /* Keep allocation, inode initialization, and directory insertion atomic. */
  ext3_journal_start(mnt);

  if (ext2_finddir_impl(node, name) != NULL) {
    ext3_journal_stop(mnt);
    return -1;
  }

  uint32_t new_ino = ext2_alloc_inode(mnt);
  if (!new_ino) {
    ext3_journal_stop(mnt);
    return -1;
  }

  ext2_inode_t new_inode;
  memset(&new_inode, 0, sizeof(ext2_inode_t));
  new_inode.i_mode        = EXT2_S_IFREG | (permission & 0x0FFF);
  new_inode.i_size        = 0;
  new_inode.i_links_count = 1;
  new_inode.i_blocks      = 0;

  uint32_t now        = ext2_current_time();
  new_inode.i_atime   = now;
  new_inode.i_ctime   = now;
  new_inode.i_mtime   = now;

  if (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)
    ext4_extent_init_inode(&new_inode);

  if (ext2_write_inode(mnt, new_ino, &new_inode)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  if (ext2_add_dir_entry(mnt, node->inode, new_ino, name, EXT2_FT_REG_FILE)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  ext3_journal_stop(mnt);
  return 0;
}

int ext2_mkdir_impl(vfs_node_t *node, char *name, uint16_t permission) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  /* Serialize shared inode-table and directory-block read/modify/writes. */
  ext3_journal_start(mnt);

  if (ext2_finddir_impl(node, name) != NULL) {
    ext3_journal_stop(mnt);
    return -1;
  }

  uint32_t new_ino = ext2_alloc_inode(mnt);
  if (!new_ino) {
    ext3_journal_stop(mnt);
    return -1;
  }

  uint32_t data_block = ext2_alloc_block(mnt);
  if (!data_block) {
    ext3_journal_stop(mnt);
    return -1;
  }

  ext2_inode_t new_inode;
  memset(&new_inode, 0, sizeof(ext2_inode_t));
  new_inode.i_mode        = EXT2_S_IFDIR | (permission & 0x0FFF);
  new_inode.i_size        = mnt->block_size;
  new_inode.i_links_count = 2;
  new_inode.i_blocks      = mnt->block_size / 512;
  new_inode.i_block[0]    = data_block;

  uint32_t now      = ext2_current_time();
  new_inode.i_atime = now;
  new_inode.i_ctime = now;
  new_inode.i_mtime = now;

  if (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
    ext4_extent_init_inode(&new_inode);
    if (ext4_extent_insert(mnt, &new_inode, new_ino, 0, data_block, 1) != 0) {
      ext3_journal_stop(mnt);
      return -1;
    }
  }

  uint8_t *block_buf = kcalloc(1, mnt->block_size);
  if (!block_buf) {
    ext3_journal_stop(mnt);
    return -1;
  }

  ext2_dirent_t *dot = (ext2_dirent_t *)block_buf;
  dot->inode     = new_ino;
  dot->rec_len   = 12;
  dot->name_len  = 1;
  dot->file_type = EXT2_FT_DIR;
  dot->name[0]   = '.';

  ext2_dirent_t *dotdot = (ext2_dirent_t *)(block_buf + 12);
  dotdot->inode     = node->inode;
  dotdot->rec_len   = (uint16_t)(mnt->block_size - 12);
  dotdot->name_len  = 2;
  dotdot->file_type = EXT2_FT_DIR;
  dotdot->name[0]   = '.';
  dotdot->name[1]   = '.';

  ext3_journal_block(mnt, data_block, block_buf);
  kfree(block_buf);

  if (ext2_write_inode(mnt, new_ino, &new_inode)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  if (ext2_add_dir_entry(mnt, node->inode, new_ino, name, EXT2_FT_DIR)) {
    ext3_journal_stop(mnt);
    return -1;
  }

  ext2_inode_t parent_inode;
  if (ext2_read_inode(mnt, node->inode, &parent_inode) == 0) {
    parent_inode.i_links_count++;
    ext2_write_inode(mnt, node->inode, &parent_inode);
  }

  uint32_t group = (new_ino - 1) / mnt->inodes_per_group;
  mnt->bgdt[group].bg_used_dirs_count++;
  ext2_mark_metadata_dirty(mnt);

  ext3_journal_stop(mnt);
  return 0;
}
