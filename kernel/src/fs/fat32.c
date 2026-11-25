#include "fat32.h"
#include "console/klog.h"
#include "drivers/storage/block.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "vfs.h"
#include <stddef.h>

// Forward Declarations

static struct dirent *fat32_readdir_impl(vfs_node_t *node, uint64_t index);
static vfs_node_t *fat32_finddir_impl(vfs_node_t *node, char *name);
static uint32_t fat32_read_impl(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer);
static uint32_t fat32_write_impl(vfs_node_t *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer);
static int fat32_unlink_impl(vfs_node_t *parent, char *name);
static int fat32_rmdir_impl(vfs_node_t *parent, char *name);
static int fat32_rename_impl(vfs_node_t *dir, char *old_name, char *new_name);
static int fat32_truncate_impl(vfs_node_t *node, uint32_t new_size);
int fat32_statfs_impl(vfs_node_t *node, struct statfs_buf *buf);

typedef struct {
  vfs_node_t vfs;
  uint32_t parent_cluster;
  uint32_t entry_cluster;
  uint32_t entry_index;
  bool has_dir_entry;
} fat32_node_t;

static uint8_t lfn_checksum(uint8_t *sfn);
static int decode_lfn(fat32_lfn_entry_t *entries, int count, char *out,
                      size_t out_cap, uint8_t *sfn);
static void get_fat_time(uint16_t *date, uint16_t *time);
static uint32_t fat_entry_cluster(const fat32_mount_t *mnt,
                                  const fat32_dir_entry_t *entry);

static void fat32_mount_free(fat32_mount_t *mnt) {
  kfree(mnt->root_node);
  block_put(mnt->dev);
  kfree(mnt);
}

// Mount lifetime.  `refs` is one for the mount itself plus one per node from
// fat32_make_vfs_node().  A node's reference drops when the VFS releases it
// (->destroy, see fat32_node_destroy) or fat32 code frees it directly
// (fat32_node_drop); the mount's drops in fat32_unmount().  Whichever is last
// frees the state, so a forced unmount with open files frees it on the last
// close instead of leaking it.
static void fat32_mount_put(fat32_mount_t *mnt) {
  if (__atomic_sub_fetch(&mnt->refs, 1, __ATOMIC_ACQ_REL) == 0)
    fat32_mount_free(mnt);
}

static void fat32_node_destroy(vfs_node_t *node) {
  fat32_mount_t *mnt = (fat32_mount_t *)node->device;
  /* vfs_close() keeps a persistent node alive (something is mounted on
   * it), so that node still references the mount and keeps counting. */
  if (!mnt || ((node->flags & FS_PERSISTENT) && !(node->flags & FS_DELETED)))
    return;
  node->device = NULL;
  fat32_mount_put(mnt);
}

// Internal Helpers

// Convert FAT date (16-bit) to Unix timestamp (approximate)
static uint32_t fat_date_to_unix(uint16_t date, uint16_t time) {
  uint32_t year = 1980 + ((date >> 9) & 0x7F);
  uint32_t month = (date >> 5) & 0x0F;
  uint32_t day = date & 0x1F;
  uint32_t hour = (time >> 11) & 0x1F;
  uint32_t minute = (time >> 5) & 0x3F;
  uint32_t second = (time & 0x1F) * 2;

  // Very rough approximation (ignoring leap years, etc.)
  uint32_t days = (year - 1970) * 365 + (month - 1) * 31 + day;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

// Check if a directory entry is the end of the directory
static bool is_end_of_directory(fat32_dir_entry_t *entry) {
  return entry->name[0] == 0x00;
}

// Check if a directory entry is deleted
static bool is_deleted_entry(fat32_dir_entry_t *entry) {
  return entry->name[0] == 0xE5;
}

// Check if an entry is an LFN entry
static bool is_lfn_entry(fat32_dir_entry_t *entry) {
  return entry->attr == FAT32_ATTR_LFN;
}

// FAT Access

static int fat_rw_bpb_sectors(fat32_mount_t *mnt, uint64_t bpb_lba,
                              uint32_t count, void *buffer, bool write) {
  if (!mnt || !mnt->dev || !buffer || count == 0)
    return -1;
  if (write && !mnt->dev->write_sectors)
    return -1;
  if (!write && !mnt->dev->read_sectors)
    return -1;

  uint32_t dev_ss = mnt->dev->sector_size ? mnt->dev->sector_size : 512;
  uint32_t bpb_ss = mnt->bytes_per_sector;
  if (dev_ss == bpb_ss) {
    return write ? mnt->dev->write_sectors(mnt->dev, bpb_lba, count, buffer)
                 : mnt->dev->read_sectors(mnt->dev, bpb_lba, count, buffer);
  }

  uint64_t byte_off = bpb_lba * (uint64_t)bpb_ss;
  uint64_t total_bytes = (uint64_t)count * (uint64_t)bpb_ss;
  uint8_t *bounce = kmalloc(dev_ss);
  if (!bounce)
    return -1;

  uint8_t *dst = (uint8_t *)buffer;
  uint64_t done = 0;
  while (done < total_bytes) {
    uint64_t cur = byte_off + done;
    uint64_t dev_lba = cur / dev_ss;
    uint32_t in_sec = (uint32_t)(cur % dev_ss);
    uint32_t chunk = dev_ss - in_sec;
    if ((uint64_t)chunk > total_bytes - done)
      chunk = (uint32_t)(total_bytes - done);

    if (mnt->dev->read_sectors(mnt->dev, dev_lba, 1, bounce) != 0) {
      kfree(bounce);
      return -1;
    }
    if (write) {
      memcpy(bounce + in_sec, dst + done, chunk);
      if (mnt->dev->write_sectors(mnt->dev, dev_lba, 1, bounce) != 0) {
        kfree(bounce);
        return -1;
      }
    } else {
      memcpy(dst + done, bounce + in_sec, chunk);
    }
    done += chunk;
  }
  kfree(bounce);
  return 0;
}

static bool fat_is_valid_cluster(const fat32_mount_t *mnt, uint32_t cluster) {
  if (cluster < 2 || cluster >= mnt->total_clusters + 2)
    return false;
  if (mnt->fat_type == 12)
    return cluster < 0xFF7u;
  if (mnt->fat_type == 16)
    return cluster < 0xFFF7u;
  return cluster < 0x0FFFFFF7u;
}

// Read a FAT entry to get the next cluster in a chain
uint32_t fat32_get_next_cluster(fat32_mount_t *mnt, uint32_t cluster) {
  if (!fat_is_valid_cluster(mnt, cluster)) {
    return 0;
  }

  uint32_t fat_offset;
  if (mnt->fat_type == 12)
    fat_offset = cluster + (cluster / 2u);
  else if (mnt->fat_type == 16)
    fat_offset = cluster * 2u;
  else
    fat_offset = cluster * 4u;

  uint32_t fat_sector =
      mnt->fat_start_sector + (fat_offset / mnt->bytes_per_sector);
  uint32_t offset_in_sector = fat_offset % mnt->bytes_per_sector;

  uint32_t sec_count =
      (mnt->fat_type == 12 && offset_in_sector + 1 >= mnt->bytes_per_sector)
          ? 2u
          : 1u;
  uint8_t *buffer = kmalloc(sec_count * mnt->bytes_per_sector);
  if (!buffer)
    return 0;

  if (fat_rw_bpb_sectors(mnt, fat_sector, sec_count, buffer, false) != 0) {
    kfree(buffer);
    return 0;
  }

  uint32_t next_cluster;
  if (mnt->fat_type == 12) {
    uint16_t raw = (uint16_t)buffer[offset_in_sector] |
                   ((uint16_t)buffer[offset_in_sector + 1] << 8);
    next_cluster = (cluster & 1u) ? (raw >> 4) : (raw & 0x0FFFu);
  } else if (mnt->fat_type == 16) {
    next_cluster = (uint16_t)buffer[offset_in_sector] |
                   ((uint16_t)buffer[offset_in_sector + 1] << 8);
  } else {
    next_cluster = *(uint32_t *)(buffer + offset_in_sector);
    next_cluster &= 0x0FFFFFFFu; // FAT32 uses only 28 bits
  }

  kfree(buffer);
  return next_cluster;
}

// Read a cluster into a buffer
int fat32_read_cluster(fat32_mount_t *mnt, uint32_t cluster, void *buffer) {
  if (!fat_is_valid_cluster(mnt, cluster)) {
    return -1;
  }

  // Calculate the sector for this cluster
  uint64_t first_sector =
      (uint64_t)mnt->data_start_sector +
      (uint64_t)(cluster - 2u) * (uint64_t)mnt->sectors_per_cluster;

  return fat_rw_bpb_sectors(mnt, first_sector, mnt->sectors_per_cluster,
                            buffer, false);
}

static void fat32_invalidate_fsinfo(fat32_mount_t *mnt) {
  if (!mnt || mnt->fsinfo_invalidated || mnt->fat_type != 32)
    return;
  uint32_t fsi_sec = mnt->boot.ebpb.fs_info_sector;
  if (fsi_sec == 0 || fsi_sec >= mnt->boot.bpb.reserved_sectors ||
      mnt->bytes_per_sector < sizeof(fat32_fsinfo_t)) {
    mnt->fsinfo_invalidated = true;
    return;
  }

  uint8_t *sec = kmalloc(mnt->bytes_per_sector);
  if (!sec)
    return;
  if (fat_rw_bpb_sectors(mnt, fsi_sec, 1, sec, false) == 0) {
    fat32_fsinfo_t *fsi = (fat32_fsinfo_t *)sec;
    if (fsi->lead_signature == 0x41615252u &&
        fsi->struct_signature == 0x61417272u &&
        fsi->trail_signature == 0xAA550000u) {
      fsi->free_clusters = 0xFFFFFFFFu;
      fsi->next_free = 0xFFFFFFFFu;
      fat_rw_bpb_sectors(mnt, fsi_sec, 1, sec, true);
    }
  }
  kfree(sec);
  mnt->fsinfo_invalidated = true;
}

// Write a FAT entry (set next cluster in chain)
static int fat32_set_fat_entry(fat32_mount_t *mnt, uint32_t cluster,
                               uint32_t value) {
  if (mnt->fat_type != 32 || !FAT32_IS_VALID(cluster) ||
      cluster >= mnt->total_clusters + 2)
    return -1;

  fat32_invalidate_fsinfo(mnt);

  uint32_t fat_offset = cluster * 4;
  uint32_t fat_sector =
      mnt->fat_start_sector + (fat_offset / mnt->bytes_per_sector);
  uint32_t offset_in_sector = fat_offset % mnt->bytes_per_sector;

  uint8_t *buffer = kmalloc(mnt->bytes_per_sector);
  if (!buffer)
    return -1;

  // Read the FAT sector
  int err = fat_rw_bpb_sectors(mnt, fat_sector, 1, buffer, false);
  if (err) {
    kfree(buffer);
    return -1;
  }

  // Modify the entry (keep upper 4 bits)
  uint32_t *entry = (uint32_t *)(buffer + offset_in_sector);
  *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);

  // Write back to all FAT copies
  for (uint32_t fat_num = 0; fat_num < mnt->boot.bpb.num_fats; fat_num++) {
    uint32_t sector = fat_sector + fat_num * mnt->fat_size_sectors;
    err = fat_rw_bpb_sectors(mnt, sector, 1, buffer, true);
    if (err) {
      kfree(buffer);
      return -1;
    }
  }

  kfree(buffer);
  return 0;
}

// Find a free cluster in the FAT
static uint32_t fat32_find_free_cluster(fat32_mount_t *mnt) {
  if (!mnt || mnt->fat_type != 32 || mnt->total_clusters == 0)
    return 0;

  uint32_t limit = mnt->total_clusters + 2u;
  uint32_t start = mnt->next_free_hint;
  if (start < 2u || start >= limit)
    start = 2u;

  uint8_t *fat_buf = kmalloc(mnt->bytes_per_sector);
  if (!fat_buf)
    return 0;

  uint32_t last_sector = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < mnt->total_clusters; i++) {
    uint32_t cluster = start + i;
    if (cluster >= limit)
      cluster -= mnt->total_clusters;

    uint32_t fat_offset = cluster * 4u;
    uint32_t fat_sector =
        mnt->fat_start_sector + (fat_offset / mnt->bytes_per_sector);
    uint32_t offset_in_sector = fat_offset % mnt->bytes_per_sector;

    if (fat_sector != last_sector) {
      if (fat_rw_bpb_sectors(mnt, fat_sector, 1, fat_buf, false) != 0) {
        last_sector = 0xFFFFFFFFu;
        continue;
      }
      last_sector = fat_sector;
    }

    uint32_t entry = *(uint32_t *)(fat_buf + offset_in_sector);
    if ((entry & 0x0FFFFFFFu) == 0) {
      uint32_t next_hint = cluster + 1u;
      if (next_hint >= limit)
        next_hint = 2u;
      mnt->next_free_hint = next_hint;
      kfree(fat_buf);
      return cluster;
    }
  }

  kfree(fat_buf);
  return 0; // No free clusters
}

// Write a cluster to disk
int fat32_write_cluster(fat32_mount_t *mnt, uint32_t cluster, void *buffer) {
  if (mnt->fat_type != 32 || !FAT32_IS_VALID(cluster))
    return -1;

  uint64_t first_sector =
      (uint64_t)mnt->data_start_sector +
      (uint64_t)(cluster - 2u) * (uint64_t)mnt->sectors_per_cluster;

  return fat_rw_bpb_sectors(mnt, first_sector, mnt->sectors_per_cluster,
                            buffer, true);
}

// Allocate a new zeroed cluster and link it to the chain
static uint32_t fat32_alloc_cluster(fat32_mount_t *mnt, uint32_t prev_cluster) {
  uint32_t new_cluster = fat32_find_free_cluster(mnt);
  if (new_cluster == 0)
    return 0;

  // Mark new cluster as EOF
  if (fat32_set_fat_entry(mnt, new_cluster, FAT32_EOF_MARKER) != 0) {
    return 0;
  }

  uint8_t *zero_buf = kmalloc(mnt->bytes_per_cluster);
  if (!zero_buf) {
    fat32_set_fat_entry(mnt, new_cluster, 0);
    return 0;
  }
  memset(zero_buf, 0, mnt->bytes_per_cluster);
  if (fat32_write_cluster(mnt, new_cluster, zero_buf) != 0) {
    kfree(zero_buf);
    fat32_set_fat_entry(mnt, new_cluster, 0);
    return 0;
  }
  kfree(zero_buf);

  // Link to previous cluster if specified
  if (prev_cluster != 0) {
    if (fat32_set_fat_entry(mnt, prev_cluster, new_cluster) != 0) {
      // Try to free the allocated cluster
      fat32_set_fat_entry(mnt, new_cluster, 0);
      return 0;
    }
  }

  return new_cluster;
}

// Free all clusters in a chain starting from the given cluster
static int fat32_free_cluster_chain(fat32_mount_t *mnt,
                                    uint32_t start_cluster) {
  if (!FAT32_IS_VALID(start_cluster))
    return -1;

  uint32_t cluster = start_cluster;
  while (FAT32_IS_VALID(cluster)) {
    uint32_t next = fat32_get_next_cluster(mnt, cluster);

    // Mark cluster as free
    if (fat32_set_fat_entry(mnt, cluster, 0) != 0) {
      return -1; // Failed to free
    }

    cluster = next;
  }

  return 0;
}

static void fat32_sfn_to_str(const uint8_t raw[11], char out[13]) {
  int j = 0;
  for (int i = 0; i < 8 && raw[i] != ' '; i++) {
    char c = (char)raw[i];
    if (i == 0 && (unsigned char)c == 0x05)
      c = (char)0xE5;
    out[j++] = c;
  }
  if (raw[8] != ' ') {
    out[j++] = '.';
    for (int i = 8; i < 11 && raw[i] != ' '; i++) {
      out[j++] = (char)raw[i];
    }
  }
  out[j] = '\0';
}

static bool fat32_is_valid_sfn_char(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
    return true;
  switch (c) {
  case '$':
  case '%':
  case '\'':
  case '-':
  case '_':
  case '@':
  case '~':
  case '`':
  case '!':
  case '(':
  case ')':
  case '{':
  case '}':
  case '^':
  case '#':
  case '&':
    return true;
  default:
    return false;
  }
}

static char fat32_to_upper(char c) {
  if (c >= 'a' && c <= 'z')
    return (char)(c - 'a' + 'A');
  return c;
}

static bool fat32_validate_filename(const char *name) {
  if (!name || !name[0])
    return false;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return false;
  size_t len = strlen(name);
  if (len > 255)
    return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c < 0x20)
      return false;
    switch (c) {
    case '/':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
      return false;
    default:
      break;
    }
  }
  return true;
}

static bool fat32_is_strict_8_3(const char *name) {
  if (!fat32_validate_filename(name))
    return false;
  int base_len = 0;
  int ext_len = 0;
  bool seen_dot = false;
  for (const char *p = name; *p; p++) {
    if (*p == '.') {
      if (seen_dot || base_len == 0)
        return false;
      seen_dot = true;
      continue;
    }
    if (!fat32_is_valid_sfn_char(*p))
      return false;
    if (!seen_dot) {
      if (++base_len > 8)
        return false;
    } else {
      if (++ext_len > 3)
        return false;
    }
  }
  if (base_len == 0)
    return false;
  if (seen_dot && ext_len == 0)
    return false;
  return true;
}

typedef struct {
  uint32_t cluster;
  uint32_t index;
} fat32_dir_slot_t;

typedef struct {
  bool found;
  fat32_dir_entry_t sfn_entry;
  uint32_t sfn_cluster;
  uint32_t sfn_index;
  fat32_dir_slot_t lfn_slots[20];
  int lfn_count;
  char lfn_name[261];
} fat32_lookup_t;

static int fat32_dir_lookup_raw(fat32_mount_t *mnt, uint32_t dir_cluster,
                                const char *name, fat32_lookup_t *out);
static int fat32_dir_mark_deleted(fat32_mount_t *mnt,
                                  const fat32_dir_slot_t *lfn_slots,
                                  int lfn_count, uint32_t sfn_cluster,
                                  uint32_t sfn_index);
static int fat32_dir_insert_entry(fat32_mount_t *mnt, uint32_t dir_cluster,
                                  const char *name,
                                  const fat32_dir_entry_t *tpl);
static bool fat32_dir_is_empty(fat32_mount_t *mnt, uint32_t dir_cluster);

static int fat32_update_dir_entry(vfs_node_t *node) {
  fat32_node_t *fn = (fat32_node_t *)node;
  fat32_mount_t *mnt = node ? (fat32_mount_t *)node->device : NULL;
  if (!mnt || mnt->fat_type != 32 || !fn->has_dir_entry ||
      !FAT32_IS_VALID(fn->entry_cluster))
    return -1;

  uint32_t entries_per_cluster =
      mnt->bytes_per_cluster / sizeof(fat32_dir_entry_t);
  if (fn->entry_index >= entries_per_cluster)
    return -1;

  uint8_t *dir_buf = kmalloc(mnt->bytes_per_cluster);
  if (!dir_buf)
    return -1;

  int rc = -1;
  if (fat32_read_cluster(mnt, fn->entry_cluster, dir_buf) == 0) {
    fat32_dir_entry_t *entry =
        (fat32_dir_entry_t *)dir_buf + fn->entry_index;
    if (entry->name[0] != 0x00 && entry->name[0] != 0xE5 &&
        entry->attr != FAT32_ATTR_LFN) {
      entry->cluster_low = (uint16_t)(node->impl & 0xFFFFu);
      entry->cluster_high = (uint16_t)((node->impl >> 16) & 0xFFFFu);
      entry->file_size = node->length;
      uint16_t md = 0, mt = 0;
      get_fat_time(&md, &mt);
      entry->modify_date = md;
      entry->modify_time = mt;
      entry->last_access_date = md;
      if (fat32_write_cluster(mnt, fn->entry_cluster, dir_buf) == 0) {
        rc = 0;
      }
    }
  }

  kfree(dir_buf);
  return rc;
}

// Delete a file from the directory
static int fat32_unlink_impl(vfs_node_t *parent, char *name) {
  fat32_mount_t *mnt = parent ? (fat32_mount_t *)parent->device : NULL;
  if (!mnt || !name)
    return -1;
  if (mnt->fat_type != 32)
    return -30; // -EROFS

  uint32_t dir_cluster = parent->impl;
  if (dir_cluster == 0)
    dir_cluster = mnt->root_cluster;

  fat32_lookup_t found;
  if (fat32_dir_lookup_raw(mnt, dir_cluster, name, &found) != 0 || !found.found)
    return -2; // -ENOENT

  if (found.sfn_entry.attr & FAT32_ATTR_DIRECTORY)
    return -21; // -EISDIR

  uint32_t file_cluster = fat_entry_cluster(mnt, &found.sfn_entry);
  if (fat32_dir_mark_deleted(mnt, found.lfn_slots, found.lfn_count,
                             found.sfn_cluster, found.sfn_index) != 0)
    return -5; // -EIO

  if (FAT32_IS_VALID(file_cluster)) {
    fat32_free_cluster_chain(mnt, file_cluster);
  }

  return 0;
}

// Delete an empty directory
static int fat32_rmdir_impl(vfs_node_t *parent, char *name) {
  fat32_mount_t *mnt = parent ? (fat32_mount_t *)parent->device : NULL;
  if (!mnt || !name)
    return -1;
  if (mnt->fat_type != 32)
    return -30; // -EROFS
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return -22; // -EINVAL

  uint32_t parent_cluster = parent->impl;
  if (parent_cluster == 0)
    parent_cluster = mnt->root_cluster;

  fat32_lookup_t found;
  if (fat32_dir_lookup_raw(mnt, parent_cluster, name, &found) != 0 ||
      !found.found)
    return -2; // -ENOENT

  if (!(found.sfn_entry.attr & FAT32_ATTR_DIRECTORY))
    return -20; // -ENOTDIR

  uint32_t dir_cluster = fat_entry_cluster(mnt, &found.sfn_entry);
  if (FAT32_IS_VALID(dir_cluster) && !fat32_dir_is_empty(mnt, dir_cluster))
    return -39; // -ENOTEMPTY

  if (fat32_dir_mark_deleted(mnt, found.lfn_slots, found.lfn_count,
                             found.sfn_cluster, found.sfn_index) != 0)
    return -5;

  if (FAT32_IS_VALID(dir_cluster)) {
    fat32_free_cluster_chain(mnt, dir_cluster);
  }

  return 0;
}

// Truncate a file to a smaller size
static int fat32_truncate_impl(vfs_node_t *node, uint32_t new_size) {
  fat32_mount_t *mnt = node ? (fat32_mount_t *)node->device : NULL;
  if (!mnt)
    return -1;
  if (mnt->fat_type != 32)
    return -30; // -EROFS

  // Can only truncate, not extend
  if (new_size > node->length)
    return -1;

  if (new_size == 0) {
    uint32_t cluster = node->impl;
    node->impl = 0;
    node->length = 0;
    if (((fat32_node_t *)node)->has_dir_entry &&
        fat32_update_dir_entry(node) != 0)
      return -5;
    if (FAT32_IS_VALID(cluster)) {
      fat32_free_cluster_chain(mnt, cluster);
    }
    return 0;
  }

  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint32_t keep_clusters = (new_size + cluster_size - 1u) / cluster_size;
  uint32_t cluster = node->impl;

  for (uint32_t i = 1; i < keep_clusters && FAT32_IS_VALID(cluster); i++) {
    cluster = fat32_get_next_cluster(mnt, cluster);
  }

  if (FAT32_IS_VALID(cluster)) {
    uint32_t next = fat32_get_next_cluster(mnt, cluster);
    if (fat32_set_fat_entry(mnt, cluster, FAT32_EOF_MARKER) != 0)
      return -5;
    if (FAT32_IS_VALID(next)) {
      fat32_free_cluster_chain(mnt, next);
    }
    uint32_t rem = new_size % cluster_size;
    if (rem != 0) {
      uint8_t *buf = kmalloc(cluster_size);
      if (buf) {
        if (fat32_read_cluster(mnt, cluster, buf) == 0) {
          memset(buf + rem, 0, cluster_size - rem);
          fat32_write_cluster(mnt, cluster, buf);
        }
        kfree(buf);
      }
    }
  }

  node->length = new_size;
  if (((fat32_node_t *)node)->has_dir_entry &&
      fat32_update_dir_entry(node) != 0)
    return -5;
  return 0;
}

#define MSDOS_SUPER_MAGIC 0x4d44

int fat32_statfs_impl(vfs_node_t *node, struct statfs_buf *buf) {
  fat32_mount_t *mnt = node ? (fat32_mount_t *)node->device : NULL;
  if (!mnt || !buf)
    return -1;
  uint64_t total = mnt->total_clusters;
  /* Count free clusters by scanning the FAT in bulk. A per-cluster
   * fat32_get_next_cluster() call does a kmalloc + sector read each time,
   * which makes statfs (df, Plasma) painfully slow; read each FAT sector
   * once and count zero entries instead. */
  uint64_t free_clusters = 0;
  if (mnt->fat_type == 32 || mnt->fat_type == 16) {
    uint32_t entry_bytes = (mnt->fat_type == 32) ? 4u : 2u;
    uint32_t entries_per_sector = mnt->bytes_per_sector / entry_bytes;
    uint8_t *sec = kmalloc(mnt->bytes_per_sector);
    if (sec) {
      uint32_t total_entries = (uint32_t)total + 2u;
      for (uint32_t s = 0; s < mnt->fat_size_sectors; s++) {
        uint32_t base = s * entries_per_sector;
        if (base >= total_entries)
          break;
        if (fat_rw_bpb_sectors(mnt, mnt->fat_start_sector + s, 1, sec, false) !=
            0)
          break;
        uint32_t n = entries_per_sector;
        if (base + n > total_entries)
          n = total_entries - base;
        uint32_t start = (base < 2) ? (2u - base) : 0;
        if (mnt->fat_type == 32) {
          uint32_t *e = (uint32_t *)sec;
          for (uint32_t i = start; i < n; i++) {
            if ((e[i] & 0x0FFFFFFFu) == 0)
              free_clusters++;
          }
        } else {
          uint16_t *e = (uint16_t *)sec;
          for (uint32_t i = start; i < n; i++) {
            if (e[i] == 0)
              free_clusters++;
          }
        }
        if (base + n >= total_entries)
          break;
      }
      kfree(sec);
    } else {
      free_clusters = total / 2;
    }
  } else {
    free_clusters = total / 2;
  }
  uint64_t bsize = mnt->bytes_per_cluster ? mnt->bytes_per_cluster : 4096;
  buf->f_type = MSDOS_SUPER_MAGIC;
  buf->f_bsize = (int64_t)bsize;
  buf->f_blocks = (int64_t)total;
  buf->f_bfree = (int64_t)free_clusters;
  buf->f_bavail = (int64_t)free_clusters;
  buf->f_files = (int64_t)total;
  buf->f_ffree = (int64_t)free_clusters;
  buf->f_fsid[0] = 0;
  buf->f_fsid[1] = 0;
  buf->f_namelen = 255;
  buf->f_frsize = (int64_t)bsize;
  buf->f_flags = (mnt->fat_type != 32) ? 1 : 0;
  return 0;
}

// Write to a file
static uint32_t fat32_write_impl(vfs_node_t *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
  fat32_mount_t *mnt = node ? (fat32_mount_t *)node->device : NULL;
  if (!mnt || !buffer)
    return (uint32_t)-14;
  if (mnt->fat_type != 32)
    return (uint32_t)-30; // -EROFS
  if (size == 0)
    return 0;

  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint32_t orig_cluster = node->impl;
  uint32_t cluster = orig_cluster;

  // Handle new file (no cluster allocated yet)
  if (cluster == 0) {
    cluster = fat32_alloc_cluster(mnt, 0);
    if (cluster == 0)
      return (uint32_t)-28;
    node->impl = cluster;
  }

  // Allocate cluster buffer
  uint8_t *cluster_buf = kmalloc(cluster_size);
  if (!cluster_buf) {
    if (orig_cluster == 0) {
      fat32_free_cluster_chain(mnt, node->impl);
      node->impl = 0;
    }
    return (uint32_t)-12;
  }

  uint32_t bytes_written = 0;
  uint32_t current_offset = 0;

  // Walk to the correct cluster, allocating as needed
  while (current_offset + cluster_size <= offset) {
    uint32_t next = fat32_get_next_cluster(mnt, cluster);

    if (!FAT32_IS_VALID(next)) {
      next = fat32_alloc_cluster(mnt, cluster);
      if (next == 0) {
        kfree(cluster_buf);
        if (orig_cluster == 0) {
          fat32_free_cluster_chain(mnt, node->impl);
          node->impl = 0;
        }
        return (uint32_t)-28;
      }
    }

    cluster = next;
    current_offset += cluster_size;
  }

  // Now write the data
  while (bytes_written < size) {
    uint32_t cluster_offset = (offset + bytes_written) - current_offset;
    uint32_t to_copy = cluster_size - cluster_offset;
    if (to_copy > size - bytes_written) {
      to_copy = size - bytes_written;
    }

    if (cluster_offset != 0 || to_copy < cluster_size) {
      if (fat32_read_cluster(mnt, cluster, cluster_buf) != 0) {
        memset(cluster_buf, 0, cluster_size);
      }
    }

    memcpy(cluster_buf + cluster_offset, buffer + bytes_written, to_copy);

    if (fat32_write_cluster(mnt, cluster, cluster_buf) != 0) {
      break;
    }

    bytes_written += to_copy;
    current_offset += cluster_size;

    if (bytes_written < size) {
      uint32_t next = fat32_get_next_cluster(mnt, cluster);
      if (!FAT32_IS_VALID(next)) {
        next = fat32_alloc_cluster(mnt, cluster);
        if (next == 0)
          break;
      }
      cluster = next;
    }
  }

  kfree(cluster_buf);

  if (bytes_written == 0) {
    if (orig_cluster == 0 && FAT32_IS_VALID(node->impl)) {
      fat32_free_cluster_chain(mnt, node->impl);
      node->impl = 0;
    }
    return (uint32_t)-28;
  }

  // Update file size if we extended it
  if (offset + bytes_written > node->length) {
    node->length = offset + bytes_written;
  }

  if (((fat32_node_t *)node)->has_dir_entry) {
    fat32_update_dir_entry(node);
  }

  return bytes_written;
}

// Get current time for directory entry (simplified - use fixed date)
static void get_fat_time(uint16_t *date, uint16_t *time) {
  // Fixed date: May 6, 2026 = 0x5A06 (year=46, month=5, day=6)
  *date = (46 << 9) | (5 << 5) | 6;  // Year from 1980, month, day
  *time = (12 << 11) | (0 << 5) | 0; // Hour, minute, second/2
}

// Create a new file or directory in `parent`
static int fat32_create_entry(vfs_node_t *parent, char *name, uint8_t attr) {
  fat32_mount_t *mnt = parent ? (fat32_mount_t *)parent->device : NULL;
  if (!mnt || !fat32_validate_filename(name))
    return -22; // -EINVAL
  if (mnt->fat_type != 32)
    return -30; // -EROFS

  uint32_t dir_cluster = parent->impl;
  if (dir_cluster == 0)
    dir_cluster = mnt->root_cluster;

  fat32_lookup_t existing;
  if (fat32_dir_lookup_raw(mnt, dir_cluster, name, &existing) == 0 &&
      existing.found) {
    return -17; // -EEXIST
  }

  fat32_dir_entry_t tpl;
  memset(&tpl, 0, sizeof(tpl));
  tpl.attr = attr;
  uint16_t cdate = 0, ctime = 0;
  get_fat_time(&cdate, &ctime);
  tpl.create_date = cdate;
  tpl.create_time = ctime;
  tpl.last_access_date = cdate;
  tpl.modify_date = cdate;
  tpl.modify_time = ctime;

  uint32_t sub = 0;
  if (attr & FAT32_ATTR_DIRECTORY) {
    sub = fat32_alloc_cluster(mnt, 0);
    uint8_t *sub_buf = sub ? kmalloc(mnt->bytes_per_cluster) : NULL;
    if (!sub_buf) {
      if (sub)
        fat32_free_cluster_chain(mnt, sub);
      return -28;
    }
    memset(sub_buf, 0, mnt->bytes_per_cluster);
    fat32_dir_entry_t *dot = (fat32_dir_entry_t *)sub_buf;
    /* ".." of a root-level directory points at cluster 0 by the FAT spec. */
    uint32_t up = dir_cluster == mnt->root_cluster ? 0 : dir_cluster;
    for (int i = 0; i < 2; i++) {
      memset(dot[i].name, ' ', 11);
      dot[i].name[0] = '.';
      if (i == 1)
        dot[i].name[1] = '.';
      uint32_t c = i == 0 ? sub : up;
      dot[i].attr = FAT32_ATTR_DIRECTORY;
      dot[i].create_date = tpl.create_date;
      dot[i].create_time = tpl.create_time;
      dot[i].modify_date = tpl.modify_date;
      dot[i].modify_time = tpl.modify_time;
      dot[i].last_access_date = tpl.create_date;
      dot[i].cluster_high = (uint16_t)(c >> 16);
      dot[i].cluster_low = (uint16_t)(c & 0xFFFF);
    }
    int wr = fat32_write_cluster(mnt, sub, sub_buf);
    kfree(sub_buf);
    if (wr != 0) {
      fat32_free_cluster_chain(mnt, sub);
      return -5;
    }
    tpl.cluster_high = (uint16_t)(sub >> 16);
    tpl.cluster_low = (uint16_t)(sub & 0xFFFF);
  }

  if (fat32_dir_insert_entry(mnt, dir_cluster, name, &tpl) != 0) {
    if (sub)
      fat32_free_cluster_chain(mnt, sub);
    return -28;
  }

  return 0;
}

static int fat32_rename_impl(vfs_node_t *dir, char *old_name, char *new_name) {
  fat32_mount_t *mnt = dir ? (fat32_mount_t *)dir->device : NULL;
  if (!mnt)
    return -22;
  if (mnt->fat_type != 32)
    return -30; // -EROFS
  if (!fat32_validate_filename(old_name) || !fat32_validate_filename(new_name))
    return -22; // -EINVAL

  uint32_t dir_cluster = dir->impl;
  if (dir_cluster == 0)
    dir_cluster = mnt->root_cluster;

  fat32_lookup_t old_res;
  if (fat32_dir_lookup_raw(mnt, dir_cluster, old_name, &old_res) != 0 ||
      !old_res.found)
    return -2; // -ENOENT

  fat32_lookup_t new_res;
  uint32_t replaced_cluster = 0;
  bool delete_new_entry = false;
  bool same_entry = false;

  if (fat32_dir_lookup_raw(mnt, dir_cluster, new_name, &new_res) == 0 &&
      new_res.found) {
    if (new_res.sfn_cluster == old_res.sfn_cluster &&
        new_res.sfn_index == old_res.sfn_index) {
      if (strcmp(old_name, new_name) == 0)
        return 0;
      same_entry = true;
    } else {
      bool old_is_dir = (old_res.sfn_entry.attr & FAT32_ATTR_DIRECTORY) != 0;
      bool new_is_dir = (new_res.sfn_entry.attr & FAT32_ATTR_DIRECTORY) != 0;
      if (old_is_dir && !new_is_dir)
        return -20; // -ENOTDIR
      if (!old_is_dir && new_is_dir)
        return -21; // -EISDIR
      if (new_is_dir) {
        uint32_t nc = fat_entry_cluster(mnt, &new_res.sfn_entry);
        if (FAT32_IS_VALID(nc) && !fat32_dir_is_empty(mnt, nc))
          return -39; // -ENOTEMPTY
      }
      replaced_cluster = fat_entry_cluster(mnt, &new_res.sfn_entry);
      delete_new_entry = true;
    }
  }

  if (same_entry) {
    if (fat32_dir_mark_deleted(mnt, old_res.lfn_slots, old_res.lfn_count,
                               old_res.sfn_cluster, old_res.sfn_index) != 0)
      return -5;
    if (fat32_dir_insert_entry(mnt, dir_cluster, new_name,
                               &old_res.sfn_entry) != 0)
      return -28;
    return 0;
  }

  if (delete_new_entry) {
    if (fat32_dir_mark_deleted(mnt, new_res.lfn_slots, new_res.lfn_count,
                               new_res.sfn_cluster, new_res.sfn_index) != 0)
      return -5;
  }

  if (fat32_dir_insert_entry(mnt, dir_cluster, new_name, &old_res.sfn_entry) !=
      0)
    return -28;

  if (fat32_dir_mark_deleted(mnt, old_res.lfn_slots, old_res.lfn_count,
                             old_res.sfn_cluster, old_res.sfn_index) != 0)
    return -5;

  if (FAT32_IS_VALID(replaced_cluster)) {
    fat32_free_cluster_chain(mnt, replaced_cluster);
  }

  return 0;
}

static int fat32_create_impl(vfs_node_t *parent, char *name,
                             uint16_t permission) {
  (void)permission;
  return fat32_create_entry(parent, name, 0);
}

static int fat32_mkdir_impl(vfs_node_t *parent, char *name,
                            uint16_t permission) {
  (void)permission;
  return fat32_create_entry(parent, name, FAT32_ATTR_DIRECTORY);
}

// VFS Operations

// Read from a file
static uint32_t fat32_read_impl(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  fat32_mount_t *mnt = (fat32_mount_t *)node->device;
  if (!mnt)
    return 0;

  uint32_t file_size = node->length;
  if (offset >= file_size)
    return 0;
  if (offset + size > file_size) {
    size = file_size - offset;
  }

  uint32_t cluster = node->impl; // First cluster stored in impl
  uint32_t cluster_size = mnt->bytes_per_cluster;

  // Allocate a cluster buffer
  uint8_t *cluster_buf = kmalloc(cluster_size);
  if (!cluster_buf)
    return 0;

  uint32_t bytes_read = 0;
  uint32_t current_offset = 0;

  // Walk to the correct cluster
  while (fat_is_valid_cluster(mnt, cluster) &&
         current_offset + cluster_size <= offset) {
    cluster = fat32_get_next_cluster(mnt, cluster);
    current_offset += cluster_size;
  }

  // Now read the data
  while (fat_is_valid_cluster(mnt, cluster) && bytes_read < size) {
    if (fat32_read_cluster(mnt, cluster, cluster_buf) != 0) {
      break;
    }

    uint32_t cluster_offset = (offset + bytes_read) - current_offset;
    uint32_t to_copy = cluster_size - cluster_offset;
    if (to_copy > size - bytes_read) {
      to_copy = size - bytes_read;
    }

    memcpy(buffer + bytes_read, cluster_buf + cluster_offset, to_copy);
    bytes_read += to_copy;
    current_offset += cluster_size;

    cluster = fat32_get_next_cluster(mnt, cluster);
  }

  kfree(cluster_buf);
  return bytes_read;
}

// Forward declarations
static struct dirent *fat32_readdir_impl(vfs_node_t *node, uint64_t index);
static vfs_node_t *fat32_finddir_impl(vfs_node_t *node, char *name);

// Create a VFS node from a directory entry
static vfs_node_t *fat32_make_vfs_node(fat32_mount_t *mnt,
                                       fat32_dir_entry_t *entry,
                                       uint32_t cluster, char *lfn_name,
                                       uint32_t parent_cluster,
                                       uint32_t entry_cluster,
                                       uint32_t entry_index) {
  fat32_node_t *fn = kmalloc(sizeof(fat32_node_t));
  if (!fn)
    return NULL;
  memset(fn, 0, sizeof(fat32_node_t));
  vfs_node_t *node = &fn->vfs;
  vfs_node_init(node);

  fn->parent_cluster = parent_cluster;
  fn->entry_cluster = entry_cluster;
  fn->entry_index = entry_index;
  fn->has_dir_entry = (entry_cluster != 0);

  // Set name - use LFN if available, otherwise SFN
  if (lfn_name && lfn_name[0]) {
    int _lfn_len = strlen(lfn_name);
    if (_lfn_len > 127)
      _lfn_len = 127;
    memcpy(node->name, lfn_name, _lfn_len);
    node->name[_lfn_len] = '\0';
  } else {
    fat32_sfn_to_str(entry->name, node->name);
  }

  node->impl = cluster;
  node->length = entry->file_size;
  node->device = mnt;
  node->inode = cluster; // Use cluster as inode number

  // Parse times
  node->atime = fat_date_to_unix(entry->last_access_date, 0);
  node->mtime = fat_date_to_unix(entry->modify_date, entry->modify_time);
  node->ctime = fat_date_to_unix(entry->create_date, entry->create_time);

  bool readonly = (mnt->fat_type != 32);

  // Set flags and operations based on attributes
  if (entry->attr & FAT32_ATTR_DIRECTORY) {
    node->flags = FS_DIRECTORY | (readonly ? FS_READONLY : 0);
    node->readdir = fat32_readdir_impl;
    node->finddir = fat32_finddir_impl;
    if (!readonly) {
      node->create = fat32_create_impl;
      node->mkdir = fat32_mkdir_impl;
      node->unlink = fat32_unlink_impl;
      node->rmdir = fat32_rmdir_impl;
      node->rename = fat32_rename_impl;
    }
  } else {
    node->flags = FS_FILE | FS_PAGE_CACHE | (readonly ? FS_READONLY : 0);
    node->read = fat32_read_impl;
    if (!readonly) {
      node->write = fat32_write_impl;
      node->unlink = fat32_unlink_impl;
      node->truncate = fat32_truncate_impl;
    }
  }

  // Set permissions (FAT doesn't have Unix permissions, use defaults)
  node->mask = 0755;
  if (!readonly && !(entry->attr & FAT32_ATTR_READ_ONLY)) {
    node->mask |= 0222; // Add write permission
  }
  node->statfs = fat32_statfs_impl;

  /* FS_AUTODESTROY makes vfs_close() run ->destroy on the last close, so
   * the count is returned even when the node is not retired. */
  node->flags |= FS_AUTODESTROY;
  node->destroy = fat32_node_destroy;
  __atomic_add_fetch(&mnt->refs, 1, __ATOMIC_ACQ_REL);

  return node;
}

// LFN Support

// Calculate checksum for LFN entries
static uint8_t lfn_checksum(uint8_t *sfn) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++) {
    sum = ((sum & 1) << 7) | (sum >> 1);
    sum += sfn[i];
  }
  return sum;
}

// Decode LFN entries into a string
static int decode_lfn(fat32_lfn_entry_t *entries, int count, char *out,
                      size_t out_cap, uint8_t *sfn) {
  if (!out || out_cap == 0)
    return -1;
  uint8_t expected_checksum = lfn_checksum(sfn);
  size_t out_pos = 0;
  bool done = false;

  // Entries are stored by sequence number: entries[0] = seq 1, entries[1] = seq
  // 2, etc. Process in forward order (seq 1 first, then seq 2, etc.)
  for (int i = 0; i < count && !done; i++) {
    fat32_lfn_entry_t *lfn = &entries[i];

    // Verify checksum
    if (lfn->checksum != expected_checksum) {
      out[0] = '\0';
      return -1;
    }

    for (int j = 0; j < 5 && !done; j++) {
      uint16_t ch = lfn->name1[j];
      if (ch == 0 || ch == 0xFFFF) {
        done = true;
        break;
      }
      if (out_pos + 1 < out_cap)
        out[out_pos++] = (char)(ch & 0xFF);
    }
    for (int j = 0; j < 6 && !done; j++) {
      uint16_t ch = lfn->name2[j];
      if (ch == 0 || ch == 0xFFFF) {
        done = true;
        break;
      }
      if (out_pos + 1 < out_cap)
        out[out_pos++] = (char)(ch & 0xFF);
    }
    for (int j = 0; j < 2 && !done; j++) {
      uint16_t ch = lfn->name3[j];
      if (ch == 0 || ch == 0xFFFF) {
        done = true;
        break;
      }
      if (out_pos + 1 < out_cap)
        out[out_pos++] = (char)(ch & 0xFF);
    }
  }

  out[out_pos] = '\0';
  return (int)out_pos;
}

// Directory Operations

static uint32_t fat_entry_cluster(const fat32_mount_t *mnt,
                                  const fat32_dir_entry_t *entry) {
  uint32_t c = entry->cluster_low;
  if (mnt->fat_type == 32)
    c |= ((uint32_t)entry->cluster_high << 16);
  return c;
}

// Context for directory iteration
typedef struct {
  fat32_mount_t *mnt;
  bool is_fixed_root;                // FAT12/16 fixed root directory
  uint32_t cluster;                  // Current cluster being read
  uint32_t root_sector;              // Next fixed root sector to read
  uint32_t root_sectors_left;        // Remaining fixed root sectors
  uint32_t entries_in_buf;           // Entries per loaded buffer
  uint32_t entry_index;              // Entry index within directory
  uint32_t cluster_offset;           // Offset within current buffer
  uint8_t *cluster_buf;              // Buffer for current cluster/sector
  fat32_lfn_entry_t lfn_entries[20]; // Buffer for LFN entries
  int lfn_count;                     // Number of LFN entries accumulated
} dir_iter_t;

// Initialize a directory iterator
static int dir_iter_init(dir_iter_t *iter, fat32_mount_t *mnt,
                         uint32_t cluster) {
  memset(iter, 0, sizeof(*iter));
  iter->mnt = mnt;
  iter->cluster = cluster;

  if (mnt->fat_type != 32 && cluster == 0) {
    if (mnt->root_dir_sectors == 0)
      return -1;
    iter->is_fixed_root = true;
    iter->entries_in_buf = mnt->bytes_per_sector / sizeof(fat32_dir_entry_t);
    iter->cluster_buf = kmalloc(mnt->bytes_per_sector);
    if (!iter->cluster_buf)
      return -1;
    if (fat_rw_bpb_sectors(mnt, mnt->root_dir_start, 1, iter->cluster_buf,
                           false) != 0) {
      kfree(iter->cluster_buf);
      iter->cluster_buf = NULL;
      return -1;
    }
    iter->root_sector = mnt->root_dir_start + 1u;
    iter->root_sectors_left = mnt->root_dir_sectors - 1u;
    return 0;
  }

  iter->entries_in_buf = mnt->bytes_per_cluster / sizeof(fat32_dir_entry_t);
  iter->cluster_buf = kmalloc(mnt->bytes_per_cluster);
  if (!iter->cluster_buf)
    return -1;

  if (fat32_read_cluster(mnt, cluster, iter->cluster_buf) != 0) {
    kfree(iter->cluster_buf);
    iter->cluster_buf = NULL;
    return -1;
  }

  return 0;
}

// Free a directory iterator
static void dir_iter_free(dir_iter_t *iter) {
  if (iter->cluster_buf) {
    kfree(iter->cluster_buf);
    iter->cluster_buf = NULL;
  }
}

// Get the next directory entry
static fat32_dir_entry_t *dir_iter_next(dir_iter_t *iter) {
  while (true) {
    // Check if we need to move to next cluster/sector
    if (iter->cluster_offset >= iter->entries_in_buf) {
      if (iter->is_fixed_root) {
        if (iter->root_sectors_left == 0)
          return NULL;
        if (fat_rw_bpb_sectors(iter->mnt, iter->root_sector, 1,
                               iter->cluster_buf, false) != 0)
          return NULL;
        iter->root_sector++;
        iter->root_sectors_left--;
        iter->cluster_offset = 0;
      } else {
        iter->cluster = fat32_get_next_cluster(iter->mnt, iter->cluster);
        if (!fat_is_valid_cluster(iter->mnt, iter->cluster)) {
          return NULL; // End of directory or error
        }
        iter->cluster_offset = 0;

        if (fat32_read_cluster(iter->mnt, iter->cluster, iter->cluster_buf) !=
            0) {
          return NULL;
        }
      }
    }

    fat32_dir_entry_t *entry =
        (fat32_dir_entry_t *)iter->cluster_buf + iter->cluster_offset;
    iter->cluster_offset++;
    iter->entry_index++;

    // Check for end of directory
    if (is_end_of_directory(entry)) {
      return NULL;
    }

    // Skip deleted entries
    if (is_deleted_entry(entry)) {
      iter->lfn_count = 0; // Reset LFN accumulation
      continue;
    }

    return entry;
  }
}

// Read a directory entry
static struct dirent *fat32_readdir_impl(vfs_node_t *node, uint64_t index) {
  fat32_mount_t *mnt = (fat32_mount_t *)node->device;
  if (!mnt)
    return NULL;

  uint32_t cluster = node->impl;
  if (cluster == 0)
    cluster = mnt->root_cluster;

  dir_iter_t iter;
  if (dir_iter_init(&iter, mnt, cluster) != 0) {
    return NULL;
  }

  struct dirent *dirent = NULL;
  struct dirent *dre = vfs_readdir_buffer();
  memset(dre, 0, sizeof(*dre));
  uint32_t current_index = 0;

  while (true) {
    fat32_dir_entry_t *entry = dir_iter_next(&iter);
    if (!entry)
      break;

    // Handle LFN entries
    if (is_lfn_entry(entry)) {
      fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)entry;
      int seq = lfn->seq & 0x1F;

      if (seq > 0 && seq <= 20) {
        // Store in reverse order
        iter.lfn_entries[seq - 1] = *lfn;
        if (lfn->seq & FAT32_LFN_LAST_ENTRY) {
          iter.lfn_count = seq;
        }
      }
      continue;
    }

    // Skip volume labels
    if (entry->attr & FAT32_ATTR_VOLUME_ID) {
      iter.lfn_count = 0;
      continue;
    }

    if (current_index == index) {
      dirent = dre;
      {
        // Decode LFN if present
        char lfn_name[261] = {0};
        if (iter.lfn_count > 0) {
          decode_lfn(iter.lfn_entries, iter.lfn_count, lfn_name,
                     sizeof(lfn_name), entry->name);
        }

        // Set name
        if (lfn_name[0]) {
          int _lfn_len = strlen(lfn_name);
          if (_lfn_len > 127)
            _lfn_len = 127;
          memcpy(dirent->name, lfn_name, _lfn_len);
          dirent->name[_lfn_len] = '\0';
        } else {
          fat32_sfn_to_str(entry->name, dirent->name);
        }

        uint32_t entry_cluster = fat_entry_cluster(mnt, entry);
        dirent->ino = entry_cluster ? entry_cluster : mnt->root_cluster;
        dirent->d_type = (entry->attr & FAT32_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
      }
      break;
    }

    current_index++;
    iter.lfn_count = 0;
  }

  dir_iter_free(&iter);
  return dirent;
}

/* Return one on-disk alias pair using the same indexed cursor as readdir.
 * Local result storage avoids the legacy readdir static-buffer race. */
int fat32_read_aliases(vfs_node_t *node, uint32_t index, bool short_only,
                       struct fat32_ioctl_dirent result[2]) {
  if (!node || node->readdir != fat32_readdir_impl)
    return -25; /* ENOTTY: only FAT directories have stored short aliases */
  fat32_mount_t *mnt = node->device;
  if (!mnt)
    return -5;
  dir_iter_t iter;
  if (dir_iter_init(&iter, mnt, node->impl ? node->impl : mnt->root_cluster))
    return -5;
  memset(result, 0, sizeof(*result) * 2);
  uint32_t ordinal = 0;
  int ret = 0;
  fat32_dir_entry_t *entry;
  while ((entry = dir_iter_next(&iter))) {
    if (is_lfn_entry(entry)) {
      fat32_lfn_entry_t *lfn = (void *)entry;
      unsigned seq = lfn->seq & 0x1f;
      if (seq && seq <= 20) {
        iter.lfn_entries[seq - 1] = *lfn;
        if (lfn->seq & FAT32_LFN_LAST_ENTRY)
          iter.lfn_count = seq;
      }
      continue;
    }
    if (entry->attr & FAT32_ATTR_VOLUME_ID) {
      iter.lfn_count = 0;
      continue;
    }
    if (ordinal++ == index) {
      unsigned len = 0;
      for (unsigned i = 0; i < 8 && entry->name[i] != ' '; i++)
        result[0].name[len++] = entry->name[i];
      if (entry->name[8] != ' ') {
        result[0].name[len++] = '.';
        for (unsigned i = 8; i < 11 && entry->name[i] != ' '; i++)
          result[0].name[len++] = entry->name[i];
      }
      /* FAT encodes an initial 0xe5 byte as 0x05 to avoid the deletion marker. */
      if ((unsigned char)result[0].name[0] == 0x05)
        result[0].name[0] = (char)0xe5;
      result[0].reclen = len;
      result[0].offset = (uint64_t)index + 1;
      result[0].ino = fat_entry_cluster(mnt, entry);
      if (!short_only && iter.lfn_count) {
        /* The legacy decoder can emit all 20*13 UTF-16 slots from a
         * malformed chain. Keep that scratch space outside the ioctl ABI,
         * and ignore invalid/checksum-failed names rather than overflowing. */
        char decoded[20 * 13 + 1] = {0};
        int length = decode_lfn(iter.lfn_entries, iter.lfn_count, decoded,
                                sizeof(decoded), entry->name);
        if (length > 0 && length < (int)sizeof(result[1].name)) {
          memcpy(result[1].name, decoded, (size_t)length + 1);
          result[1].reclen = (uint16_t)length;
          result[1].offset = result[0].offset;
          result[1].ino = result[0].ino;
        }
      }
      ret = 1;
      break;
    }
    iter.lfn_count = 0;
  }
  dir_iter_free(&iter);
  return ret;
}

static int fat32_dir_lookup_raw(fat32_mount_t *mnt, uint32_t dir_cluster,
                                const char *name, fat32_lookup_t *out) {
  if (!mnt || !name || !out)
    return -1;
  memset(out, 0, sizeof(*out));

  dir_iter_t iter;
  if (dir_iter_init(&iter, mnt, dir_cluster) != 0)
    return -1;

  fat32_dir_slot_t lfn_slots[20];
  memset(lfn_slots, 0, sizeof(lfn_slots));

  while (true) {
    fat32_dir_entry_t *entry = dir_iter_next(&iter);
    if (!entry)
      break;

    if (is_lfn_entry(entry)) {
      fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)entry;
      int seq = lfn->seq & 0x1F;
      if (seq > 0 && seq <= 20) {
        if (lfn->seq & FAT32_LFN_LAST_ENTRY) {
          memset(lfn_slots, 0, sizeof(lfn_slots));
          iter.lfn_count = seq;
        }
        iter.lfn_entries[seq - 1] = *lfn;
        lfn_slots[seq - 1].cluster = iter.cluster;
        lfn_slots[seq - 1].index = iter.cluster_offset - 1u;
      }
      continue;
    }

    if (entry->attr & FAT32_ATTR_VOLUME_ID) {
      iter.lfn_count = 0;
      continue;
    }

    char lfn_name[261] = {0};
    int lfn_len = 0;
    if (iter.lfn_count > 0) {
      lfn_len = decode_lfn(iter.lfn_entries, iter.lfn_count, lfn_name,
                           sizeof(lfn_name), entry->name);
      if (lfn_len < 0)
        lfn_name[0] = '\0';
    }

    char sfn_name[13];
    fat32_sfn_to_str(entry->name, sfn_name);

    bool match = false;
    if (lfn_name[0])
      match = (strcasecmp(name, lfn_name) == 0);
    if (!match)
      match = (strcasecmp(name, sfn_name) == 0);

    if (match) {
      out->found = true;
      out->sfn_entry = *entry;
      out->sfn_cluster = iter.cluster;
      out->sfn_index = iter.cluster_offset - 1u;
      if (lfn_len > 0 && iter.lfn_count > 0 && iter.lfn_count <= 20) {
        out->lfn_count = iter.lfn_count;
        memcpy(out->lfn_slots, lfn_slots,
               sizeof(fat32_dir_slot_t) * (size_t)iter.lfn_count);
        memcpy(out->lfn_name, lfn_name, (size_t)lfn_len + 1);
      }
      break;
    }

    iter.lfn_count = 0;
  }

  dir_iter_free(&iter);
  return 0;
}

static bool fat32_dir_is_empty(fat32_mount_t *mnt, uint32_t dir_cluster) {
  dir_iter_t iter;
  if (dir_iter_init(&iter, mnt, dir_cluster) != 0)
    return false;

  bool empty = true;
  fat32_dir_entry_t *entry;
  while ((entry = dir_iter_next(&iter)) != NULL) {
    if (is_lfn_entry(entry) || (entry->attr & FAT32_ATTR_VOLUME_ID))
      continue;
    char sfn[13];
    fat32_sfn_to_str(entry->name, sfn);
    if (strcmp(sfn, ".") != 0 && strcmp(sfn, "..") != 0) {
      empty = false;
      break;
    }
  }
  dir_iter_free(&iter);
  return empty;
}

static int fat32_dir_mark_deleted(fat32_mount_t *mnt,
                                  const fat32_dir_slot_t *lfn_slots,
                                  int lfn_count, uint32_t sfn_cluster,
                                  uint32_t sfn_index) {
  if (!mnt || mnt->fat_type != 32 || !FAT32_IS_VALID(sfn_cluster))
    return -1;

  fat32_dir_slot_t all_slots[21];
  int total = 0;
  for (int i = 0; i < lfn_count && i < 20; i++) {
    if (FAT32_IS_VALID(lfn_slots[i].cluster)) {
      all_slots[total++] = lfn_slots[i];
    }
  }
  all_slots[total].cluster = sfn_cluster;
  all_slots[total].index = sfn_index;
  total++;

  uint32_t entries_per_cluster =
      mnt->bytes_per_cluster / sizeof(fat32_dir_entry_t);
  uint8_t *buf = kmalloc(mnt->bytes_per_cluster);
  if (!buf)
    return -1;

  uint32_t cur_cluster = 0;
  bool dirty = false;
  int rc = 0;

  for (int i = 0; i < total; i++) {
    if (all_slots[i].index >= entries_per_cluster) {
      rc = -1;
      break;
    }
    if (all_slots[i].cluster != cur_cluster) {
      if (dirty) {
        if (fat32_write_cluster(mnt, cur_cluster, buf) != 0) {
          rc = -1;
          break;
        }
        dirty = false;
      }
      cur_cluster = all_slots[i].cluster;
      if (fat32_read_cluster(mnt, cur_cluster, buf) != 0) {
        rc = -1;
        break;
      }
    }
    fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf + all_slots[i].index;
    e->name[0] = 0xE5;
    dirty = true;
  }

  if (rc == 0 && dirty) {
    if (fat32_write_cluster(mnt, cur_cluster, buf) != 0)
      rc = -1;
  }

  kfree(buf);
  return rc;
}

static bool fat32_sfn_exists(fat32_mount_t *mnt, uint32_t dir_cluster,
                             const uint8_t sfn[11]) {
  dir_iter_t iter;
  if (dir_iter_init(&iter, mnt, dir_cluster) != 0)
    return false;

  bool exists = false;
  fat32_dir_entry_t *entry;
  while ((entry = dir_iter_next(&iter)) != NULL) {
    if (is_lfn_entry(entry) || (entry->attr & FAT32_ATTR_VOLUME_ID))
      continue;
    if (memcmp(entry->name, sfn, 11) == 0) {
      exists = true;
      break;
    }
  }
  dir_iter_free(&iter);
  return exists;
}

static void fat32_generate_sfn(fat32_mount_t *mnt, uint32_t dir_cluster,
                               const char *name, uint8_t sfn[11]) {
  memset(sfn, ' ', 11);

  if (fat32_is_strict_8_3(name)) {
    const char *dot = strchr(name, '.');
    int base_len = dot ? (int)(dot - name) : (int)strlen(name);
    for (int i = 0; i < base_len; i++)
      sfn[i] = (uint8_t)name[i];
    if (dot) {
      dot++;
      for (int i = 0; dot[i] && i < 3; i++)
        sfn[8 + i] = (uint8_t)dot[i];
    }
    return;
  }

  const char *last_dot = strrchr(name, '.');
  if (last_dot == name)
    last_dot = NULL;

  char base[8];
  int base_len = 0;
  bool lossy = false;
  for (const char *p = name; *p && p != last_dot; p++) {
    if (*p == ' ' || *p == '.') {
      lossy = true;
      continue;
    }
    char u = fat32_to_upper(*p);
    if (!fat32_is_valid_sfn_char(u)) {
      u = '_';
      lossy = true;
    }
    if (base_len < 8)
      base[base_len++] = u;
    else
      lossy = true;
  }
  if (base_len == 0) {
    base[0] = '_';
    base_len = 1;
    lossy = true;
  }

  char ext[3];
  int ext_len = 0;
  if (last_dot && last_dot[1]) {
    for (const char *p = last_dot + 1; *p; p++) {
      if (*p == ' ' || *p == '.') {
        lossy = true;
        continue;
      }
      char u = fat32_to_upper(*p);
      if (!fat32_is_valid_sfn_char(u)) {
        u = '_';
        lossy = true;
      }
      if (ext_len < 3)
        ext[ext_len++] = u;
      else
        lossy = true;
    }
  } else if (last_dot) {
    lossy = true;
  }

  if (!lossy) {
    for (int i = 0; i < base_len; i++)
      sfn[i] = (uint8_t)base[i];
    for (int i = 0; i < ext_len; i++)
      sfn[8 + i] = (uint8_t)ext[i];
    if (!fat32_sfn_exists(mnt, dir_cluster, sfn))
      return;
  }

  for (uint32_t n = 1; n <= 999999; n++) {
    char tail[8];
    int tlen = snprintf(tail, sizeof(tail), "~%u", n);
    int keep = 8 - tlen;
    if (keep > base_len)
      keep = base_len;
    memset(sfn, ' ', 11);
    for (int i = 0; i < keep; i++)
      sfn[i] = (uint8_t)base[i];
    for (int i = 0; i < tlen; i++)
      sfn[keep + i] = (uint8_t)tail[i];
    for (int i = 0; i < ext_len; i++)
      sfn[8 + i] = (uint8_t)ext[i];
    if (!fat32_sfn_exists(mnt, dir_cluster, sfn))
      return;
  }
}

static uint16_t fat32_lfn_char(const char *name, int name_len, int idx) {
  if (idx < name_len)
    return (uint16_t)(uint8_t)name[idx];
  if (idx == name_len)
    return 0x0000;
  return 0xFFFF;
}

static int fat32_dir_insert_entry(fat32_mount_t *mnt, uint32_t dir_cluster,
                                  const char *name,
                                  const fat32_dir_entry_t *tpl) {
  if (!mnt || mnt->fat_type != 32 || !FAT32_IS_VALID(dir_cluster) || !name ||
      !tpl)
    return -1;

  int name_len = (int)strlen(name);
  if (name_len <= 0 || name_len > 255)
    return -1;

  bool need_lfn = !fat32_is_strict_8_3(name);
  int num_lfn = need_lfn ? (name_len + 12) / 13 : 0;
  int slots_needed = num_lfn + 1;

  uint8_t sfn[11];
  fat32_generate_sfn(mnt, dir_cluster, name, sfn);

  fat32_dir_entry_t entries[21];
  memset(entries, 0, sizeof(entries));

  if (num_lfn > 0) {
    uint8_t csum = lfn_checksum(sfn);
    for (int k = 0; k < num_lfn; k++) {
      int seq = num_lfn - k;
      fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&entries[k];
      lfn->seq = (uint8_t)(seq | (k == 0 ? FAT32_LFN_LAST_ENTRY : 0));
      lfn->attr = FAT32_ATTR_LFN;
      lfn->type = 0;
      lfn->checksum = csum;
      lfn->reserved = 0;
      int base_idx = (seq - 1) * 13;
      for (int j = 0; j < 5; j++)
        lfn->name1[j] = fat32_lfn_char(name, name_len, base_idx + j);
      for (int j = 0; j < 6; j++)
        lfn->name2[j] = fat32_lfn_char(name, name_len, base_idx + 5 + j);
      for (int j = 0; j < 2; j++)
        lfn->name3[j] = fat32_lfn_char(name, name_len, base_idx + 11 + j);
    }
  }
  entries[num_lfn] = *tpl;
  memcpy(entries[num_lfn].name, sfn, 11);

  uint32_t entries_per_cluster =
      mnt->bytes_per_cluster / sizeof(fat32_dir_entry_t);
  uint8_t *cluster_buf = kmalloc(mnt->bytes_per_cluster);
  if (!cluster_buf)
    return -1;

  fat32_dir_slot_t slots[21];
  int run = 0;
  uint32_t cur_cluster = dir_cluster;
  uint32_t last_cluster = dir_cluster;

  while (run < slots_needed && FAT32_IS_VALID(cur_cluster)) {
    if (fat32_read_cluster(mnt, cur_cluster, cluster_buf) != 0) {
      kfree(cluster_buf);
      return -1;
    }

    for (uint32_t i = 0; i < entries_per_cluster && run < slots_needed; i++) {
      fat32_dir_entry_t *e = (fat32_dir_entry_t *)cluster_buf + i;
      if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
        slots[run].cluster = cur_cluster;
        slots[run].index = i;
        run++;
      } else {
        run = 0;
      }
    }

    if (run >= slots_needed)
      break;

    last_cluster = cur_cluster;
    uint32_t next = fat32_get_next_cluster(mnt, cur_cluster);
    if (!FAT32_IS_VALID(next)) {
      next = fat32_alloc_cluster(mnt, last_cluster);
      if (next == 0) {
        kfree(cluster_buf);
        return -1;
      }
    }
    cur_cluster = next;
  }

  if (run < slots_needed) {
    kfree(cluster_buf);
    return -1;
  }

  uint32_t write_cluster = 0;
  bool dirty = false;
  int rc = 0;
  for (int i = 0; i < slots_needed; i++) {
    if (slots[i].cluster != write_cluster) {
      if (dirty) {
        if (fat32_write_cluster(mnt, write_cluster, cluster_buf) != 0) {
          rc = -1;
          break;
        }
        dirty = false;
      }
      write_cluster = slots[i].cluster;
      if (fat32_read_cluster(mnt, write_cluster, cluster_buf) != 0) {
        rc = -1;
        break;
      }
    }
    fat32_dir_entry_t *dst = (fat32_dir_entry_t *)cluster_buf + slots[i].index;
    *dst = entries[i];
    dirty = true;
  }

  if (rc == 0 && dirty) {
    if (fat32_write_cluster(mnt, write_cluster, cluster_buf) != 0)
      rc = -1;
  }

  kfree(cluster_buf);
  return rc;
}

// Find a file in a directory
static vfs_node_t *fat32_finddir_impl(vfs_node_t *node, char *name) {
  fat32_mount_t *mnt = node ? (fat32_mount_t *)node->device : NULL;
  if (!mnt || !name)
    return NULL;

  uint32_t cluster = node->impl;
  if (cluster == 0)
    cluster = mnt->root_cluster;

  fat32_lookup_t found;
  if (fat32_dir_lookup_raw(mnt, cluster, name, &found) != 0 || !found.found)
    return NULL;

  uint32_t entry_cluster = fat_entry_cluster(mnt, &found.sfn_entry);
  uint32_t dir_slot_cluster = (mnt->fat_type == 32) ? found.sfn_cluster : 0;
  return fat32_make_vfs_node(mnt, &found.sfn_entry, entry_cluster,
                             found.lfn_name, cluster, dir_slot_cluster,
                             found.sfn_index);
}

// Mount Operations

int fat32_mount(struct block_device *dev, vfs_node_t *mountpoint) {
  if (!dev || !mountpoint)
    return -1;

  klog_puts("[FAT] Probing block device '");
  klog_puts(dev->name);
  klog_puts("' for FAT12/16/32 filesystem...\n");

  uint32_t dev_ss = dev->sector_size ? dev->sector_size : 512;
  uint32_t boot_alloc = dev_ss < 512 ? 512 : dev_ss;
  uint8_t *boot_buf = kmalloc(boot_alloc);
  if (!boot_buf)
    return -1;

  int err = dev->read_sectors(dev, 0, 1, boot_buf);
  if (err) {
    kfree(boot_buf);
    klog_puts("[FAT] Failed to read boot sector.\n");
    return -1;
  }

  // Check boot signature
  uint16_t sig = (uint16_t)boot_buf[510] | ((uint16_t)boot_buf[511] << 8);
  if (sig != 0xAA55) {
    kfree(boot_buf);
    klog_puts("[FAT] Invalid boot signature.\n");
    return -1;
  }

  fat32_boot_sector_t *boot = (fat32_boot_sector_t *)boot_buf;

  // Check bytes per sector
  if (boot->bpb.bytes_per_sector != 512 && boot->bpb.bytes_per_sector != 1024 &&
      boot->bpb.bytes_per_sector != 2048 &&
      boot->bpb.bytes_per_sector != 4096) {
    kfree(boot_buf);
    klog_puts("[FAT] Invalid bytes per sector.\n");
    return -1;
  }

  // Check sectors per cluster
  uint8_t spc = boot->bpb.sectors_per_cluster;
  if (spc != 1 && spc != 2 && spc != 4 && spc != 8 && spc != 16 && spc != 32 &&
      spc != 64 && spc != 128) {
    kfree(boot_buf);
    klog_puts("[FAT] Invalid sectors per cluster.\n");
    return -1;
  }

  if (boot->bpb.reserved_sectors == 0 || boot->bpb.num_fats == 0) {
    kfree(boot_buf);
    klog_puts("[FAT] Invalid reserved sectors or FAT count.\n");
    return -1;
  }

  uint32_t fat_size = boot->bpb.fat_size_16 ? boot->bpb.fat_size_16
                                            : boot->ebpb.fat_size_32;
  uint32_t total_sectors = boot->bpb.total_sectors_16
                               ? boot->bpb.total_sectors_16
                               : boot->bpb.total_sectors_32;
  uint32_t root_dir_sectors =
      ((uint32_t)boot->bpb.root_entry_count * 32u +
       boot->bpb.bytes_per_sector - 1u) /
      boot->bpb.bytes_per_sector;
  uint32_t root_dir_start =
      boot->bpb.reserved_sectors + boot->bpb.num_fats * fat_size;
  uint32_t data_start_sector = root_dir_start + root_dir_sectors;

  if (fat_size == 0 || total_sectors <= data_start_sector) {
    kfree(boot_buf);
    klog_puts("[FAT] Invalid FAT geometry.\n");
    return -1;
  }

  uint32_t data_sectors = total_sectors - data_start_sector;
  uint32_t total_clusters = data_sectors / spc;
  if (total_clusters == 0) {
    kfree(boot_buf);
    return -1;
  }

  uint8_t fat_type;
  if (boot->bpb.fat_size_16 == 0 && boot->bpb.root_entry_count == 0) {
    fat_type = 32;
    if (boot->ebpb.root_cluster < 2) {
      kfree(boot_buf);
      return -1;
    }
  } else if (total_clusters < 4085) {
    fat_type = 12;
  } else if (total_clusters < 65525) {
    fat_type = 16;
  } else {
    fat_type = 32;
    if (boot->bpb.fat_size_16 != 0 || boot->ebpb.root_cluster < 2) {
      kfree(boot_buf);
      return -1;
    }
  }

  if (fat_type != 32 && root_dir_sectors == 0) {
    kfree(boot_buf);
    return -1;
  }

  // Allocate mount context
  fat32_mount_t *mnt = kmalloc(sizeof(fat32_mount_t));
  if (!mnt) {
    kfree(boot_buf);
    return -1;
  }
  memset(mnt, 0, sizeof(fat32_mount_t));

  memcpy(&mnt->boot, boot, sizeof(fat32_boot_sector_t));
  mnt->bytes_per_sector = boot->bpb.bytes_per_sector;
  mnt->sectors_per_cluster = spc;
  mnt->bytes_per_cluster = mnt->bytes_per_sector * mnt->sectors_per_cluster;
  mnt->fat_start_sector = boot->bpb.reserved_sectors;
  mnt->fat_size_sectors = fat_size;
  mnt->root_dir_start = root_dir_start;
  mnt->root_dir_sectors = root_dir_sectors;
  mnt->data_start_sector = data_start_sector;
  mnt->total_clusters = total_clusters;
  mnt->fat_type = fat_type;
  mnt->root_cluster = (fat_type == 32) ? boot->ebpb.root_cluster : 0;
  mnt->next_free_hint = 2;
  mnt->fsinfo_invalidated = false;
  kfree(boot_buf);

  klog_puts("[FAT] Superblock validated (FAT");
  klog_uint64(mnt->fat_type);
  klog_puts("):\n");
  klog_puts("       Sector size:   ");
  klog_uint64(mnt->bytes_per_sector);
  klog_puts(" bytes\n");
  klog_puts("       Cluster size:  ");
  klog_uint64(mnt->bytes_per_cluster);
  klog_puts(" bytes\n");
  klog_puts("       Total clusters: ");
  klog_uint64(mnt->total_clusters);
  klog_puts("\n");
  klog_puts("       Root cluster:   ");
  klog_uint64(mnt->root_cluster);
  klog_puts("\n");
  klog_puts("       FAT start:      sector ");
  klog_uint64(mnt->fat_start_sector);
  klog_puts("\n");
  klog_puts("       Data start:     sector ");
  klog_uint64(mnt->data_start_sector);
  klog_puts("\n");

  bool readonly = (mnt->fat_type != 32);

  // Create VFS node for root directory
  vfs_node_t *root_vfs = kmalloc(sizeof(vfs_node_t));
  if (!root_vfs) {
    kfree(mnt);
    return -1;
  }
  vfs_node_init(root_vfs);

  memcpy(root_vfs->name, "mnt", 4);
  root_vfs->flags = FS_DIRECTORY | (readonly ? FS_READONLY : 0);
  root_vfs->impl = mnt->root_cluster;
  root_vfs->device = mnt;
  root_vfs->readdir = fat32_readdir_impl;
  root_vfs->finddir = fat32_finddir_impl;
  if (!readonly) {
    root_vfs->create = fat32_create_impl;
    root_vfs->mkdir = fat32_mkdir_impl;
    root_vfs->unlink = fat32_unlink_impl;
    root_vfs->rmdir = fat32_rmdir_impl;
    root_vfs->rename = fat32_rename_impl;
  }
  root_vfs->mask = 0755;
  root_vfs->inode = mnt->root_cluster ? mnt->root_cluster : 1;
  root_vfs->statfs = fat32_statfs_impl;

  mnt->root_node = root_vfs;

  mnt->dev = block_ref(dev);
  mnt->refs = 1;

  // Wire up the mountpoint. It is usually a directory of another filesystem
  // (the ESP on an ext4 root), so every operation it inherited must go: an
  // ext4 mkdir left in place would run against this FAT mount as if it were
  // an ext2_mount_t and read garbage journal state.
  mountpoint->flags = FS_DIRECTORY | (readonly ? FS_READONLY : 0);
  mountpoint->inode = root_vfs->inode;
  mountpoint->impl = mnt->root_cluster;
  mountpoint->device = mnt;
  mountpoint->read = NULL;
  mountpoint->write = NULL;
  mountpoint->append = NULL;
  mountpoint->mknod = NULL;
  mountpoint->readlink = NULL;
  mountpoint->symlink = NULL;
  mountpoint->rename = readonly ? NULL : fat32_rename_impl;
  mountpoint->chmod = NULL;
  mountpoint->chown = NULL;
  mountpoint->truncate = NULL;
  mountpoint->mmap = NULL;
  mountpoint->fallocate = NULL;
  mountpoint->get_nlink = NULL;
  mountpoint->readdir = fat32_readdir_impl;
  mountpoint->finddir = fat32_finddir_impl;
  mountpoint->create = readonly ? NULL : fat32_create_impl;
  mountpoint->mkdir = readonly ? NULL : fat32_mkdir_impl;
  mountpoint->unlink = readonly ? NULL : fat32_unlink_impl;
  mountpoint->rmdir = readonly ? NULL : fat32_rmdir_impl;
  mountpoint->statfs = fat32_statfs_impl;

  klog_puts("[OK] FAT filesystem mounted.\n");
  return 0;
}

int fat32_live_nodes(vfs_node_t *root) {
  fat32_mount_t *mnt = root ? (fat32_mount_t *)root->device : NULL;
  if (!mnt)
    return 0;
  return (int)__atomic_load_n(&mnt->refs, __ATOMIC_ACQUIRE) - 1;
}

void fat32_unmount(vfs_node_t *root) {
  fat32_mount_t *mnt = root ? (fat32_mount_t *)root->device : NULL;
  if (mnt)
    fat32_mount_put(mnt);
}

int fat32_mount_root(struct block_device *dev) {
  if (!dev)
    return -1;

  // Create a minimal root VFS node
  vfs_node_t *root = kmalloc(sizeof(vfs_node_t));
  if (!root)
    return -1;
  vfs_node_init(root);

  memcpy(root->name, "/", 2);
  root->flags = FS_DIRECTORY;

  // Mount FAT32 onto this node
  if (fat32_mount(dev, root) != 0) {
    kfree(root);
    return -1;
  }

  // Set as the global root
  fs_root = root;

  return 0;
}

// Self-Test for Phases 1-6

void fat32_self_test(void) {
  klog_puts("\n[FAT32] ═══ FAT32 Driver Self-Test (Phases 1-6) ═══\n");

  int passed = 0;
  int failed = 0;

  // Get the boot device (assumes AHCI already registered it)
  struct block_device *dev = block_get(0);
  if (!dev) {
    klog_puts("[FAT32] TEST SKIP: No block device available.\n");
    return;
  }

  klog_puts("[FAT32] Using block device: ");
  klog_puts(dev->name);
  klog_puts("\n");

  // Test 1: Mount the filesystem
  klog_puts("\n[FAT32] Test 1: Mount filesystem\n");
  vfs_node_t *mountpoint = kmalloc(sizeof(vfs_node_t));
  if (!mountpoint) {
    klog_puts("[FAT32] TEST FAIL: Out of memory\n");
    block_put(dev);
    return;
  }
  vfs_node_init(mountpoint);
  memcpy(mountpoint->name, "test", 5);

  int mount_result = fat32_mount(dev, mountpoint);
  if (mount_result == 0) {
    klog_puts("[FAT32] TEST PASS: Mount succeeded\n");
    passed++;
  } else {
    klog_puts("[FAT32] TEST FAIL: Mount failed (not a FAT32 volume?)\n");
    failed++;
    kfree(mountpoint);
    goto summary;
  }

  // Test 2: Read root directory
  klog_puts("\n[FAT32] Test 2: Read root directory\n");
  uint32_t entry_count = 0;
  struct dirent *dent;
  while ((dent = fat32_readdir_impl(mountpoint, entry_count)) != NULL) {
    klog_puts("  - ");
    klog_puts(dent->name);
    klog_puts("\n");
    kfree(dent);
    entry_count++;
  }

  if (entry_count > 0) {
    klog_puts("[FAT32] TEST PASS: Found ");
    klog_uint64(entry_count);
    klog_puts(" entries in root directory\n");
    passed++;
  } else {
    klog_puts("[FAT32] TEST FAIL: Root directory is empty\n");
    failed++;
  }

  // Test 3: Find and read a file
  klog_puts("\n[FAT32] Test 3: Find and read file 'hello.txt'\n");
  vfs_node_t *hello = fat32_finddir_impl(mountpoint, "hello.txt");
  if (hello) {
    klog_puts("[FAT32] Found file: ");
    klog_puts(hello->name);
    klog_puts(" (size: ");
    klog_uint64(hello->length);
    klog_puts(" bytes)\n");

    if (hello->flags == FS_FILE && hello->read) {
      uint8_t *buf = kmalloc(hello->length + 1);
      if (buf) {
        memset(buf, 0, hello->length + 1);
        uint32_t read = hello->read(hello, 0, hello->length, buf);
        if (read > 0) {
          klog_puts("[FAT32] File content: \"");
          klog_puts((char *)buf);
          klog_puts("\"\n");
          klog_puts("[FAT32] TEST PASS: Read ");
          klog_uint64(read);
          klog_puts(" bytes from file\n");
          passed++;
        } else {
          klog_puts("[FAT32] TEST FAIL: Could not read file content\n");
          failed++;
        }
        kfree(buf);
      } else {
        klog_puts("[FAT32] TEST FAIL: Out of memory\n");
        failed++;
      }
    } else {
      klog_puts("[FAT32] TEST FAIL: File has no read callback\n");
      failed++;
    }
    kfree(hello);
  } else {
    klog_puts("[FAT32] TEST SKIP: 'hello.txt' not found (need test image)\n");
    // Don't count as failure if file doesn't exist
  }

  // Test 4: LFN support - find a long filename
  klog_puts("\n[FAT32] Test 4: Long Filename (LFN) support\n");
  vfs_node_t *lfn_file =
      fat32_finddir_impl(mountpoint, "This Is A Very Long Filename.txt");
  if (lfn_file) {
    klog_puts("[FAT32] Found LFN file: ");
    klog_puts(lfn_file->name);
    klog_puts("\n");
    klog_puts("[FAT32] TEST PASS: LFN support working\n");
    passed++;
    kfree(lfn_file);
  } else {
    klog_puts("[FAT32] TEST SKIP: LFN test file not found\n");
  }

  // Test 5: Subdirectory traversal
  klog_puts("\n[FAT32] Test 5: Subdirectory traversal\n");
  vfs_node_t *subdir = fat32_finddir_impl(mountpoint, "subdir");
  if (subdir) {
    klog_puts("[FAT32] Found subdirectory: ");
    klog_puts(subdir->name);
    klog_puts("\n");

    uint32_t sub_count = 0;
    struct dirent *sub_dent;
    while ((sub_dent = fat32_readdir_impl(subdir, sub_count)) != NULL) {
      klog_puts("  - ");
      klog_puts(sub_dent->name);
      klog_puts("\n");
      kfree(sub_dent);
      sub_count++;
    }

    if (sub_count > 0) {
      klog_puts("[FAT32] TEST PASS: Subdirectory contains ");
      klog_uint64(sub_count);
      klog_puts(" entries\n");
      passed++;
    } else {
      klog_puts("[FAT32] TEST FAIL: Subdirectory is empty\n");
      failed++;
    }
    kfree(subdir);
  } else {
    klog_puts("[FAT32] TEST SKIP: 'subdir' not found\n");
  }

  // Test 6: Create a new file (delete first if exists from previous run)
  klog_puts("\n[FAT32] Test 6: Create new file 'testfile.txt'\n");
  // Delete if it exists from previous test run
  fat32_unlink_impl(mountpoint, "testfile.txt");
  int create_result = fat32_create_impl(mountpoint, "testfile.txt", 0644);
  if (create_result == 0) {
    klog_puts("[FAT32] TEST PASS: File creation succeeded\n");
    passed++;

    // Test 7: Find the created file
    klog_puts("\n[FAT32] Test 7: Find created file\n");
    vfs_node_t *new_file = fat32_finddir_impl(mountpoint, "testfile.txt");
    if (new_file) {
      klog_puts("[FAT32] Found file: ");
      klog_puts(new_file->name);
      klog_puts("\n");
      passed++;

      // Test 8: Write to the new file
      klog_puts("\n[FAT32] Test 8: Write data to new file\n");
      const char *test_data = "This is test data written by FAT32 driver!";
      uint32_t written = fat32_write_impl(new_file, 0, strlen(test_data),
                                          (uint8_t *)test_data);
      if (written == strlen(test_data)) {
        klog_puts("[FAT32] Wrote ");
        klog_uint64(written);
        klog_puts(" bytes\n");
        klog_puts("[FAT32] TEST PASS: Write succeeded\n");
        passed++;

        // Test 9: Read back the data
        klog_puts("\n[FAT32] Test 9: Read back written data\n");
        uint8_t *read_buf = kmalloc(written + 1);
        if (read_buf) {
          memset(read_buf, 0, written + 1);
          uint32_t read_back = fat32_read_impl(new_file, 0, written, read_buf);
          if (read_back == written &&
              memcmp(read_buf, test_data, written) == 0) {
            klog_puts("[FAT32] Read back: \"");
            klog_puts((char *)read_buf);
            klog_puts("\"\n");
            klog_puts("[FAT32] TEST PASS: Data verified correctly\n");
            passed++;
          } else {
            klog_puts("[FAT32] TEST FAIL: Data mismatch (read ");
            klog_uint64(read_back);
            klog_puts(" bytes)\n");
            failed++;
          }
          kfree(read_buf);
        } else {
          klog_puts("[FAT32] TEST FAIL: Out of memory\n");
          failed++;
        }
      } else {
        klog_puts("[FAT32] TEST FAIL: Write failed (wrote ");
        klog_uint64(written);
        klog_puts(" bytes)\n");
        failed++;
      }
      kfree(new_file);
    } else {
      klog_puts("[FAT32] TEST FAIL: Could not find created file\n");
      failed++;
    }
  } else {
    klog_puts("[FAT32] TEST FAIL: Could not create file\n");
    failed++;
  }

  // Test 10: Truncate the file
  klog_puts("\n[FAT32] Test 10: Truncate file to 10 bytes\n");
  vfs_node_t *trunc_file = fat32_finddir_impl(mountpoint, "testfile.txt");
  if (trunc_file) {
    uint32_t orig_size = trunc_file->length;
    int trunc_result = fat32_truncate_impl(trunc_file, 10);
    if (trunc_result == 0 && trunc_file->length == 10) {
      klog_puts("[FAT32] Truncated from ");
      klog_uint64(orig_size);
      klog_puts(" to ");
      klog_uint64(trunc_file->length);
      klog_puts(" bytes\n");
      klog_puts("[FAT32] TEST PASS: Truncate succeeded\n");
      passed++;
    } else {
      klog_puts("[FAT32] TEST FAIL: Truncate failed\n");
      failed++;
    }
    kfree(trunc_file);
  } else {
    klog_puts("[FAT32] TEST SKIP: testfile.txt not found\n");
  }

  // Test 11: Delete the file
  klog_puts("\n[FAT32] Test 11: Delete file 'testfile.txt'\n");
  int unlink_result = fat32_unlink_impl(mountpoint, "testfile.txt");
  if (unlink_result == 0) {
    klog_puts("[FAT32] TEST PASS: File deleted\n");
    passed++;

    // Verify it's gone
    vfs_node_t *gone = fat32_finddir_impl(mountpoint, "testfile.txt");
    if (!gone) {
      klog_puts("[FAT32] TEST PASS: File no longer in directory\n");
      passed++;
    } else {
      klog_puts("[FAT32] TEST FAIL: File still exists after unlink\n");
      failed++;
      kfree(gone);
    }
  } else {
    klog_puts("[FAT32] TEST FAIL: Unlink failed\n");
    failed++;
  }

  // Cleanup.  The test frees its nodes with kfree(), bypassing the mount's
  // reference count, so free the mount state directly.
  fat32_mount_t *mnt = (fat32_mount_t *)mountpoint->device;
  if (mnt)
    fat32_mount_free(mnt);
  kfree(mountpoint);

summary:
  block_put(dev);
  klog_puts("\n[FAT32] ═══ Test Summary ═══\n");
  klog_puts("Passed: ");
  klog_uint64(passed);
  klog_puts("\nFailed: ");
  klog_uint64(failed);
  klog_puts("\n");

  if (failed == 0) {
    klog_puts("[FAT32] All tests PASSED!\n");
  } else {
    klog_puts("[FAT32] Some tests FAILED.\n");
  }
}
