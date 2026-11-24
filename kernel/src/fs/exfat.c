#include "exfat.h"
#include "arch/uaccess.h"
#include "console/klog.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "vfs.h"
#include <stddef.h>

#define EXFAT_SUPER_MAGIC 0x2011BAB0LL

#define EXFAT_TYPE_EOD 0x00
#define EXFAT_TYPE_BITMAP 0x81
#define EXFAT_TYPE_UPCASE 0x82
#define EXFAT_TYPE_LABEL 0x83
#define EXFAT_TYPE_FILE 0x85
#define EXFAT_TYPE_STREAM 0xC0
#define EXFAT_TYPE_NAME 0xC1

#define EXFAT_ATTR_DIR 0x0010
#define EXFAT_FLAG_NO_FAT_CHAIN 0x02

static inline uint16_t exfat_le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t exfat_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline uint64_t exfat_le64(const uint8_t *p) {
  return (uint64_t)exfat_le32(p) | ((uint64_t)exfat_le32(p + 4) << 32);
}

static void exfat_mount_free(exfat_mount_t *mnt) {
  kfree(mnt->root_node);
  block_put(mnt->dev);
  kfree(mnt);
}

static void exfat_mount_put(exfat_mount_t *mnt) {
  if (__atomic_sub_fetch(&mnt->refs, 1, __ATOMIC_ACQ_REL) == 0)
    exfat_mount_free(mnt);
}

static void exfat_node_destroy(vfs_node_t *node) {
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt || ((node->flags & FS_PERSISTENT) && !(node->flags & FS_DELETED)))
    return;
  node->device = NULL;
  exfat_mount_put(mnt);
}

static int exfat_read_vol_sectors(exfat_mount_t *mnt, uint64_t vol_lba,
                                  uint32_t count, void *buffer) {
  if (!mnt || !mnt->dev || !mnt->dev->read_sectors || !buffer || count == 0)
    return -1;

  uint32_t dev_ss = mnt->dev->sector_size ? mnt->dev->sector_size : 512;
  uint32_t vol_ss = mnt->bytes_per_sector;
  if (dev_ss == vol_ss) {
    return mnt->dev->read_sectors(mnt->dev, vol_lba, count, buffer);
  }

  uint64_t byte_off = vol_lba * (uint64_t)vol_ss;
  uint64_t total_bytes = (uint64_t)count * (uint64_t)vol_ss;
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
    memcpy(dst + done, bounce + in_sec, chunk);
    done += chunk;
  }
  kfree(bounce);
  return 0;
}

static bool exfat_is_valid_cluster(const exfat_mount_t *mnt, uint32_t cluster) {
  return cluster >= 2 && cluster < mnt->cluster_count + 2;
}

static uint32_t exfat_next_cluster(exfat_mount_t *mnt, uint32_t cluster,
                                   bool contig) {
  if (!exfat_is_valid_cluster(mnt, cluster))
    return 0xFFFFFFFFu;
  if (contig) {
    uint32_t next = cluster + 1u;
    return exfat_is_valid_cluster(mnt, next) ? next : 0xFFFFFFFFu;
  }

  uint64_t fat_byte = (uint64_t)cluster * 4u;
  uint64_t fat_sector =
      (uint64_t)mnt->fat_offset + (fat_byte / mnt->bytes_per_sector);
  uint32_t off = (uint32_t)(fat_byte % mnt->bytes_per_sector);
  if (fat_sector >= (uint64_t)mnt->fat_offset + mnt->fat_length)
    return 0xFFFFFFFFu;

  uint8_t *sec = kmalloc(mnt->bytes_per_sector);
  if (!sec)
    return 0xFFFFFFFFu;
  if (exfat_read_vol_sectors(mnt, fat_sector, 1, sec) != 0) {
    kfree(sec);
    return 0xFFFFFFFFu;
  }
  uint32_t next = exfat_le32(sec + off);
  kfree(sec);
  return next;
}

static int exfat_read_cluster(exfat_mount_t *mnt, uint32_t cluster,
                              void *buffer) {
  if (!exfat_is_valid_cluster(mnt, cluster))
    return -1;
  uint64_t lba = (uint64_t)mnt->cluster_heap_offset +
                 (uint64_t)(cluster - 2u) * (uint64_t)mnt->sectors_per_cluster;
  return exfat_read_vol_sectors(mnt, lba, mnt->sectors_per_cluster, buffer);
}

static uint32_t exfat_dos_time_to_unix(uint32_t dt) {
  uint32_t sec = (dt & 0x1Fu) * 2u;
  uint32_t min = (dt >> 5) & 0x3Fu;
  uint32_t hour = (dt >> 11) & 0x1Fu;
  uint32_t day = (dt >> 16) & 0x1Fu;
  uint32_t month = (dt >> 21) & 0x0Fu;
  uint32_t year = 1980u + ((dt >> 25) & 0x7Fu);
  if (month == 0)
    month = 1;
  if (day == 0)
    day = 1;
  uint32_t days = (year - 1970u) * 365u + (month - 1u) * 31u + day;
  return days * 86400u + hour * 3600u + min * 60u + sec;
}

static void exfat_utf16_append_utf8(char *out, size_t cap, size_t *pos,
                                    uint16_t ch) {
  if (*pos + 1 >= cap)
    return;
  if (ch < 0x80u) {
    out[(*pos)++] = (char)ch;
  } else if (ch < 0x800u) {
    if (*pos + 2 >= cap)
      return;
    out[(*pos)++] = (char)(0xC0u | (ch >> 6));
    out[(*pos)++] = (char)(0x80u | (ch & 0x3Fu));
  } else {
    if (*pos + 3 >= cap)
      return;
    out[(*pos)++] = (char)(0xE0u | (ch >> 12));
    out[(*pos)++] = (char)(0x80u | ((ch >> 6) & 0x3Fu));
    out[(*pos)++] = (char)(0x80u | (ch & 0x3Fu));
  }
}

typedef struct {
  exfat_mount_t *mnt;
  uint32_t cluster;
  bool contig;
  uint32_t offset_in_cluster;
  uint32_t entries_per_cluster;
  uint8_t *buf;
  bool eod;
} exfat_dir_cursor_t;

static int exfat_cursor_init(exfat_dir_cursor_t *cur, exfat_mount_t *mnt,
                             uint32_t cluster, bool contig) {
  memset(cur, 0, sizeof(*cur));
  cur->mnt = mnt;
  cur->cluster = cluster;
  cur->contig = contig;
  cur->entries_per_cluster = mnt->bytes_per_cluster / 32u;
  if (!exfat_is_valid_cluster(mnt, cluster))
    return -1;
  cur->buf = kmalloc(mnt->bytes_per_cluster);
  if (!cur->buf)
    return -1;
  if (exfat_read_cluster(mnt, cluster, cur->buf) != 0) {
    kfree(cur->buf);
    cur->buf = NULL;
    return -1;
  }
  return 0;
}

static void exfat_cursor_free(exfat_dir_cursor_t *cur) {
  kfree(cur->buf);
  cur->buf = NULL;
}

static const uint8_t *exfat_cursor_raw_next(exfat_dir_cursor_t *cur) {
  if (cur->eod)
    return NULL;
  if (cur->offset_in_cluster >= cur->entries_per_cluster) {
    cur->cluster = exfat_next_cluster(cur->mnt, cur->cluster, cur->contig);
    if (!exfat_is_valid_cluster(cur->mnt, cur->cluster)) {
      cur->eod = true;
      return NULL;
    }
    if (exfat_read_cluster(cur->mnt, cur->cluster, cur->buf) != 0) {
      cur->eod = true;
      return NULL;
    }
    cur->offset_in_cluster = 0;
  }
  const uint8_t *e = cur->buf + cur->offset_in_cluster * 32u;
  cur->offset_in_cluster++;
  if (e[0] == EXFAT_TYPE_EOD) {
    cur->eod = true;
    return NULL;
  }
  return e;
}

typedef struct {
  char name[256];
  uint32_t first_cluster;
  uint64_t data_length;
  uint16_t attributes;
  bool contig;
  uint32_t ctime;
  uint32_t mtime;
  uint32_t atime;
} exfat_entry_info_t;

static bool exfat_cursor_next_item(exfat_dir_cursor_t *cur,
                                   exfat_entry_info_t *out) {
  for (;;) {
    const uint8_t *e = exfat_cursor_raw_next(cur);
    if (!e)
      return false;

    if (e[0] == EXFAT_TYPE_BITMAP && cur->mnt->bitmap_cluster == 0) {
      cur->mnt->bitmap_cluster = exfat_le32(e + 20);
      cur->mnt->bitmap_length = exfat_le64(e + 24);
      continue;
    }

    if (e[0] != EXFAT_TYPE_FILE)
      continue;

    uint8_t sec_count = e[1];
    if (sec_count < 2 || sec_count > 18)
      continue;

    uint16_t attrs = exfat_le16(e + 4);
    uint32_t ctime = exfat_dos_time_to_unix(exfat_le32(e + 8));
    uint32_t mtime = exfat_dos_time_to_unix(exfat_le32(e + 12));
    uint32_t atime = exfat_dos_time_to_unix(exfat_le32(e + 16));

    const uint8_t *stream = exfat_cursor_raw_next(cur);
    if (!stream)
      return false;
    uint8_t remaining = sec_count - 1u;
    if (stream[0] != EXFAT_TYPE_STREAM) {
      for (uint8_t i = 0; i < remaining; i++)
        if (!exfat_cursor_raw_next(cur))
          return false;
      continue;
    }

    bool contig = (stream[1] & EXFAT_FLAG_NO_FAT_CHAIN) != 0;
    uint8_t name_len = stream[3];
    uint32_t first_cluster = exfat_le32(stream + 20);
    uint64_t data_len = exfat_le64(stream + 24);

    char name[256];
    size_t pos = 0;
    uint16_t chars_left = name_len;
    bool ok = name_len > 0;

    for (uint8_t i = 0; i < remaining; i++) {
      const uint8_t *ne = exfat_cursor_raw_next(cur);
      if (!ne)
        return false;
      if (ne[0] != EXFAT_TYPE_NAME) {
        ok = false;
        continue;
      }
      for (int k = 0; k < 15 && chars_left > 0; k++) {
        uint16_t ch = exfat_le16(ne + 2 + k * 2);
        if (ch == 0) {
          chars_left = 0;
          break;
        }
        exfat_utf16_append_utf8(name, sizeof(name), &pos, ch);
        chars_left--;
      }
    }

    if (!ok || pos == 0)
      continue;
    name[pos] = '\0';

    memset(out, 0, sizeof(*out));
    memcpy(out->name, name, pos + 1);
    out->first_cluster = first_cluster;
    out->data_length = data_len;
    out->attributes = attrs;
    out->contig = contig;
    out->ctime = ctime;
    out->mtime = mtime;
    out->atime = atime;
    return true;
  }
}

static struct dirent *exfat_readdir_impl(vfs_node_t *node, uint64_t index);
static vfs_node_t *exfat_finddir_impl(vfs_node_t *node, char *name);

static uint32_t exfat_read_impl(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt || !buffer)
    return 0;

  uint32_t file_size = node->length;
  if (offset >= file_size || size == 0)
    return 0;
  if (size > file_size - offset)
    size = file_size - offset;

  bool to_user = is_user_ptr((uint64_t)buffer);
  uint32_t cluster = node->impl;
  if (cluster == 0) {
    if (to_user) {
      unsigned long uncopied = clear_user(buffer, size);
      return size - (uint32_t)uncopied;
    }
    memset(buffer, 0, size);
    return size;
  }

  bool contig = (node->flags & FS_EXFAT_CONTIG) != 0;
  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint8_t *cluster_buf = kmalloc(cluster_size);
  if (!cluster_buf)
    return 0;

  uint32_t current_offset = 0;
  if (contig) {
    uint32_t skip = offset / cluster_size;
    cluster += skip;
    current_offset = skip * cluster_size;
  } else {
    while (exfat_is_valid_cluster(mnt, cluster) &&
           current_offset + cluster_size <= offset) {
      cluster = exfat_next_cluster(mnt, cluster, false);
      current_offset += cluster_size;
    }
  }

  uint32_t bytes_read = 0;
  while (exfat_is_valid_cluster(mnt, cluster) && bytes_read < size) {
    if (exfat_read_cluster(mnt, cluster, cluster_buf) != 0)
      break;

    uint32_t cluster_offset = (offset + bytes_read) - current_offset;
    uint32_t to_copy = cluster_size - cluster_offset;
    if (to_copy > size - bytes_read)
      to_copy = size - bytes_read;

    if (to_user) {
      unsigned long uncopied =
          copy_to_user(buffer + bytes_read, cluster_buf + cluster_offset, to_copy);
      bytes_read += to_copy - (uint32_t)uncopied;
      if (uncopied > 0)
        break;
    } else {
      memcpy(buffer + bytes_read, cluster_buf + cluster_offset, to_copy);
      bytes_read += to_copy;
    }

    current_offset += cluster_size;
    cluster = exfat_next_cluster(mnt, cluster, contig);
  }

  kfree(cluster_buf);
  return bytes_read;
}

static vfs_node_t *exfat_make_vfs_node(exfat_mount_t *mnt,
                                       const exfat_entry_info_t *info) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;
  vfs_node_init(node);

  size_t len = strlen(info->name);
  if (len > 127)
    len = 127;
  memcpy(node->name, info->name, len);
  node->name[len] = '\0';

  node->impl = info->first_cluster;
  node->inode = info->first_cluster ? info->first_cluster : 1;
  node->length = (info->data_length > UINT32_MAX) ? UINT32_MAX
                                                  : (uint32_t)info->data_length;
  node->device = mnt;
  node->atime = info->atime;
  node->mtime = info->mtime;
  node->ctime = info->ctime;

  uint32_t contig_flag = info->contig ? FS_EXFAT_CONTIG : 0;
  if (info->attributes & EXFAT_ATTR_DIR) {
    node->flags = FS_DIRECTORY | FS_READONLY | FS_AUTODESTROY | contig_flag;
    node->readdir = exfat_readdir_impl;
    node->finddir = exfat_finddir_impl;
  } else {
    node->flags =
        FS_FILE | FS_PAGE_CACHE | FS_READONLY | FS_AUTODESTROY | contig_flag;
    node->read = exfat_read_impl;
  }
  node->mask = 0555;
  node->statfs = exfat_statfs_impl;
  node->destroy = exfat_node_destroy;
  __atomic_add_fetch(&mnt->refs, 1, __ATOMIC_ACQ_REL);
  return node;
}

static uint32_t exfat_dir_cluster(exfat_mount_t *mnt, vfs_node_t *node,
                                  bool *contig_out) {
  if (node->impl == 0 || node == mnt->root_node) {
    *contig_out = false;
    return mnt->root_cluster;
  }
  *contig_out = (node->flags & FS_EXFAT_CONTIG) != 0;
  return node->impl;
}

static struct dirent *exfat_readdir_impl(vfs_node_t *node, uint64_t index) {
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt)
    return NULL;

  bool contig = false;
  uint32_t cluster = exfat_dir_cluster(mnt, node, &contig);
  exfat_dir_cursor_t cur;
  if (exfat_cursor_init(&cur, mnt, cluster, contig) != 0)
    return NULL;

  struct dirent *ret = NULL;
  struct dirent *dre = vfs_readdir_buffer();
  memset(dre, 0, sizeof(*dre));

  exfat_entry_info_t info;
  uint64_t idx = 0;
  while (exfat_cursor_next_item(&cur, &info)) {
    if (idx == index) {
      size_t len = strlen(info.name);
      if (len > 127)
        len = 127;
      memcpy(dre->name, info.name, len);
      dre->name[len] = '\0';
      dre->ino = info.first_cluster ? info.first_cluster : mnt->root_cluster;
      dre->d_type = (info.attributes & EXFAT_ATTR_DIR) ? DT_DIR : DT_REG;
      ret = dre;
      break;
    }
    idx++;
  }

  exfat_cursor_free(&cur);
  return ret;
}

static vfs_node_t *exfat_finddir_impl(vfs_node_t *node, char *name) {
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt || !name)
    return NULL;

  bool contig = false;
  uint32_t cluster = exfat_dir_cluster(mnt, node, &contig);
  exfat_dir_cursor_t cur;
  if (exfat_cursor_init(&cur, mnt, cluster, contig) != 0)
    return NULL;

  vfs_node_t *result = NULL;
  exfat_entry_info_t info;
  while (exfat_cursor_next_item(&cur, &info)) {
    if (strcasecmp(name, info.name) == 0) {
      result = exfat_make_vfs_node(mnt, &info);
      break;
    }
  }

  exfat_cursor_free(&cur);
  return result;
}

int exfat_statfs_impl(vfs_node_t *node, struct statfs_buf *buf) {
  exfat_mount_t *mnt = node ? (exfat_mount_t *)node->device : NULL;
  if (!mnt || !buf)
    return -1;

  uint64_t total = mnt->cluster_count;
  uint64_t free_clusters = 0;

  if (exfat_is_valid_cluster(mnt, mnt->bitmap_cluster) &&
      mnt->bitmap_length > 0) {
    uint8_t *cbuf = kmalloc(mnt->bytes_per_cluster);
    if (cbuf) {
      uint32_t c = mnt->bitmap_cluster;
      uint64_t remaining_clusters = total;
      uint64_t bytes_left = mnt->bitmap_length;
      while (exfat_is_valid_cluster(mnt, c) && remaining_clusters > 0 &&
             bytes_left > 0) {
        if (exfat_read_cluster(mnt, c, cbuf) != 0)
          break;
        uint64_t chunk = bytes_left < mnt->bytes_per_cluster
                             ? bytes_left
                             : mnt->bytes_per_cluster;
        for (uint64_t i = 0; i < chunk && remaining_clusters > 0; i++) {
          uint8_t b = cbuf[i];
          uint64_t bits = remaining_clusters < 8 ? remaining_clusters : 8;
          for (uint64_t bit = 0; bit < bits; bit++) {
            if (!(b & (1u << bit)))
              free_clusters++;
          }
          remaining_clusters -= bits;
        }
        bytes_left -= chunk;
        c = exfat_next_cluster(mnt, c, false);
      }
      kfree(cbuf);
    }
  }

  buf->f_type = EXFAT_SUPER_MAGIC;
  buf->f_bsize = (int64_t)mnt->bytes_per_cluster;
  buf->f_blocks = (int64_t)total;
  buf->f_bfree = (int64_t)free_clusters;
  buf->f_bavail = (int64_t)free_clusters;
  buf->f_files = (int64_t)total;
  buf->f_ffree = (int64_t)free_clusters;
  buf->f_fsid[0] = 0;
  buf->f_fsid[1] = 0;
  buf->f_namelen = 255;
  buf->f_frsize = (int64_t)mnt->bytes_per_cluster;
  buf->f_flags = 1; /* ST_RDONLY */
  return 0;
}

int exfat_mount(struct block_device *dev, vfs_node_t *mountpoint) {
  if (!dev || !mountpoint)
    return -1;

  uint32_t dev_ss = dev->sector_size ? dev->sector_size : 512;
  uint32_t boot_alloc = dev_ss < 512 ? 512 : dev_ss;
  uint8_t *boot = kmalloc(boot_alloc);
  if (!boot)
    return -1;

  if (dev->read_sectors(dev, 0, 1, boot) != 0) {
    kfree(boot);
    return -1;
  }

  if (boot[510] != 0x55 || boot[511] != 0xAA ||
      memcmp(boot + 3, "EXFAT   ", 8) != 0) {
    kfree(boot);
    return -1;
  }

  for (size_t i = 11; i < 64; i++) {
    if (boot[i] != 0) {
      kfree(boot);
      return -1;
    }
  }

  uint8_t bps_shift = boot[108];
  uint8_t spc_shift = boot[109];
  uint8_t num_fats = boot[110];
  if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25u - bps_shift ||
      (num_fats != 1 && num_fats != 2)) {
    kfree(boot);
    return -1;
  }

  uint32_t bytes_per_sector = 1u << bps_shift;
  uint32_t sectors_per_cluster = 1u << spc_shift;
  uint64_t volume_length = exfat_le64(boot + 72);
  uint32_t fat_offset = exfat_le32(boot + 80);
  uint32_t fat_length = exfat_le32(boot + 84);
  uint32_t cluster_heap_offset = exfat_le32(boot + 88);
  uint32_t cluster_count = exfat_le32(boot + 92);
  uint32_t root_cluster = exfat_le32(boot + 96);

  if (cluster_count == 0 || root_cluster < 2 ||
      root_cluster >= cluster_count + 2 ||
      (uint64_t)cluster_heap_offset +
              (uint64_t)cluster_count * sectors_per_cluster >
          volume_length) {
    kfree(boot);
    return -1;
  }
  kfree(boot);

  exfat_mount_t *mnt = kmalloc(sizeof(exfat_mount_t));
  if (!mnt)
    return -1;
  memset(mnt, 0, sizeof(exfat_mount_t));

  mnt->bytes_per_sector = bytes_per_sector;
  mnt->sectors_per_cluster = sectors_per_cluster;
  mnt->bytes_per_cluster = bytes_per_sector * sectors_per_cluster;
  mnt->fat_offset = fat_offset;
  mnt->fat_length = fat_length;
  mnt->cluster_heap_offset = cluster_heap_offset;
  mnt->cluster_count = cluster_count;
  mnt->root_cluster = root_cluster;
  mnt->volume_length = volume_length;

  vfs_node_t *root_vfs = kmalloc(sizeof(vfs_node_t));
  if (!root_vfs) {
    kfree(mnt);
    return -1;
  }
  vfs_node_init(root_vfs);
  memcpy(root_vfs->name, "mnt", 4);
  root_vfs->flags = FS_DIRECTORY | FS_READONLY;
  root_vfs->impl = mnt->root_cluster;
  root_vfs->inode = mnt->root_cluster;
  root_vfs->device = mnt;
  root_vfs->mask = 0555;
  root_vfs->readdir = exfat_readdir_impl;
  root_vfs->finddir = exfat_finddir_impl;
  root_vfs->statfs = exfat_statfs_impl;

  mnt->root_node = root_vfs;
  mnt->dev = block_ref(dev);
  mnt->refs = 1;

  /* Locate the allocation bitmap entry in the root directory so statfs()
   * reports free space even before the first directory listing. */
  exfat_dir_cursor_t bcur;
  if (exfat_cursor_init(&bcur, mnt, mnt->root_cluster, false) == 0) {
    const uint8_t *be;
    while ((be = exfat_cursor_raw_next(&bcur)) != NULL) {
      if (be[0] == EXFAT_TYPE_BITMAP) {
        mnt->bitmap_cluster = exfat_le32(be + 20);
        mnt->bitmap_length = exfat_le64(be + 24);
        break;
      }
    }
    exfat_cursor_free(&bcur);
  }

  mountpoint->flags = FS_DIRECTORY | FS_READONLY;
  mountpoint->inode = mnt->root_cluster;
  mountpoint->impl = mnt->root_cluster;
  mountpoint->device = mnt;
  mountpoint->read = NULL;
  mountpoint->write = NULL;
  mountpoint->append = NULL;
  mountpoint->mknod = NULL;
  mountpoint->readlink = NULL;
  mountpoint->symlink = NULL;
  mountpoint->rename = NULL;
  mountpoint->chmod = NULL;
  mountpoint->chown = NULL;
  mountpoint->truncate = NULL;
  mountpoint->mmap = NULL;
  mountpoint->fallocate = NULL;
  mountpoint->get_nlink = NULL;
  mountpoint->create = NULL;
  mountpoint->mkdir = NULL;
  mountpoint->unlink = NULL;
  mountpoint->rmdir = NULL;
  mountpoint->readdir = exfat_readdir_impl;
  mountpoint->finddir = exfat_finddir_impl;
  mountpoint->statfs = exfat_statfs_impl;

  klog_puts("[OK] exFAT filesystem mounted (read-only).\n");
  return 0;
}

int exfat_live_nodes(vfs_node_t *root) {
  exfat_mount_t *mnt = root ? (exfat_mount_t *)root->device : NULL;
  if (!mnt)
    return 0;
  return (int)__atomic_load_n(&mnt->refs, __ATOMIC_ACQUIRE) - 1;
}

void exfat_unmount(vfs_node_t *root) {
  exfat_mount_t *mnt = root ? (exfat_mount_t *)root->device : NULL;
  if (mnt)
    exfat_mount_put(mnt);
}
