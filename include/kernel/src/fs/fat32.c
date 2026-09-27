#include "fat32.h"
#include "console/klog.h"
#include "drivers/storage/block.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "vfs.h"
#include <stddef.h>

// Forward Declarations

static struct dirent *fat32_readdir_impl(vfs_node_t *node, uint32_t index);
static vfs_node_t *fat32_finddir_impl(vfs_node_t *node, char *name);
static uint32_t fat32_read_impl(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer);
static uint32_t fat32_write_impl(vfs_node_t *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer);
static int fat32_unlink_impl(vfs_node_t *parent, char *name);
static int fat32_rmdir_impl(vfs_node_t *parent, char *name);
static int fat32_truncate_impl(vfs_node_t *node, uint32_t new_size);

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

// Read a FAT entry to get the next cluster in a chain
uint32_t fat32_get_next_cluster(fat32_mount_t *mnt, uint32_t cluster) {
  if (!FAT32_IS_VALID(cluster)) {
    return 0;
  }

  // Each FAT entry is 4 bytes (32 bits)
  uint32_t fat_offset = cluster * 4;
  uint32_t fat_sector =
      mnt->fat_start_sector + (fat_offset / mnt->bytes_per_sector);
  uint32_t offset_in_sector = fat_offset % mnt->bytes_per_sector;

  uint8_t *buffer = kmalloc(mnt->bytes_per_sector);
  if (!buffer)
    return 0;

  int err = mnt->dev->read_sectors(mnt->dev, fat_sector, 1, buffer);
  if (err) {
    kfree(buffer);
    return 0;
  }

  uint32_t next_cluster = *(uint32_t *)(buffer + offset_in_sector);
  next_cluster &= 0x0FFFFFFF; // FAT32 uses only 28 bits

  kfree(buffer);
  return next_cluster;
}

// Read a cluster into a buffer
int fat32_read_cluster(fat32_mount_t *mnt, uint32_t cluster, void *buffer) {
  if (!FAT32_IS_VALID(cluster)) {
    return -1;
  }

  // Calculate the sector for this cluster
  uint32_t first_sector =
      mnt->data_start_sector + (cluster - 2) * mnt->sectors_per_cluster;

  int err = mnt->dev->read_sectors(mnt->dev, first_sector,
                                   mnt->sectors_per_cluster, buffer);
  return err;
}

// Write a FAT entry (set next cluster in chain)
static int fat32_set_fat_entry(fat32_mount_t *mnt, uint32_t cluster,
                               uint32_t value) {
  if (!FAT32_IS_VALID(cluster) || cluster >= mnt->total_clusters + 2)
    return -1;

  uint32_t fat_offset = cluster * 4;
  uint32_t fat_sector =
      mnt->fat_start_sector + (fat_offset / mnt->bytes_per_sector);
  uint32_t offset_in_sector = fat_offset % mnt->bytes_per_sector;

  uint8_t *buffer = kmalloc(mnt->bytes_per_sector);
  if (!buffer)
    return -1;

  // Read the FAT sector
  int err = mnt->dev->read_sectors(mnt->dev, fat_sector, 1, buffer);
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
    err = mnt->dev->write_sectors(mnt->dev, sector, 1, buffer);
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
  // Start from cluster 2 (first data cluster)
  for (uint32_t cluster = 2; cluster < mnt->total_clusters + 2; cluster++) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector =
        mnt->fat_start_sector + (fat_offset / mnt->bytes_per_sector);
    uint32_t offset_in_sector = fat_offset % mnt->bytes_per_sector;

    // Read FAT sector if needed
    static uint8_t *fat_buf = NULL;
    static uint32_t last_sector = 0xFFFFFFFF;

    if (!fat_buf) {
      fat_buf = kmalloc(mnt->bytes_per_sector);
      if (!fat_buf)
        return 0;
    }

    if (fat_sector != last_sector) {
      if (mnt->dev->read_sectors(mnt->dev, fat_sector, 1, fat_buf) != 0) {
        continue;
      }
      last_sector = fat_sector;
    }

    uint32_t entry = *(uint32_t *)(fat_buf + offset_in_sector);
    if ((entry & 0x0FFFFFFF) == 0) {
      return cluster; // Free cluster found
    }
  }

  return 0; // No free clusters
}

// Allocate a new cluster and link it to the chain
static uint32_t fat32_alloc_cluster(fat32_mount_t *mnt, uint32_t prev_cluster) {
  uint32_t new_cluster = fat32_find_free_cluster(mnt);
  if (new_cluster == 0)
    return 0;

  // Mark new cluster as EOF
  if (fat32_set_fat_entry(mnt, new_cluster, FAT32_EOF_MARKER) != 0) {
    return 0;
  }

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

// Write a cluster to disk
int fat32_write_cluster(fat32_mount_t *mnt, uint32_t cluster, void *buffer) {
  if (!FAT32_IS_VALID(cluster))
    return -1;

  uint32_t first_sector =
      mnt->data_start_sector + (cluster - 2) * mnt->sectors_per_cluster;

  return mnt->dev->write_sectors(mnt->dev, first_sector,
                                 mnt->sectors_per_cluster, buffer);
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

// Delete a file from the directory
static int fat32_unlink_impl(vfs_node_t *parent, char *name) {
  fat32_mount_t *mnt = (fat32_mount_t *)parent->device;
  if (!mnt)
    return -1;

  uint32_t dir_cluster = parent->impl;
  if (dir_cluster == 0)
    dir_cluster = mnt->root_cluster;

  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

  uint8_t *cluster_buf = kmalloc(cluster_size);
  if (!cluster_buf)
    return -1;

  uint32_t entry_cluster = dir_cluster;
  uint32_t prev_cluster = 0;

  // Search for the file entry
  while (FAT32_IS_VALID(entry_cluster)) {
    if (fat32_read_cluster(mnt, entry_cluster, cluster_buf) != 0)
      break;

    for (uint32_t i = 0; i < entries_per_cluster; i++) {
      fat32_dir_entry_t *entry = (fat32_dir_entry_t *)cluster_buf + i;

      // Skip deleted or unused entries
      if (entry->name[0] == 0x00)
        goto not_found; // End of directory
      if (entry->name[0] == 0xE5)
        continue;

      // Check if this is a LFN entry
      if (entry->attr == 0x0F)
        continue;

      // Compare name (SFN comparison)
      char sfn[13];
      int j = 0;
      for (int k = 0; k < 8 && entry->name[k] != ' '; k++) {
        sfn[j++] = entry->name[k];
      }
      if (entry->name[8] != ' ') {
        sfn[j++] = '.';
        for (int k = 8; k < 11 && entry->name[k] != ' '; k++) {
          sfn[j++] = entry->name[k];
        }
      }
      sfn[j] = '\0';

      // Compare case-insensitive
      if (strcasecmp(sfn, name) == 0) {
        // Found the file - check it's not a directory
        if (entry->attr & FAT32_ATTR_DIRECTORY) {
          kfree(cluster_buf);
          return -2; // Is a directory, use rmdir
        }

        // Get the file's cluster chain and free it
        uint32_t file_cluster =
            ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
        if (FAT32_IS_VALID(file_cluster)) {
          fat32_free_cluster_chain(mnt, file_cluster);
        }

        // Mark directory entry as deleted
        entry->name[0] = 0xE5;

        // Write back the cluster
        if (fat32_write_cluster(mnt, entry_cluster, cluster_buf) != 0) {
          kfree(cluster_buf);
          return -1;
        }

        kfree(cluster_buf);
        return 0; // Success
      }
    }

    prev_cluster = entry_cluster;
    entry_cluster = fat32_get_next_cluster(mnt, entry_cluster);
  }

not_found:
  kfree(cluster_buf);
  return -1; // File not found
}

// Delete an empty directory
static int fat32_rmdir_impl(vfs_node_t *parent, char *name) {
  fat32_mount_t *mnt = (fat32_mount_t *)parent->device;
  if (!mnt)
    return -1;

  // First find the directory to check if it's empty
  vfs_node_t *dir_node = fat32_finddir_impl(parent, name);
  if (!dir_node)
    return -1;

  if (!(dir_node->flags & FS_DIRECTORY)) {
    kfree(dir_node);
    return -2; // Not a directory
  }

  // Check if directory is empty (only . and ..)
  uint32_t count = 0;
  struct dirent *dent;
  while ((dent = fat32_readdir_impl(dir_node, count)) != NULL) {
    kfree(dent);
    count++;
  }

  // Directory should have at least . and .., more means not empty
  if (count > 2) {
    kfree(dir_node);
    return -3; // Directory not empty
  }

  uint32_t dir_cluster = dir_node->impl;
  kfree(dir_node);

  // Now delete from parent directory
  uint32_t parent_cluster = parent->impl;
  if (parent_cluster == 0)
    parent_cluster = mnt->root_cluster;

  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

  uint8_t *cluster_buf = kmalloc(cluster_size);
  if (!cluster_buf)
    return -1;

  uint32_t entry_cluster = parent_cluster;

  while (FAT32_IS_VALID(entry_cluster)) {
    if (fat32_read_cluster(mnt, entry_cluster, cluster_buf) != 0)
      break;

    for (uint32_t i = 0; i < entries_per_cluster; i++) {
      fat32_dir_entry_t *entry = (fat32_dir_entry_t *)cluster_buf + i;

      if (entry->name[0] == 0x00)
        break;
      if (entry->name[0] == 0xE5)
        continue;
      if (entry->attr == 0x0F)
        continue;

      // Check if this matches
      uint32_t entry_cluster_val =
          ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
      if (entry_cluster_val == dir_cluster &&
          (entry->attr & FAT32_ATTR_DIRECTORY)) {

        // Found it - mark as deleted
        entry->name[0] = 0xE5;

        // Free the directory's cluster
        if (FAT32_IS_VALID(dir_cluster)) {
          fat32_free_cluster_chain(mnt, dir_cluster);
        }

        // Write back
        fat32_write_cluster(mnt, entry_cluster, cluster_buf);
        kfree(cluster_buf);
        return 0;
      }
    }

    entry_cluster = fat32_get_next_cluster(mnt, entry_cluster);
  }

  kfree(cluster_buf);
  return -1;
}

// Truncate a file to a smaller size
static int fat32_truncate_impl(vfs_node_t *node, uint32_t new_size) {
  fat32_mount_t *mnt = (fat32_mount_t *)node->device;
  if (!mnt)
    return -1;

  // Can only truncate, not extend
  if (new_size > node->length)
    return -1;

  if (new_size == 0) {
    // Free all clusters
    uint32_t cluster = node->impl;
    if (FAT32_IS_VALID(cluster)) {
      fat32_free_cluster_chain(mnt, cluster);
      node->impl = 0;
    }
    node->length = 0;
    return 0;
  }

  // Find the cluster containing the new end
  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint32_t cluster = node->impl;
  uint32_t current_offset = 0;
  uint32_t prev_cluster = 0;

  while (FAT32_IS_VALID(cluster) && current_offset + cluster_size <= new_size) {
    prev_cluster = cluster;
    cluster = fat32_get_next_cluster(mnt, cluster);
    current_offset += cluster_size;
  }

  // Free all clusters after this one
  if (FAT32_IS_VALID(cluster)) {
    uint32_t next = fat32_get_next_cluster(mnt, cluster);
    if (FAT32_IS_VALID(next)) {
      fat32_free_cluster_chain(mnt, next);
    }

    // Mark current cluster as EOF
    fat32_set_fat_entry(mnt, cluster, FAT32_EOF_MARKER);
  }

  node->length = new_size;
  return 0;
}

// Write to a file
static uint32_t fat32_write_impl(vfs_node_t *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
  fat32_mount_t *mnt = (fat32_mount_t *)node->device;
  if (!mnt)
    return 0;

  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint32_t cluster = node->impl;

  // Handle new file (no cluster allocated yet)
  if (cluster == 0) {
    cluster = fat32_alloc_cluster(mnt, 0);
    if (cluster == 0)
      return 0;
    node->impl = cluster;
  }

  // Allocate cluster buffer
  uint8_t *cluster_buf = kmalloc(cluster_size);
  if (!cluster_buf)
    return 0;

  uint32_t bytes_written = 0;
  uint32_t current_offset = 0;
  uint32_t prev_cluster = 0;

  // Walk to the correct cluster, allocating as needed
  while (current_offset + cluster_size <= offset) {
    prev_cluster = cluster;
    uint32_t next = fat32_get_next_cluster(mnt, cluster);

    if (!FAT32_IS_VALID(next)) {
      // Need to allocate new cluster
      next = fat32_alloc_cluster(mnt, cluster);
      if (next == 0) {
        kfree(cluster_buf);
        return bytes_written;
      }
    }

    cluster = next;
    current_offset += cluster_size;
  }

  // Now write the data
  while (bytes_written < size) {
    // Read existing cluster content (to preserve data before/after write)
    if (fat32_read_cluster(mnt, cluster, cluster_buf) != 0) {
      // For new clusters, zero the buffer
      memset(cluster_buf, 0, cluster_size);
    }

    uint32_t cluster_offset = (offset + bytes_written) - current_offset;
    uint32_t to_copy = cluster_size - cluster_offset;
    if (to_copy > size - bytes_written) {
      to_copy = size - bytes_written;
    }

    memcpy(cluster_buf + cluster_offset, buffer + bytes_written, to_copy);

    // Write cluster back
    if (fat32_write_cluster(mnt, cluster, cluster_buf) != 0) {
      break;
    }

    bytes_written += to_copy;
    current_offset += cluster_size;

    // Get next cluster or allocate
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

  // Update file size if we extended it
  if (offset + bytes_written > node->length) {
    node->length = offset + bytes_written;
  }

  // Update directory entry with new cluster and size
  // Find the directory containing this file by matching name
  if (mnt) {
    uint32_t dir_cluster = mnt->root_cluster;
    uint32_t cluster_size = mnt->bytes_per_cluster;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    uint8_t *dir_buf = kmalloc(cluster_size);
    if (dir_buf) {
      while (FAT32_IS_VALID(dir_cluster)) {
        if (fat32_read_cluster(mnt, dir_cluster, dir_buf) == 0) {
          for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)dir_buf + i;
            if (entry->name[0] == 0x00)
              break;
            if (entry->name[0] == 0xE5)
              continue;
            if (entry->attr == 0x0F)
              continue;

            // Build SFN from entry and compare
            char sfn[13];
            int j = 0;
            for (int k = 0; k < 8 && entry->name[k] != ' '; k++) {
              sfn[j++] = entry->name[k];
            }
            if (entry->name[8] != ' ') {
              sfn[j++] = '.';
              for (int k = 8; k < 11 && entry->name[k] != ' '; k++) {
                sfn[j++] = entry->name[k];
              }
            }
            sfn[j] = '\0';

            if (strcasecmp(sfn, node->name) == 0) {
              // Found the entry - update it
              entry->cluster_low = node->impl & 0xFFFF;
              entry->cluster_high = (node->impl >> 16) & 0xFFFF;
              entry->file_size = node->length;
              fat32_write_cluster(mnt, dir_cluster, dir_buf);
              kfree(dir_buf);
              return bytes_written;
            }
          }
        }
        dir_cluster = fat32_get_next_cluster(mnt, dir_cluster);
      }
      kfree(dir_buf);
    }
  }

  return bytes_written;
}

// Get current time for directory entry (simplified - use fixed date)
static void get_fat_time(uint16_t *date, uint16_t *time) {
  // Fixed date: May 6, 2026 = 0x5A06 (year=46, month=5, day=6)
  *date = (46 << 9) | (5 << 5) | 6;  // Year from 1980, month, day
  *time = (12 << 11) | (0 << 5) | 0; // Hour, minute, second/2
}

// Create a new file in a directory (VFS create callback)
static int fat32_create_impl(vfs_node_t *parent, char *name,
                             uint16_t permission) {
  fat32_mount_t *mnt = (fat32_mount_t *)parent->device;
  if (!mnt)
    return -1;

  // Check if file already exists
  vfs_node_t *existing = fat32_finddir_impl(parent, name);
  if (existing) {
    kfree(existing);
    return -1; // File exists
  }

  uint32_t dir_cluster = parent->impl;
  if (dir_cluster == 0)
    dir_cluster = mnt->root_cluster;

  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

  // Find a free directory entry
  uint8_t *cluster_buf = kmalloc(cluster_size);
  if (!cluster_buf)
    return -1;

  fat32_dir_entry_t *new_entry = NULL;
  uint32_t entry_cluster = dir_cluster;
  uint32_t entry_offset = 0;
  bool found_slot = false;

  // Scan directory for free slot or end
  while (FAT32_IS_VALID(entry_cluster)) {
    if (fat32_read_cluster(mnt, entry_cluster, cluster_buf) != 0)
      break;

    for (uint32_t i = 0; i < entries_per_cluster; i++) {
      fat32_dir_entry_t *entry = (fat32_dir_entry_t *)cluster_buf + i;

      // Found deleted entry or end of directory
      if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        new_entry = entry;
        entry_offset = i;
        found_slot = true;
        break;
      }
    }

    if (found_slot)
      break;

    uint32_t next = fat32_get_next_cluster(mnt, entry_cluster);
    if (!FAT32_IS_VALID(next)) {
      // Need to extend directory
      next = fat32_alloc_cluster(mnt, entry_cluster);
      if (next == 0)
        break;
      // Zero the new cluster
      memset(cluster_buf, 0, cluster_size);
      fat32_write_cluster(mnt, next, cluster_buf);
      entry_cluster = next;
      entry_offset = 0;
      new_entry = (fat32_dir_entry_t *)cluster_buf;
      found_slot = true;
      break;
    }
    entry_cluster = next;
  }

  if (!found_slot) {
    kfree(cluster_buf);
    return -1;
  }

  // Create SFN from name (simplified - just pad/truncate)
  memset(new_entry->name, ' ', 11);
  int name_len = strlen(name);
  int dot_pos = name_len;
  for (int i = 0; i < name_len; i++) {
    if (name[i] == '.')
      dot_pos = i;
  }

  // Copy base name (max 8 chars)
  for (int i = 0; i < 8 && i < dot_pos; i++) {
    char c = name[i];
    if (c >= 'a' && c <= 'z')
      c -= 32; // Uppercase
    new_entry->name[i] = c;
  }

  // Copy extension (max 3 chars)
  if (dot_pos < name_len - 1) {
    for (int i = 0; i < 3 && dot_pos + 1 + i < name_len; i++) {
      char c = name[dot_pos + 1 + i];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      new_entry->name[8 + i] = c;
    }
  }

  // Set attributes (create regular file, not directory - mkdir handles that)
  new_entry->attr = 0; // Regular file
  new_entry->reserved = 0;
  get_fat_time(&new_entry->create_date, &new_entry->create_time);
  new_entry->last_access_date = new_entry->create_date;
  new_entry->cluster_high = 0;
  get_fat_time(&new_entry->modify_date, &new_entry->modify_time);
  new_entry->cluster_low = 0;
  new_entry->file_size = 0;

  // Write directory entry back
  if (fat32_write_cluster(mnt, entry_cluster, cluster_buf) != 0) {
    kfree(cluster_buf);
    return -1;
  }

  kfree(cluster_buf);
  return 0; // Success
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
  while (cluster && current_offset + cluster_size <= offset) {
    cluster = fat32_get_next_cluster(mnt, cluster);
    current_offset += cluster_size;
  }

  // Now read the data
  while (cluster && bytes_read < size) {
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
static struct dirent *fat32_readdir_impl(vfs_node_t *node, uint32_t index);
static vfs_node_t *fat32_finddir_impl(vfs_node_t *node, char *name);

// Create a VFS node from a directory entry
static vfs_node_t *fat32_make_vfs_node(fat32_mount_t *mnt,
                                       fat32_dir_entry_t *entry,
                                       uint32_t cluster, char *lfn_name) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;
  vfs_node_init(node);

  // Set name - use LFN if available, otherwise SFN
  if (lfn_name && lfn_name[0]) {
    int _lfn_len = strlen(lfn_name);
    if (_lfn_len > 127)
      _lfn_len = 127;
    memcpy(node->name, lfn_name, _lfn_len);
    node->name[_lfn_len] = '\0';
  } else {
    // Convert 8.3 name to null-terminated string
    int j = 0;
    for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
      node->name[j++] = entry->name[i];
    }
    if (entry->name[8] != ' ') {
      node->name[j++] = '.';
      for (int i = 8; i < 11 && entry->name[i] != ' '; i++) {
        node->name[j++] = entry->name[i];
      }
    }
    node->name[j] = '\0';
  }

  node->impl = cluster;
  node->length = entry->file_size;
  node->device = mnt;
  node->inode = cluster; // Use cluster as inode number

  // Parse times
  node->atime = fat_date_to_unix(entry->last_access_date, 0);
  node->mtime = fat_date_to_unix(entry->modify_date, entry->modify_time);
  node->ctime = fat_date_to_unix(entry->create_date, entry->create_time);

  // Set flags and operations based on attributes
  if (entry->attr & FAT32_ATTR_DIRECTORY) {
    node->flags = FS_DIRECTORY;
    node->readdir = fat32_readdir_impl;
    node->finddir = fat32_finddir_impl;
    node->create = fat32_create_impl;
    node->unlink = fat32_unlink_impl;
    node->rmdir = fat32_rmdir_impl;
  } else {
    node->flags = FS_FILE | FS_PAGE_CACHE;
    node->read = fat32_read_impl;
    node->write = fat32_write_impl;
    node->unlink = fat32_unlink_impl;
    node->truncate = fat32_truncate_impl;
  }

  // Set permissions (FAT doesn't have Unix permissions, use defaults)
  node->mask = 0755;
  if (!(entry->attr & FAT32_ATTR_READ_ONLY)) {
    node->mask |= 0222; // Add write permission
  }

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
                      uint8_t *sfn) {
  uint8_t expected_checksum = lfn_checksum(sfn);
  int out_pos = 0;

  // Entries are stored by sequence number: entries[0] = seq 1, entries[1] = seq
  // 2, etc. Process in forward order (seq 1 first, then seq 2, etc.)
  for (int i = 0; i < count; i++) {
    fat32_lfn_entry_t *lfn = &entries[i];

    // Verify checksum
    if (lfn->checksum != expected_checksum) {
      return -1;
    }

    // Decode UTF-16 to ASCII (simplified - just take low byte)
    for (int j = 0; j < 5; j++) {
      if (lfn->name1[j] == 0 || lfn->name1[j] == 0xFFFF)
        break;
      out[out_pos++] = lfn->name1[j] & 0xFF;
    }
    for (int j = 0; j < 6; j++) {
      if (lfn->name2[j] == 0 || lfn->name2[j] == 0xFFFF)
        break;
      out[out_pos++] = lfn->name2[j] & 0xFF;
    }
    for (int j = 0; j < 2; j++) {
      if (lfn->name3[j] == 0 || lfn->name3[j] == 0xFFFF)
        break;
      out[out_pos++] = lfn->name3[j] & 0xFF;
    }
  }

  out[out_pos] = '\0';
  return out_pos;
}

// Directory Operations

// Context for directory iteration
typedef struct {
  fat32_mount_t *mnt;
  uint32_t cluster;                  // Current cluster being read
  uint32_t entry_index;              // Entry index within directory
  uint32_t cluster_offset;           // Offset within current cluster
  uint8_t *cluster_buf;              // Buffer for current cluster
  fat32_lfn_entry_t lfn_entries[20]; // Buffer for LFN entries
  int lfn_count;                     // Number of LFN entries accumulated
} dir_iter_t;

// Initialize a directory iterator
static int dir_iter_init(dir_iter_t *iter, fat32_mount_t *mnt,
                         uint32_t cluster) {
  iter->mnt = mnt;
  iter->cluster = cluster;
  iter->entry_index = 0;
  iter->cluster_offset = 0;
  iter->cluster_buf = kmalloc(mnt->bytes_per_cluster);
  iter->lfn_count = 0;

  if (!iter->cluster_buf)
    return -1;

  if (fat32_read_cluster(mnt, cluster, iter->cluster_buf) != 0) {
    kfree(iter->cluster_buf);
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
  uint32_t entries_per_cluster =
      iter->mnt->bytes_per_cluster / sizeof(fat32_dir_entry_t);

  while (true) {
    // Check if we need to move to next cluster
    if (iter->cluster_offset >= entries_per_cluster) {
      iter->cluster = fat32_get_next_cluster(iter->mnt, iter->cluster);
      if (!FAT32_IS_VALID(iter->cluster) && !FAT32_IS_EOF(iter->cluster)) {
        return NULL; // End of directory or error
      }
      iter->cluster_offset = 0;

      if (fat32_read_cluster(iter->mnt, iter->cluster, iter->cluster_buf) !=
          0) {
        return NULL;
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
static struct dirent *fat32_readdir_impl(vfs_node_t *node, uint32_t index) {
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
  static struct dirent dre;
  memset(&dre, 0, sizeof(dre));
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
      dirent = &dre;
      {
        // Decode LFN if present
        char lfn_name[256] = {0};
        if (iter.lfn_count > 0) {
          decode_lfn(iter.lfn_entries, iter.lfn_count, lfn_name, entry->name);
        }

        // Set name
        if (lfn_name[0]) {
          int _lfn_len = strlen(lfn_name);
          if (_lfn_len > 127)
            _lfn_len = 127;
          memcpy(dirent->name, lfn_name, _lfn_len);
          dirent->name[_lfn_len] = '\0';
        } else {
          // Convert 8.3 name
          int j = 0;
          for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
            dirent->name[j++] = entry->name[i];
          }
          if (entry->name[8] != ' ') {
            dirent->name[j++] = '.';
            for (int i = 8; i < 11 && entry->name[i] != ' '; i++) {
              dirent->name[j++] = entry->name[i];
            }
          }
          dirent->name[j] = '\0';
        }

        uint32_t entry_cluster =
            entry->cluster_low | (entry->cluster_high << 16);
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

// Find a file in a directory
static vfs_node_t *fat32_finddir_impl(vfs_node_t *node, char *name) {
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

  vfs_node_t *result = NULL;

  while (true) {
    fat32_dir_entry_t *entry = dir_iter_next(&iter);
    if (!entry)
      break;

    // Handle LFN entries
    if (is_lfn_entry(entry)) {
      fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)entry;
      int seq = lfn->seq & 0x1F;

      if (seq > 0 && seq <= 20) {
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

    // Decode LFN if present
    char lfn_name[256] = {0};
    if (iter.lfn_count > 0) {
      decode_lfn(iter.lfn_entries, iter.lfn_count, lfn_name, entry->name);
    }

    // Build SFN for comparison
    char sfn_name[13];
    int j = 0;
    for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
      sfn_name[j++] = entry->name[i];
    }
    if (entry->name[8] != ' ') {
      sfn_name[j++] = '.';
      for (int i = 8; i < 11 && entry->name[i] != ' '; i++) {
        sfn_name[j++] = entry->name[i];
      }
    }
    sfn_name[j] = '\0';

    // Compare names (case-insensitive for FAT)
    bool match = false;
    if (lfn_name[0]) {
      match = (strcasecmp(name, lfn_name) == 0);
    }
    if (!match) {
      match = (strcasecmp(name, sfn_name) == 0);
    }

    if (match) {
      uint32_t entry_cluster = entry->cluster_low | (entry->cluster_high << 16);
      result = fat32_make_vfs_node(mnt, entry, entry_cluster, lfn_name);
      break;
    }

    iter.lfn_count = 0;
  }

  dir_iter_free(&iter);
  return result;
}

// Mount Operations

int fat32_mount(struct block_device *dev, vfs_node_t *mountpoint) {
  if (!dev || !mountpoint)
    return -1;

  klog_puts("[FAT32] Probing block device '");
  klog_puts(dev->name);
  klog_puts("' for FAT32 filesystem...\n");

  // Read the boot sector
  uint8_t boot_buf[512];
  int err = dev->read_sectors(dev, 0, 1, boot_buf);
  if (err) {
    klog_puts("[FAT32] Failed to read boot sector.\n");
    return -1;
  }

  // Check boot signature
  uint16_t *sig = (uint16_t *)(boot_buf + 510);
  if (*sig != 0xAA55) {
    klog_puts("[FAT32] Invalid boot signature.\n");
    return -1;
  }

  fat32_boot_sector_t *boot = (fat32_boot_sector_t *)boot_buf;

  // Validate FAT32 specific fields
  if (boot->bpb.fat_size_16 != 0) {
    klog_puts("[FAT32] Not a FAT32 volume (FAT16 detected).\n");
    return -1;
  }

  if (boot->bpb.root_entry_count != 0) {
    klog_puts("[FAT32] Not a FAT32 volume (FAT12/16 root entry count).\n");
    return -1;
  }

  // Check bytes per sector
  if (boot->bpb.bytes_per_sector != 512 && boot->bpb.bytes_per_sector != 1024 &&
      boot->bpb.bytes_per_sector != 2048 &&
      boot->bpb.bytes_per_sector != 4096) {
    klog_puts("[FAT32] Invalid bytes per sector.\n");
    return -1;
  }

  // Check sectors per cluster
  uint8_t spc = boot->bpb.sectors_per_cluster;
  if (spc != 1 && spc != 2 && spc != 4 && spc != 8 && spc != 16 && spc != 32 &&
      spc != 64 && spc != 128) {
    klog_puts("[FAT32] Invalid sectors per cluster.\n");
    return -1;
  }

  // Allocate mount context
  fat32_mount_t *mnt = kmalloc(sizeof(fat32_mount_t));
  if (!mnt)
    return -1;
  memset(mnt, 0, sizeof(fat32_mount_t));

  mnt->dev = dev;
  memcpy(&mnt->boot, boot, sizeof(fat32_boot_sector_t));
  mnt->bytes_per_sector = boot->bpb.bytes_per_sector;
  mnt->sectors_per_cluster = boot->bpb.sectors_per_cluster;
  mnt->bytes_per_cluster = mnt->bytes_per_sector * mnt->sectors_per_cluster;
  mnt->fat_start_sector = boot->bpb.reserved_sectors;
  mnt->fat_size_sectors = boot->ebpb.fat_size_32;
  mnt->root_cluster = boot->ebpb.root_cluster;

  // Calculate data start sector
  uint32_t total_fat_sectors = boot->bpb.num_fats * mnt->fat_size_sectors;
  mnt->data_start_sector = mnt->fat_start_sector + total_fat_sectors;

  // Calculate total clusters
  uint32_t total_sectors = boot->bpb.total_sectors_16
                               ? boot->bpb.total_sectors_16
                               : boot->bpb.total_sectors_32;
  uint32_t data_sectors = total_sectors - mnt->data_start_sector;
  mnt->total_clusters = data_sectors / mnt->sectors_per_cluster;

  // Verify it's actually FAT32 (not FAT16 or FAT12)
  if (mnt->total_clusters < 65525) {
    klog_puts("[FAT32] Not a FAT32 volume (cluster count too low).\n");
    kfree(mnt);
    return -1;
  }

  klog_puts("[FAT32] Superblock validated:\n");
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

  // Create VFS node for root directory
  vfs_node_t *root_vfs = kmalloc(sizeof(vfs_node_t));
  if (!root_vfs) {
    kfree(mnt);
    return -1;
  }
  vfs_node_init(root_vfs);

  memcpy(root_vfs->name, "mnt", 4);
  root_vfs->flags = FS_DIRECTORY;
  root_vfs->impl = mnt->root_cluster;
  root_vfs->device = mnt;
  root_vfs->readdir = fat32_readdir_impl;
  root_vfs->finddir = fat32_finddir_impl;
  root_vfs->create = fat32_create_impl;
  root_vfs->unlink = fat32_unlink_impl;
  root_vfs->rmdir = fat32_rmdir_impl;
  root_vfs->mask = 0755;
  root_vfs->inode = mnt->root_cluster;

  mnt->root_node = root_vfs;

  // Wire up the mountpoint
  mountpoint->flags = FS_DIRECTORY;
  mountpoint->inode = mnt->root_cluster;
  mountpoint->device = mnt;
  mountpoint->readdir = fat32_readdir_impl;
  mountpoint->finddir = fat32_finddir_impl;
  mountpoint->create = fat32_create_impl;
  mountpoint->unlink = fat32_unlink_impl;
  mountpoint->rmdir = fat32_rmdir_impl;

  klog_puts("[OK] FAT32 filesystem mounted.\n");
  return 0;
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

  // Cleanup
  fat32_mount_t *mnt = (fat32_mount_t *)mountpoint->device;
  if (mnt) {
    if (mnt->root_node)
      kfree(mnt->root_node);
    kfree(mnt);
  }
  kfree(mountpoint);

summary:
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
