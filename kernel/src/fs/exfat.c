#include "exfat.h"
#include "arch/uaccess.h"
#include "console/klog.h"
#include "drivers/timer/rtc.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "vfs.h"
#include <linux/errno.h>
#include <stddef.h>

#define EXFAT_SUPER_MAGIC 0x2011BAB0LL

#define EXFAT_TYPE_EOD 0x00
#define EXFAT_TYPE_BITMAP 0x81
#define EXFAT_TYPE_UPCASE 0x82
#define EXFAT_TYPE_LABEL 0x83
#define EXFAT_TYPE_FILE 0x85
#define EXFAT_TYPE_STREAM 0xC0
#define EXFAT_TYPE_NAME 0xC1

#define EXFAT_ATTR_READONLY 0x0001
#define EXFAT_ATTR_DIR 0x0010
#define EXFAT_FLAG_NO_FAT_CHAIN 0x02
#define EXFAT_ENTRY_SIZE 32u
#define EXFAT_MAX_ENTRIES 19u
#define EXFAT_EOC 0xFFFFFFFFu

static spinlock_t exfat_mount_lock = SPINLOCK_INIT;
static exfat_mount_t *exfat_mounts;

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

static void exfat_store16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void exfat_store32(uint8_t *p, uint32_t value) {
  exfat_store16(p, (uint16_t)value);
  exfat_store16(p + 2, (uint16_t)(value >> 16));
}

static void exfat_store64(uint8_t *p, uint64_t value) {
  exfat_store32(p, (uint32_t)value);
  exfat_store32(p + 4, (uint32_t)(value >> 32));
}

static uint16_t exfat_entry_checksum(uint16_t sum, const uint8_t *entry,
                                     bool primary) {
  for (unsigned i = 0; i < 32; i++) {
    if (!primary || (i != 2 && i != 3))
      sum = (uint16_t)((sum >> 1) + ((sum & 1u) << 15) + entry[i]);
  }
  return sum;
}

static void exfat_mount_free(exfat_mount_t *mnt) {
  spinlock_acquire(&exfat_mount_lock);
  exfat_mount_t **link = &exfat_mounts;
  while (*link != mnt)
    link = &(*link)->next;
  *link = mnt->next;
  spinlock_release(&exfat_mount_lock);
  kfree(mnt->upcase);
  kfree(mnt->bitmap_chain);
  kfree(mnt->root_node);
  block_put(mnt->dev);
  kfree(mnt);
}

static void exfat_mount_put(exfat_mount_t *mnt) {
  if (__atomic_sub_fetch(&mnt->refs, 1, __ATOMIC_ACQ_REL) == 0)
    exfat_mount_free(mnt);
}

static int exfat_read_vol_sectors(exfat_mount_t *mnt, uint64_t vol_lba,
                                  uint32_t count, void *buffer) {
  if (!mnt || !mnt->dev || !mnt->dev->read_sectors || !buffer || count == 0)
    return -1;

  uint32_t dev_ss = mnt->dev->sector_size ? mnt->dev->sector_size : 512;
  uint32_t vol_ss = mnt->bytes_per_sector;
  if (vol_ss % dev_ss == 0) {
    uint32_t scale = vol_ss / dev_ss;
    if (count > UINT32_MAX / scale || vol_lba > UINT64_MAX / scale)
      return -1;
    return mnt->dev->read_sectors(mnt->dev, vol_lba * scale, count * scale, buffer);
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

static int exfat_write_sector(exfat_mount_t *mnt, uint64_t lba, const void *buffer) {
  uint32_t dev_size = mnt->dev->sector_size ? mnt->dev->sector_size : 512;
  uint32_t scale = mnt->bytes_per_sector / dev_size;
  if (!scale || lba >= mnt->volume_length || lba > UINT64_MAX / scale)
    return -EIO;
  return block_write_fua(mnt->dev, lba * scale, scale, buffer);
}

static bool exfat_is_valid_cluster(const exfat_mount_t *mnt, uint32_t cluster) {
  return cluster >= 2 && cluster < mnt->cluster_count + 2;
}

static int exfat_read_fat_entry(exfat_mount_t *mnt, uint32_t cluster,
                                uint8_t *sector, uint64_t *cached_lba,
                                uint32_t *next) {
  if (!exfat_is_valid_cluster(mnt, cluster))
    return -1;

  uint64_t fat_byte = (uint64_t)cluster * 4u;
  uint64_t lba = (uint64_t)mnt->fat_offset + fat_byte / mnt->bytes_per_sector;
  if (lba >= (uint64_t)mnt->fat_offset + mnt->fat_length)
    return -1;
  if (lba != *cached_lba) {
    if (exfat_read_vol_sectors(mnt, lba, 1, sector) != 0)
      return -1;
    *cached_lba = lba;
  }
  *next = exfat_le32(sector + fat_byte % mnt->bytes_per_sector);
  return 0;
}

static uint32_t exfat_next_cluster(exfat_mount_t *mnt, uint32_t cluster,
                                   bool contig) {
  if (!exfat_is_valid_cluster(mnt, cluster))
    return 0xFFFFFFFFu;
  if (contig) {
    uint32_t next = cluster + 1u;
    return exfat_is_valid_cluster(mnt, next) ? next : 0xFFFFFFFFu;
  }

  uint8_t *sector = kmalloc(mnt->bytes_per_sector);
  if (!sector)
    return 0xFFFFFFFFu;
  uint64_t cached_lba = UINT64_MAX;
  uint32_t next;
  int rc = exfat_read_fat_entry(mnt, cluster, sector, &cached_lba, &next);
  kfree(sector);
  return rc == 0 ? next : 0xFFFFFFFFu;
}

static uint64_t exfat_cluster_lba(const exfat_mount_t *mnt, uint32_t cluster) {
  return (uint64_t)mnt->cluster_heap_offset +
         (uint64_t)(cluster - 2u) * mnt->sectors_per_cluster;
}

static int exfat_read_cluster(exfat_mount_t *mnt, uint32_t cluster,
                              void *buffer) {
  if (!exfat_is_valid_cluster(mnt, cluster))
    return -1;
  uint64_t lba = exfat_cluster_lba(mnt, cluster);
  return exfat_read_vol_sectors(mnt, lba, mnt->sectors_per_cluster, buffer);
}

static unsigned exfat_month_days(unsigned year, unsigned month);

static uint32_t exfat_dos_time_to_unix(uint32_t dt, uint8_t utc_offset,
                                       uint8_t centiseconds) {
  uint32_t sec = (dt & 0x1Fu) * 2u;
  uint32_t min = (dt >> 5) & 0x3Fu;
  uint32_t hour = (dt >> 11) & 0x1Fu;
  uint32_t day = (dt >> 16) & 0x1Fu;
  uint32_t month = (dt >> 21) & 0x0Fu;
  uint32_t year = 1980u + ((dt >> 25) & 0x7Fu);
  if (month < 1 || month > 12 || day < 1 || day > exfat_month_days(year, month) ||
      hour > 23 || min > 59 || sec > 58)
    return 0;
  uint32_t days = day - 1;
  for (unsigned y = 1970; y < year; y++)
    days += 337 + exfat_month_days(y, 2);
  for (unsigned m = 1; m < month; m++)
    days += exfat_month_days(year, m);
  int32_t quarters = utc_offset & 0x7f;
  if (quarters & 0x40)
    quarters -= 128;
  uint64_t time = (uint64_t)days * 86400 + hour * 3600 + min * 60 + sec +
                  (centiseconds >= 100);
  if (utc_offset & 0x80)
    time -= quarters * 900;
  return time > UINT32_MAX ? UINT32_MAX : (uint32_t)time;
}

static bool exfat_append_utf8(char *out, size_t cap, size_t *pos, uint32_t ch) {
  size_t needed = ch < 0x80 ? 1 : (ch < 0x800 ? 2 : (ch < 0x10000 ? 3 : 4));
  if (*pos + needed >= cap)
    return false;
  if (ch < 0x80u) {
    out[(*pos)++] = (char)ch;
  } else if (ch < 0x800u) {
    out[(*pos)++] = (char)(0xC0u | (ch >> 6));
    out[(*pos)++] = (char)(0x80u | (ch & 0x3Fu));
  } else if (ch < 0x10000) {
    out[(*pos)++] = (char)(0xE0u | (ch >> 12));
    out[(*pos)++] = (char)(0x80u | ((ch >> 6) & 0x3Fu));
    out[(*pos)++] = (char)(0x80u | (ch & 0x3Fu));
  } else {
    out[(*pos)++] = (char)(0xf0u | (ch >> 18));
    out[(*pos)++] = (char)(0x80u | ((ch >> 12) & 0x3fu));
    out[(*pos)++] = (char)(0x80u | ((ch >> 6) & 0x3fu));
    out[(*pos)++] = (char)(0x80u | (ch & 0x3fu));
  }
  return true;
}

static int exfat_utf8_to_utf16(const char *name, uint16_t *out, bool creating) {
  unsigned count = 0;
  const uint8_t *p = (const uint8_t *)name;
  while (*p) {
    uint32_t ch = *p++;
    if (ch >= 0x80) {
      unsigned extra;
      uint32_t minimum;
      if (ch >= 0xc2 && ch <= 0xdf) {
        ch &= 0x1f;
        extra = 1;
        minimum = 0x80;
      } else if (ch >= 0xe0 && ch <= 0xef) {
        ch &= 0x0f;
        extra = 2;
        minimum = 0x800;
      } else if (ch >= 0xf0 && ch <= 0xf4) {
        ch &= 7;
        extra = 3;
        minimum = 0x10000;
      } else {
        return -EINVAL;
      }
      for (unsigned i = 0; i < extra; i++) {
        if ((*p & 0xc0) != 0x80)
          return -EINVAL;
        ch = (ch << 6) | (*p++ & 0x3f);
      }
      if (ch < minimum || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff))
        return -EINVAL;
    }
    if (creating && (ch < 32 || (ch < 128 && strchr("\"*/:<>?\\|", ch))))
      return -EINVAL;
    if (count + (ch > 0xffff ? 2u : 1u) > 255)
      return -ENAMETOOLONG;
    if (ch > 0xffff) {
      ch -= 0x10000;
      out[count++] = 0xd800 | (ch >> 10);
      ch = 0xdc00 | (ch & 0x3ff);
    }
    out[count++] = (uint16_t)ch;
  }
  if (!count || (creating && (out[count - 1] == '.' || out[count - 1] == ' ')))
    return -EINVAL;
  return (int)count;
}

static bool exfat_name_equal(exfat_mount_t *mnt, const char *a, const char *b) {
  if (!mnt->upcase)
    return strcasecmp(a, b) == 0;
  uint16_t left[255], right[255];
  int n = exfat_utf8_to_utf16(a, left, false);
  if (n < 0 || n != exfat_utf8_to_utf16(b, right, false))
    return false;
  for (int i = 0; i < n; i++) {
    if (mnt->upcase[left[i]] != mnt->upcase[right[i]])
      return false;
  }
  return true;
}

typedef struct {
  exfat_mount_t *mnt;
  uint32_t cluster;
  bool contig;
  uint32_t offset_in_cluster;
  uint32_t entries_per_cluster;
  uint8_t *buf;
  uint32_t clusters_left;
  bool bounded;
  int error;
  bool eod;
} exfat_dir_cursor_t;

static int exfat_cursor_init(exfat_dir_cursor_t *cur, exfat_mount_t *mnt,
                             uint32_t cluster, bool contig) {
  memset(cur, 0, sizeof(*cur));
  cur->mnt = mnt;
  cur->cluster = cluster;
  cur->contig = contig;
  cur->entries_per_cluster = mnt->bytes_per_cluster / 32u;
  cur->clusters_left = mnt->cluster_count - 1;
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
    if (cur->bounded && cur->contig && !cur->clusters_left) {
      cur->eod = true;
      return NULL;
    }
    cur->cluster = exfat_next_cluster(cur->mnt, cur->cluster, cur->contig);
    if (!cur->clusters_left && exfat_is_valid_cluster(cur->mnt, cur->cluster)) {
      cur->error = -EIO;
      cur->eod = true;
      return NULL;
    }
    if (!exfat_is_valid_cluster(cur->mnt, cur->cluster)) {
      cur->eod = true;
      return NULL;
    }
    if (exfat_read_cluster(cur->mnt, cur->cluster, cur->buf) != 0) {
      cur->error = -EIO;
      cur->eod = true;
      return NULL;
    }
    cur->offset_in_cluster = 0;
    cur->clusters_left--;
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
  uint64_t slots[EXFAT_MAX_ENTRIES];
  uint8_t count;
} exfat_entry_location_t;

typedef struct {
  char name[768];
  exfat_entry_location_t location;
  uint32_t first_cluster;
  uint64_t valid_data_length;
  uint64_t data_length;
  uint16_t attributes;
  bool contig;
  bool checksum_ok;
  uint32_t ctime;
  uint32_t mtime;
  uint32_t atime;
} exfat_entry_info_t;

typedef struct exfat_node {
  vfs_node_t node;
  exfat_entry_location_t location;
  struct exfat_node *next;
  uint32_t parent_cluster;
  uint64_t data_length;
  uint64_t valid_data_length;
  bool fat_verified;
  bool unlinked;
} exfat_node_t;

static void exfat_node_destroy(vfs_node_t *node);

static uint64_t exfat_cursor_slot(const exfat_dir_cursor_t *cur) {
  return exfat_cluster_lba(cur->mnt, cur->cluster) * cur->mnt->bytes_per_sector +
         (cur->offset_in_cluster - 1) * EXFAT_ENTRY_SIZE;
}

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

    exfat_entry_location_t location = {.count = sec_count + 1};
    location.slots[0] = exfat_cursor_slot(cur);
    uint16_t attrs = exfat_le16(e + 4);
    uint16_t expected_checksum = exfat_le16(e + 2);
    uint16_t checksum = exfat_entry_checksum(0, e, true);
    uint32_t ctime = exfat_dos_time_to_unix(exfat_le32(e + 8), e[22], e[20]);
    uint32_t mtime = exfat_dos_time_to_unix(exfat_le32(e + 12), e[23], e[21]);
    uint32_t atime = exfat_dos_time_to_unix(exfat_le32(e + 16), e[24], 0);

    const uint8_t *stream = exfat_cursor_raw_next(cur);
    if (!stream)
      return false;
    location.slots[1] = exfat_cursor_slot(cur);
    uint8_t remaining = sec_count - 1u;
    if (stream[0] != EXFAT_TYPE_STREAM) {
      for (uint8_t i = 0; i < remaining; i++)
        if (!exfat_cursor_raw_next(cur))
          return false;
      continue;
    }

    bool contig = (stream[1] & EXFAT_FLAG_NO_FAT_CHAIN) != 0;
    checksum = exfat_entry_checksum(checksum, stream, false);
    uint8_t name_len = stream[3];
    uint64_t valid_data_len = exfat_le64(stream + 8);
    uint32_t first_cluster = exfat_le32(stream + 20);
    uint64_t data_len = exfat_le64(stream + 24);

    char name[768];
    size_t pos = 0;
    uint16_t chars_left = name_len;
    uint16_t high = 0;
    bool ok = name_len > 0 && valid_data_len <= data_len &&
              (data_len == 0 || exfat_is_valid_cluster(cur->mnt, first_cluster));

    for (uint8_t i = 0; i < remaining; i++) {
      const uint8_t *ne = exfat_cursor_raw_next(cur);
      if (!ne)
        return false;
      location.slots[i + 2] = exfat_cursor_slot(cur);
      checksum = exfat_entry_checksum(checksum, ne, false);
      if (ne[0] != EXFAT_TYPE_NAME) {
        if (chars_left || (ne[0] & 0xe0) != 0xe0)
          ok = false;
        continue;
      }
      for (int k = 0; k < 15 && chars_left > 0; k++) {
        uint16_t ch = exfat_le16(ne + 2 + k * 2);
        if (high) {
          if (ch < 0xdc00 || ch > 0xdfff ||
              !exfat_append_utf8(name, sizeof(name), &pos,
                  0x10000u + ((high - 0xd800u) << 10) + (ch - 0xdc00u)))
            ok = false;
          high = 0;
        } else if (ch >= 0xd800 && ch <= 0xdbff) {
          high = ch;
        } else if (!ch || (ch >= 0xdc00 && ch <= 0xdfff) ||
                   !exfat_append_utf8(name, sizeof(name), &pos, ch)) {
          ok = false;
        }
        chars_left--;
      }
    }

    if (!ok || !pos || chars_left || high || pos >= sizeof(out->name))
      continue;
    name[pos] = '\0';

    memset(out, 0, sizeof(*out));
    out->location = location;
    memcpy(out->name, name, pos + 1);
    out->first_cluster = first_cluster;
    out->valid_data_length = valid_data_len;
    out->data_length = data_len;
    out->attributes = attrs;
    out->contig = contig;
    out->checksum_ok = checksum == expected_checksum;
    out->ctime = ctime;
    out->mtime = mtime;
    out->atime = atime;
    return true;
  }
}

static struct dirent *exfat_readdir_impl(vfs_node_t *node, uint64_t index);
static vfs_node_t *exfat_finddir_impl(vfs_node_t *node, char *name);

static uint32_t exfat_read_impl(vfs_node_t *node, uint64_t offset,
                                uint32_t size, uint8_t *buffer) {
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt || !buffer)
    return 0;

  exfat_node_t *entry = (exfat_node_t *)node;
  uint64_t file_size = entry->data_length;
  if (offset >= file_size || size == 0)
    return 0;
  if (size > file_size - offset)
    size = file_size - offset;

  bool to_user = is_user_ptr((uint64_t)buffer);
  uint64_t available = offset < entry->valid_data_length ? entry->valid_data_length - offset : 0;
  uint32_t initialized = available > size ? size : (uint32_t)available;

  uint32_t bytes_read = 0;
  if (initialized > 0) {
    bool contig = (node->flags & FS_EXFAT_CONTIG) != 0;
    uint32_t cluster_size = mnt->bytes_per_cluster;
    uint32_t cluster = node->impl;
    uint64_t skip = offset / cluster_size;
    if (skip >= mnt->cluster_count)
      return 0;
    if (contig) {
      if (skip > mnt->cluster_count + 1 - cluster)
        return 0;
      cluster += skip;
    } else {
      while (skip > 0 && exfat_is_valid_cluster(mnt, cluster)) {
        cluster = exfat_next_cluster(mnt, cluster, false);
        skip--;
      }
    }

    uint8_t *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf)
      return 0;

    uint32_t cluster_offset = offset % cluster_size;
    while (bytes_read < initialized && exfat_is_valid_cluster(mnt, cluster)) {
      if (exfat_read_cluster(mnt, cluster, cluster_buf) != 0)
        break;

      uint32_t to_copy = cluster_size - cluster_offset;
      if (to_copy > initialized - bytes_read)
        to_copy = initialized - bytes_read;

      if (to_user) {
        unsigned long uncopied = copy_to_user(buffer + bytes_read,
                                            cluster_buf + cluster_offset, to_copy);
        bytes_read += to_copy - (uint32_t)uncopied;
        if (uncopied > 0)
          break;
      } else {
        memcpy(buffer + bytes_read, cluster_buf + cluster_offset, to_copy);
        bytes_read += to_copy;
      }

      if (bytes_read == initialized)
        break;
      cluster_offset = 0;
      cluster = exfat_next_cluster(mnt, cluster, contig);
    }
    kfree(cluster_buf);
  }

  /* DataLength includes allocated bytes that have never been initialized. */
  if (bytes_read < initialized || bytes_read == size)
    return bytes_read;
  if (to_user)
    return size - (uint32_t)clear_user(buffer + bytes_read, size - bytes_read);
  memset(buffer + bytes_read, 0, size - bytes_read);
  return size;
}

static int exfat_lock(exfat_mount_t *mnt, bool interruptible) {
  spinlock_acquire(&mnt->write_lock);
  while (mnt->writing) {
    struct thread *thread = sched_get_current();
    if (!thread || (interruptible && thread_has_pending_signal(thread))) {
      spinlock_release(&mnt->write_lock);
      return thread ? -EINTR : -EAGAIN;
    }
    wait_queue_entry_t waiter = {.thread = thread};
    thread->state = THREAD_BLOCKED;
    wait_queue_add(&mnt->write_waiters, &waiter);
    if (interruptible && thread_has_pending_signal(thread)) {
      thread->state = THREAD_RUNNING;
      wait_queue_remove(&mnt->write_waiters, &waiter);
      spinlock_release(&mnt->write_lock);
      return -EINTR;
    }
    spinlock_release(&mnt->write_lock);
    sched_yield();
    thread->state = THREAD_RUNNING;
    wait_queue_remove(&mnt->write_waiters, &waiter);
    spinlock_acquire(&mnt->write_lock);
  }
  mnt->writing = true;
  spinlock_release(&mnt->write_lock);
  return 0;
}

static int exfat_begin_write(exfat_mount_t *mnt) {
  return exfat_lock(mnt, true);
}

static void exfat_end_write(exfat_mount_t *mnt) {
  spinlock_acquire(&mnt->write_lock);
  mnt->writing = false;
  wait_queue_wake_all(&mnt->write_waiters);
  spinlock_release(&mnt->write_lock);
}

typedef struct exfat_sector_change {
  struct exfat_sector_change *next;
  uint64_t lba;
  uint8_t data[]; /* original sector followed by the proposed sector */
} exfat_sector_change_t;

typedef struct {
  exfat_mount_t *mnt;
  exfat_sector_change_t *changes;
  uint8_t *read_cache;
  uint64_t cached_lba;
  int error;
  int orphan_delta;
} exfat_transaction_t;

static uint8_t *exfat_tx_sector(exfat_transaction_t *tx, uint64_t lba,
                                 bool modify) {
  if (tx->error)
    return NULL;
  uint32_t size = tx->mnt->bytes_per_sector;
  for (exfat_sector_change_t *s = tx->changes; s; s = s->next) {
    if (s->lba == lba)
      return s->data + size;
  }
  if (!tx->read_cache) {
    tx->read_cache = kmalloc(size);
    if (!tx->read_cache) {
      tx->error = -ENOMEM;
      return NULL;
    }
    tx->cached_lba = UINT64_MAX;
  }
  if (lba != tx->cached_lba) {
    if (lba >= tx->mnt->volume_length ||
        exfat_read_vol_sectors(tx->mnt, lba, 1, tx->read_cache) != 0) {
      tx->error = -EIO;
      return NULL;
    }
    tx->cached_lba = lba;
  }
  if (!modify)
    return tx->read_cache;

  exfat_sector_change_t *change = kmalloc(sizeof(*change) + 2 * size);
  if (!change) {
    tx->error = -ENOMEM;
    return NULL;
  }
  change->lba = lba;
  memcpy(change->data, tx->read_cache, size);
  memcpy(change->data + size, tx->read_cache, size);
  change->next = tx->changes;
  tx->changes = change;
  return change->data + size;
}

static void exfat_tx_discard(exfat_transaction_t *tx) {
  while (tx->changes) {
    exfat_sector_change_t *sector = tx->changes;
    tx->changes = sector->next;
    kfree(sector);
  }
  kfree(tx->read_cache);
}

static int exfat_tx_commit(exfat_transaction_t *tx) {
  if (tx->error)
    return tx->error;
  exfat_mount_t *mnt = tx->mnt;
  if (!tx->changes && !tx->orphan_delta)
    return 0;
  uint32_t size = mnt->bytes_per_sector;
  if (!tx->read_cache) {
    tx->read_cache = kmalloc(size);
    if (!tx->read_cache)
      return -ENOMEM;
  }
  uint8_t *boot = tx->read_cache;
  if (exfat_read_vol_sectors(mnt, 0, 1, boot) != 0)
    return -EIO;
  uint16_t flags = exfat_le16(boot + 106);
  if ((flags & 2) && !mnt->dirty)
    return -EIO;

  /* VolumeFlags is excluded from the boot checksum. Keep VolumeDirty set
   * until all metadata writes, or all rollback writes, have reached disk. */
  if (!mnt->dirty) {
    exfat_store16(boot + 106, flags | 2u);
    boot[112] = 0xff; /* Allocation bitmap is authoritative. */
    if (exfat_write_sector(mnt, 0, boot) != 0) {
      mnt->write_failed = true;
      return -EIO;
    }
    mnt->dirty = true;
  }
  exfat_sector_change_t *failed = NULL;
  for (exfat_sector_change_t *s = tx->changes; s; s = s->next) {
    if (exfat_write_sector(mnt, s->lba, s->data + size) != 0) {
      failed = s;
      break;
    }
  }
  if (failed) {
    for (exfat_sector_change_t *s = tx->changes; s; s = s->next) {
      if (exfat_write_sector(mnt, s->lba, s->data) != 0)
        mnt->write_failed = true;
      if (s == failed)
        break;
    }
  }
  if (mnt->write_failed)
    return -EIO;
  if (!failed)
    mnt->orphans += tx->orphan_delta;
  if (!mnt->orphans) {
    exfat_store16(boot + 106, flags & ~2u);
    if (exfat_write_sector(mnt, 0, boot) != 0) {
      /* Publish the committed metadata, but stop further mutations. */
      mnt->write_failed = true;
    } else {
      mnt->dirty = false;
    }
  }
  return failed ? -EIO : 0;
}

static uint32_t exfat_tx_fat(exfat_transaction_t *tx, uint32_t cluster,
                             bool modify, uint32_t value) {
  if (!exfat_is_valid_cluster(tx->mnt, cluster)) {
    tx->error = -EIO;
    return EXFAT_EOC;
  }
  uint64_t byte = (uint64_t)cluster * 4;
  uint32_t size = tx->mnt->bytes_per_sector;
  uint8_t *sector = exfat_tx_sector(tx, tx->mnt->fat_offset + byte / size, modify);
  if (!sector)
    return EXFAT_EOC;
  uint8_t *entry = sector + byte % size;
  if (modify)
    exfat_store32(entry, value);
  return exfat_le32(entry);
}

static int exfat_tx_bitmap(exfat_transaction_t *tx, uint32_t cluster,
                            bool modify, bool allocated) {
  if (!exfat_is_valid_cluster(tx->mnt, cluster)) {
    tx->error = -EIO;
    return -1;
  }
  uint32_t bit = cluster - 2;
  uint32_t byte = bit / 8;
  exfat_mount_t *mnt = tx->mnt;
  uint32_t bitmap = mnt->bitmap_chain[byte / mnt->bytes_per_cluster];
  uint32_t offset = byte % mnt->bytes_per_cluster;
  uint64_t lba = exfat_cluster_lba(mnt, bitmap) + offset / mnt->bytes_per_sector;
  uint8_t *sector = exfat_tx_sector(tx, lba, modify);
  if (!sector)
    return -1;
  uint8_t *map = sector + offset % mnt->bytes_per_sector;
  uint8_t mask = (uint8_t)(1u << (bit % 8));
  if (modify) {
    if (allocated)
      *map |= mask;
    else
      *map &= (uint8_t)~mask;
  }
  return (*map & mask) != 0;
}

static uint32_t exfat_alloc_cluster(exfat_transaction_t *tx) {
  exfat_mount_t *mnt = tx->mnt;
  uint32_t cluster = mnt->alloc_hint;
  for (uint32_t i = 0; i < mnt->cluster_count; i++) {
    if (!exfat_is_valid_cluster(mnt, cluster))
      cluster = 2;
    int used = exfat_tx_bitmap(tx, cluster, false, false);
    if (used < 0)
      return 0;
    if (!used) {
      exfat_tx_bitmap(tx, cluster, true, true);
      exfat_tx_fat(tx, cluster, true, EXFAT_EOC);
      mnt->alloc_hint = cluster + 1;
      return tx->error ? 0 : cluster;
    }
    cluster++;
  }
  tx->error = -ENOSPC;
  return 0;
}

typedef struct {
  uint32_t first;
  uint64_t length;
  uint64_t valid;
  bool contiguous;
} exfat_stream_t;

static exfat_stream_t exfat_node_stream(const exfat_node_t *entry) {
  return (exfat_stream_t){entry->node.impl, entry->data_length,
                         entry->valid_data_length,
                         (entry->node.flags & FS_EXFAT_CONTIG) != 0};
}

static void exfat_apply_stream(exfat_node_t *entry, const exfat_stream_t *stream) {
  entry->node.impl = stream->first;
  __atomic_store_n(&entry->data_length, stream->length, __ATOMIC_RELEASE);
  entry->valid_data_length = stream->valid;
  entry->fat_verified = true;
  if (stream->contiguous)
    entry->node.flags |= FS_EXFAT_CONTIG;
  else
    entry->node.flags &= ~FS_EXFAT_CONTIG;
  __atomic_store_n(&entry->node.length, stream->length > UINT32_MAX
                       ? UINT32_MAX : (uint32_t)stream->length, __ATOMIC_RELEASE);
}

static uint32_t exfat_tx_next(exfat_transaction_t *tx,
                              const exfat_stream_t *stream, uint32_t cluster) {
  return stream->contiguous ? cluster + 1 : exfat_tx_fat(tx, cluster, false, 0);
}

static int exfat_validate_stream(exfat_transaction_t *tx, exfat_node_t *entry) {
  if (entry->fat_verified)
    return 0;
  exfat_stream_t stream = exfat_node_stream(entry);
  if (!stream.length)
    return stream.first ? -EIO : 0;
  uint32_t cluster = stream.first;
  uint32_t count = (stream.length - 1) / tx->mnt->bytes_per_cluster + 1;
  for (uint32_t i = 0; i < count; i++) {
    if (exfat_tx_bitmap(tx, cluster, false, false) != 1)
      return tx->error ? tx->error : -EIO;
    cluster = exfat_tx_next(tx, &stream, cluster);
  }
  if (tx->error || (!stream.contiguous && cluster != EXFAT_EOC))
    return -EIO;
  entry->fat_verified = true;
  return 0;
}

static int exfat_resize_stream(exfat_transaction_t *tx, exfat_stream_t *stream,
                                uint64_t length) {
  uint32_t cluster_size = tx->mnt->bytes_per_cluster;
  uint64_t capacity = (uint64_t)tx->mnt->cluster_count * cluster_size;
  if (length > capacity)
    return -ENOSPC;
  if (stream->length > capacity)
    return -EIO;
  uint32_t old_count = stream->length ? (stream->length - 1) / cluster_size + 1 : 0;
  uint32_t new_count = length ? (length - 1) / cluster_size + 1 : 0;
  uint32_t last = 0;
  if (old_count != new_count) {
    uint32_t cluster = stream->first;
    for (uint32_t i = 0; i < old_count; i++) {
      if (exfat_tx_bitmap(tx, cluster, false, false) != 1)
        return tx->error ? tx->error : -EIO;
      uint32_t next = exfat_tx_next(tx, stream, cluster);
      if (i >= new_count) {
        exfat_tx_bitmap(tx, cluster, true, false);
        exfat_tx_fat(tx, cluster, true, 0);
      } else {
        last = cluster;
        if (stream->contiguous && new_count > old_count)
          exfat_tx_fat(tx, cluster, true, EXFAT_EOC);
      }
      if (stream->contiguous && new_count > old_count && i + 1 < old_count)
        exfat_tx_fat(tx, cluster, true, next);
      cluster = next;
    }
    if (old_count && !stream->contiguous && cluster != EXFAT_EOC)
      return -EIO;
    if (new_count < old_count && last && !stream->contiguous)
      exfat_tx_fat(tx, last, true, EXFAT_EOC);
    for (uint32_t i = old_count; i < new_count && !tx->error; i++) {
      uint32_t allocated = exfat_alloc_cluster(tx);
      if (!allocated)
        break;
      if (last)
        exfat_tx_fat(tx, last, true, allocated);
      else
        stream->first = allocated;
      last = allocated;
    }
    if (!new_count)
      stream->first = 0;
    if (new_count > old_count || !new_count)
      stream->contiguous = false;
  }
  stream->length = length;
  if (stream->valid > length)
    stream->valid = length;
  return tx->error;
}

static uint8_t *exfat_tx_slot(exfat_transaction_t *tx, uint64_t position,
                              bool modify) {
  uint32_t size = tx->mnt->bytes_per_sector;
  uint8_t *sector = exfat_tx_sector(tx, position / size, modify);
  return sector ? sector + position % size : NULL;
}

static int exfat_load_set(exfat_transaction_t *tx,
                          const exfat_entry_location_t *location,
                          uint8_t set[EXFAT_MAX_ENTRIES][EXFAT_ENTRY_SIZE]) {
  for (uint8_t i = 0; i < location->count; i++) {
    const uint8_t *slot = exfat_tx_slot(tx, location->slots[i], false);
    if (!slot)
      return tx->error;
    memcpy(set[i], slot, EXFAT_ENTRY_SIZE);
  }
  return 0;
}

static int exfat_store_set(exfat_transaction_t *tx,
                           const exfat_entry_location_t *location,
                           uint8_t set[EXFAT_MAX_ENTRIES][EXFAT_ENTRY_SIZE]) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < location->count; i++)
    sum = exfat_entry_checksum(sum, set[i], i == 0);
  exfat_store16(set[0] + 2, sum);
  for (uint8_t i = 0; i < location->count; i++) {
    uint8_t *slot = exfat_tx_slot(tx, location->slots[i], true);
    if (!slot)
      return tx->error;
    memcpy(slot, set[i], EXFAT_ENTRY_SIZE);
  }
  return 0;
}

static unsigned exfat_month_days(unsigned year, unsigned month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = !(year % 4) && ((year % 100) || !(year % 400));
  return days[month - 1] + (month == 2 && leap);
}

static uint32_t exfat_disk_time(uint64_t timestamp) {
  if (timestamp < 315532800)
    timestamp = 315532800;
  if (timestamp >= 4354819200ULL)
    timestamp = 4354819199ULL;
  uint32_t days = (uint32_t)(timestamp / 86400);
  unsigned year = 1970;
  while (days >= 337 + exfat_month_days(year, 2)) {
    days -= 337 + exfat_month_days(year, 2);
    year++;
  }
  unsigned month = 1;
  while (days >= exfat_month_days(year, month))
    days -= exfat_month_days(year, month++);
  return ((year - 1980) << 25) | (month << 21) | ((days + 1) << 16) |
         ((timestamp / 3600 % 24) << 11) | ((timestamp / 60 % 60) << 5) |
         (timestamp % 60 / 2);
}

static uint8_t exfat_disk_cs(uint32_t ts) {
  return (ts >= 315532800u && (ts & 1u)) ? 100u : 0u;
}

static void exfat_set_stream(uint8_t *entry, const exfat_stream_t *stream) {
  entry[1] = 1u | (stream->contiguous ? EXFAT_FLAG_NO_FAT_CHAIN : 0);
  exfat_store64(entry + 8, stream->valid);
  exfat_store32(entry + 20, stream->first);
  exfat_store64(entry + 24, stream->length);
}

static int exfat_update_stream(exfat_transaction_t *tx, const exfat_node_t *entry,
                                const exfat_stream_t *stream, uint32_t now) {
  if (entry->unlinked)
    return 0;
  uint8_t set[EXFAT_MAX_ENTRIES][EXFAT_ENTRY_SIZE];
  int rc = exfat_load_set(tx, &entry->location, set);
  if (rc)
    return rc;
  if (entry->location.count < 3 || set[0][0] != EXFAT_TYPE_FILE ||
      set[1][0] != EXFAT_TYPE_STREAM)
    return -EIO;
  exfat_set_stream(set[1], stream);
  exfat_store32(set[0] + 12, exfat_disk_time(now));
  set[0][21] = exfat_disk_cs(now);
  set[0][23] = 0x80; /* UTC */
  if (!(exfat_le16(set[0] + 4) & EXFAT_ATTR_DIR))
    set[0][4] |= 0x20; /* Archive */
  return exfat_store_set(tx, &entry->location, set);
}

static uint64_t exfat_write_range(exfat_transaction_t *tx, const exfat_stream_t *stream,
                                  uint64_t offset, uint64_t size,
                                  const uint8_t *data, uint8_t *sector) {
  exfat_mount_t *mnt = tx->mnt;
  uint32_t cluster_size = mnt->bytes_per_cluster;
  uint32_t sector_size = mnt->bytes_per_sector;
  uint32_t cluster = stream->first;
  uint64_t skip = offset / cluster_size;
  if (!exfat_is_valid_cluster(mnt, cluster) || skip >= mnt->cluster_count)
    return 0;
  if (stream->contiguous) {
    if (skip > mnt->cluster_count + 1 - cluster)
      return 0;
    cluster += (uint32_t)skip;
  } else {
    while (skip-- > 0 && !tx->error)
      cluster = exfat_tx_next(tx, stream, cluster);
  }

  uint32_t within = offset % cluster_size;
  uint64_t done = 0;
  while (done < size && exfat_tx_bitmap(tx, cluster, false, false) == 1) {
    uint32_t start = within % sector_size;
    uint32_t chunk = sector_size - start;
    if (chunk > size - done)
      chunk = size - done;
    uint64_t lba = exfat_cluster_lba(mnt, cluster) + within / sector_size;
    if (chunk < sector_size && exfat_read_vol_sectors(mnt, lba, 1, sector) != 0)
      break;
    if (data)
      memcpy(sector + start, data + done, chunk);
    else
      memset(sector + start, 0, chunk);
    if (exfat_write_sector(mnt, lba, sector) != 0)
      break;
    done += chunk;
    within += chunk;
    if (within == cluster_size && done < size) {
      within = 0;
      cluster = exfat_tx_next(tx, stream, cluster);
    }
  }
  return done;
}

static uint32_t exfat_write_file(vfs_node_t *node, uint64_t offset,
                                 uint32_t size, uint8_t *buffer,
                                 uint64_t *append_end, bool writeback) {
  if (!size)
    return 0;
  exfat_mount_t *mnt = node->device;
  exfat_node_t *entry = (exfat_node_t *)node;
  uint8_t *staged = NULL;
  if (is_user_ptr((uint64_t)buffer)) {
    staged = kmalloc(size);
    if (!staged)
      return (uint32_t)-ENOMEM;
    if (copy_from_user(staged, buffer, size)) {
      kfree(staged);
      return (uint32_t)-EFAULT;
    }
    buffer = staged;
  }
  uint32_t written = 0;
  int rc = exfat_begin_write(mnt);
  if (rc) {
    kfree(staged);
    return (uint32_t)rc;
  }
  exfat_transaction_t tx = {.mnt = mnt};
  uint8_t *sector = NULL;
  exfat_stream_t stream = exfat_node_stream(entry);
  uint64_t old_length = stream.length;
  uint64_t old_valid = stream.valid;
  if (append_end)
    offset = stream.length;
  if (mnt->write_failed) {
    rc = -EROFS;
    goto out;
  }
  if (offset > INT64_MAX || size > (uint64_t)INT64_MAX - offset) {
    rc = -EFBIG;
    goto out;
  }
  uint64_t end = offset + size;
  if (mnt->overwrite_only && end > old_valid) {
    rc = -EOPNOTSUPP;
    goto out;
  }
  rc = exfat_validate_stream(&tx, entry);
  if (rc)
    goto out;
  uint32_t now = (uint32_t)rtc_get_timestamp();
  if (end > stream.length && (rc = exfat_resize_stream(&tx, &stream, end)) != 0)
    goto out;
  if (end > stream.valid)
    stream.valid = end;
  rc = mnt->overwrite_only ? 0 : exfat_update_stream(&tx, entry, &stream, now);
  if (rc)
    goto out;
  sector = kmalloc(mnt->bytes_per_sector);
  if (!sector) {
    rc = -ENOMEM;
    goto out;
  }
  if (offset > old_valid &&
      exfat_write_range(&tx, &stream, old_valid, offset - old_valid, NULL, sector) !=
          offset - old_valid) {
    rc = -EIO;
    goto out;
  }
  written = exfat_write_range(&tx, &stream, offset, size, buffer, sector);
  rc = written == size ? exfat_tx_commit(&tx) : -EIO;
  if (!rc) {
    exfat_apply_stream(entry, &stream);
    if (!mnt->overwrite_only)
      node->mtime = node->ctime = now;
  } else {
    uint64_t visible = offset < old_valid ? old_valid - offset : 0;
    if (written > visible)
      written = visible;
  }
  if (written && append_end)
    *append_end = offset + written;
  if (!writeback && written && (node->flags & FS_PAGE_CACHE)) {
    if (old_length <= UINT32_MAX && stream.length > UINT32_MAX)
      vfs_cache_invalidate(node, 0xFFFFF000u);
    if (offset < 0x100000000ULL) {
      uint64_t pc_limit = stream.length > UINT32_MAX ? 0xFFFFF000ULL : 0x100000000ULL;
      if (offset < pc_limit) {
        uint64_t max_len = pc_limit - offset;
        uint32_t cache_len = written < max_len ? written : (uint32_t)max_len;
        vfs_cache_update_or_invalidate(node, (uint32_t)offset, cache_len, buffer);
      }
      if (pc_limit == 0xFFFFF000ULL && offset + written > 0xFFFFF000ULL)
        vfs_cache_invalidate(node, 0xFFFFF000u);
    }
  }
out:
  exfat_tx_discard(&tx);
  kfree(sector);
  exfat_end_write(mnt);
  kfree(staged);
  return written ? written : (uint32_t)rc;
}

static uint32_t exfat_write_impl(vfs_node_t *node, uint64_t offset,
                                 uint32_t size, uint8_t *data) {
  return exfat_write_file(node, offset, size, data, NULL, false);
}

static uint32_t exfat_append_impl(vfs_node_t *node, uint32_t size, uint8_t *data,
                                  uint64_t *end) {
  return exfat_write_file(node, 0, size, data, end, false);
}

static int exfat_truncate_impl(vfs_node_t *node, uint64_t length) {
  exfat_mount_t *mnt = node->device;
  exfat_node_t *entry = (exfat_node_t *)node;
  vfs_cache_sync(node);
  int rc = exfat_begin_write(mnt);
  if (rc)
    return rc;
  exfat_transaction_t tx = {.mnt = mnt};
  exfat_stream_t stream = exfat_node_stream(entry);
  uint32_t now = (uint32_t)rtc_get_timestamp();
  rc = mnt->write_failed ? -EROFS : exfat_validate_stream(&tx, entry);
  if (!rc)
    rc = exfat_resize_stream(&tx, &stream, length);
  if (!rc)
    rc = exfat_update_stream(&tx, entry, &stream, now);
  if (!rc)
    rc = exfat_tx_commit(&tx);
  if (!rc) {
    exfat_apply_stream(entry, &stream);
    node->mtime = node->ctime = now;
    vfs_cache_clear(node);
  }
  exfat_tx_discard(&tx);
  exfat_end_write(mnt);
  return rc;
}

static bool exfat_is_root(exfat_mount_t *mnt, vfs_node_t *dir) {
  return dir->impl == mnt->root_cluster;
}

static int exfat_dir_init(exfat_dir_cursor_t *cur, vfs_node_t *dir) {
  exfat_mount_t *mnt = dir->device;
  bool root = exfat_is_root(mnt, dir);
  if (!root && ((exfat_node_t *)dir)->unlinked)
    return -ENOENT;
  int rc = exfat_cursor_init(cur, mnt, dir->impl,
                            !root && (dir->flags & FS_EXFAT_CONTIG));
  if (!rc && !root) {
    if (!dir->length || dir->length % mnt->bytes_per_cluster) {
      exfat_cursor_free(cur);
      return -EIO;
    }
    uint32_t remaining = dir->length / mnt->bytes_per_cluster - 1;
    cur->bounded = true;
    if (remaining < cur->clusters_left)
      cur->clusters_left = remaining;
  }
  return rc ? -EIO : 0;
}

/* Called under the mount's mutation gate when directory contents can change. */
static int exfat_lookup_info(vfs_node_t *dir, const char *name,
                              exfat_entry_info_t *info) {
  exfat_dir_cursor_t cursor;
  int rc = exfat_dir_init(&cursor, dir);
  if (rc)
    return rc;
  rc = -ENOENT;
  while (exfat_cursor_next_item(&cursor, info)) {
    if (exfat_name_equal(cursor.mnt, info->name, name)) {
      rc = 0;
      break;
    }
  }
  if (cursor.error)
    rc = cursor.error;
  exfat_cursor_free(&cursor);
  return rc;
}

static int exfat_zero_cluster(exfat_mount_t *mnt, uint32_t cluster) {
  uint8_t *zero = kcalloc(1, mnt->bytes_per_sector);
  if (!zero)
    return -ENOMEM;
  uint64_t lba = exfat_cluster_lba(mnt, cluster);
  int rc = 0;
  for (uint32_t i = 0; i < mnt->sectors_per_cluster; i++) {
    if (exfat_write_sector(mnt, lba + i, zero) != 0) {
      rc = -EIO;
      break;
    }
  }
  kfree(zero);
  return rc;
}

static int exfat_find_slots(exfat_transaction_t *tx, vfs_node_t *dir,
                             uint8_t count, exfat_entry_location_t *location,
                             exfat_stream_t *grown) {
  exfat_mount_t *mnt = tx->mnt;
  bool root = exfat_is_root(mnt, dir);
  exfat_stream_t stream = root ? (exfat_stream_t){.first = mnt->root_cluster}
                               : exfat_node_stream((exfat_node_t *)dir);
  uint32_t cluster = stream.first;
  uint32_t seen = 0;
  uint32_t slots_per_cluster = mnt->bytes_per_cluster / EXFAT_ENTRY_SIZE;
  bool end = false;
  location->count = 0;
  while (seen++ < mnt->cluster_count && exfat_is_valid_cluster(mnt, cluster)) {
    uint32_t capacity = root ? mnt->cluster_count : stream.length / mnt->bytes_per_cluster;
    uint64_t start = exfat_cluster_lba(mnt, cluster) * mnt->bytes_per_sector;
    uint32_t next = stream.contiguous && seen == capacity ? EXFAT_EOC
                    : exfat_tx_next(tx, &stream, cluster);
    if (tx->error)
      return tx->error;
    if ((!root && seen == capacity && next != EXFAT_EOC) ||
        (!root && seen < capacity && next == EXFAT_EOC))
      return -EIO;
    for (uint32_t i = 0; i < slots_per_cluster; i++) {
      uint64_t position = start + (uint64_t)i * EXFAT_ENTRY_SIZE;
      uint8_t *slot = exfat_tx_slot(tx, position, false);
      if (!slot)
        return tx->error;
      if (!slot[0])
        end = true;
      if (!end && (slot[0] & 0x80)) {
        location->count = 0;
        continue;
      }
      location->slots[location->count++] = position;
      if (location->count == count) {
        if (end && (i + 1 < slots_per_cluster || exfat_is_valid_cluster(mnt, next))) {
          uint64_t after = i + 1 < slots_per_cluster
              ? position + EXFAT_ENTRY_SIZE
              : exfat_cluster_lba(mnt, next) * mnt->bytes_per_sector;
          uint8_t *marker = exfat_tx_slot(tx, after, true);
          if (!marker)
            return tx->error;
          memset(marker, 0, EXFAT_ENTRY_SIZE);
        }
        return 0;
      }
    }
    if (next == EXFAT_EOC) {
      if (root) {
        next = exfat_alloc_cluster(tx);
        if (next)
          exfat_tx_fat(tx, cluster, true, next);
      } else {
        if (stream.length > UINT32_MAX - mnt->bytes_per_cluster)
          return -ENOSPC;
        int rc = exfat_resize_stream(tx, &stream, stream.length + mnt->bytes_per_cluster);
        if (rc)
          return rc;
        next = exfat_tx_fat(tx, cluster, false, 0);
        stream.valid = stream.length;
        *grown = stream;
      }
      if (!next || tx->error)
        return tx->error ? tx->error : -EIO;
      int rc = exfat_zero_cluster(mnt, next);
      if (rc)
        return rc;
      /* Allocation may have cached a sector before the direct zeroing. */
      tx->cached_lba = UINT64_MAX;
    }
    cluster = next;
  }
  return -EIO;
}

static void exfat_set_name(exfat_mount_t *mnt,
                           uint8_t set[EXFAT_MAX_ENTRIES][EXFAT_ENTRY_SIZE],
                           const uint16_t *name, unsigned length) {
  uint16_t hash = 0;
  unsigned entries = (length + 14) / 15;
  set[0][1] = (uint8_t)(entries + 1);
  set[1][3] = (uint8_t)length;
  memset(set[2], 0, entries * EXFAT_ENTRY_SIZE);
  for (unsigned i = 0; i < entries; i++)
    set[i + 2][0] = EXFAT_TYPE_NAME;
  for (unsigned i = 0; i < length; i++) {
    uint16_t upper = mnt->upcase[name[i]];
    exfat_store16(set[2 + i / 15] + 2 + i % 15 * 2, name[i]);
    hash = (uint16_t)((hash >> 1) + ((hash & 1u) << 15) + (upper & 255));
    hash = (uint16_t)((hash >> 1) + ((hash & 1u) << 15) + (upper >> 8));
  }
  exfat_store16(set[1] + 4, hash);
}

static int exfat_create_node(vfs_node_t *dir, const char *name,
                              uint16_t permission, bool directory) {
  if (strlen(name) >= sizeof(dir->name))
    return -ENAMETOOLONG;
  uint16_t utf16[255];
  int len = exfat_utf8_to_utf16(name, utf16, true);
  if (len < 0)
    return len;
  exfat_mount_t *mnt = dir->device;
  int rc = exfat_begin_write(mnt);
  if (rc)
    return rc;
  exfat_transaction_t tx = {.mnt = mnt};
  exfat_entry_info_t info;
  exfat_stream_t stream = {0}, grown = {0};
  exfat_entry_location_t location;
  uint8_t set[EXFAT_MAX_ENTRIES][EXFAT_ENTRY_SIZE] = {{0}};
  rc = mnt->write_failed ? -EROFS : exfat_lookup_info(dir, name, &info);
  if (rc != -ENOENT) {
    if (!rc)
      rc = -EEXIST;
    goto out;
  }
  if (directory) {
    rc = exfat_resize_stream(&tx, &stream, mnt->bytes_per_cluster);
    if (rc || (rc = exfat_zero_cluster(mnt, stream.first)) != 0)
      goto out;
    stream.valid = stream.length;
  }
  rc = exfat_find_slots(&tx, dir, 2 + (len + 14) / 15, &location, &grown);
  if (rc)
    goto out;
  uint32_t now = (uint32_t)rtc_get_timestamp();
  if (grown.length && (rc = exfat_update_stream(&tx, (exfat_node_t *)dir, &grown, now)))
    goto out;
  set[0][0] = EXFAT_TYPE_FILE;
  uint16_t attributes = directory ? EXFAT_ATTR_DIR : 0x20;
  if (!directory && !(permission & 0222))
    attributes |= EXFAT_ATTR_READONLY;
  exfat_store16(set[0] + 4, attributes);
  uint32_t stamp = exfat_disk_time(now);
  uint8_t cs = exfat_disk_cs(now);
  for (unsigned i = 0; i < 3; i++) {
    exfat_store32(set[0] + 8 + i * 4, stamp);
    set[0][22 + i] = 0x80;
  }
  set[0][20] = cs;
  set[0][21] = cs;
  set[1][0] = EXFAT_TYPE_STREAM;
  exfat_set_stream(set[1], &stream);
  exfat_set_name(mnt, set, utf16, (unsigned)len);
  rc = exfat_store_set(&tx, &location, set);
  if (!rc)
    rc = exfat_tx_commit(&tx);
  if (!rc && grown.length)
    exfat_apply_stream((exfat_node_t *)dir, &grown);
out:
  exfat_tx_discard(&tx);
  exfat_end_write(mnt);
  return rc;
}

static int exfat_create_impl(vfs_node_t *node, char *name, uint16_t permission) {
  return exfat_create_node(node, name, permission, false);
}

static int exfat_mkdir_impl(vfs_node_t *node, char *name, uint16_t permission) {
  return exfat_create_node(node, name, permission, true);
}

static exfat_node_t *exfat_live_ref(exfat_mount_t *mnt, uint64_t slot) {
  exfat_node_t *result = NULL;
  spinlock_acquire(&mnt->nodes_lock);
  for (exfat_node_t *entry = mnt->nodes; entry; entry = entry->next) {
    if (!entry->unlinked && entry->location.slots[0] == slot &&
        vfs_node_tryref(&entry->node)) {
      result = entry;
      break;
    }
  }
  spinlock_release(&mnt->nodes_lock);
  return result;
}

static void exfat_node_destroy(vfs_node_t *node) {
  exfat_mount_t *mnt = node->device;
  if (!mnt || ((node->flags & FS_PERSISTENT) && !(node->flags & FS_DELETED)))
    return;
  exfat_node_t *entry = (exfat_node_t *)node;
  if (mnt->writable && !mnt->overwrite_only) {
    spinlock_acquire(&mnt->nodes_lock);
    exfat_node_t **link = &mnt->nodes;
    while (*link && *link != entry)
      link = &(*link)->next;
    if (*link)
      *link = entry->next;
    spinlock_release(&mnt->nodes_lock);
    if (entry->unlinked) {
      int rc = exfat_lock(mnt, false);
      if (!rc) {
        exfat_transaction_t tx = {.mnt = mnt, .orphan_delta = -1};
        exfat_stream_t stream = exfat_node_stream(entry);
        rc = mnt->write_failed ? -EROFS : exfat_resize_stream(&tx, &stream, 0);
        if (!rc)
          rc = exfat_tx_commit(&tx);
        if (rc)
          mnt->write_failed = true;
        exfat_tx_discard(&tx);
        exfat_end_write(mnt);
      }
    }
  }
  node->device = NULL;
  exfat_mount_put(mnt);
}

static int exfat_empty_directory(exfat_mount_t *mnt,
                                 const exfat_entry_info_t *info) {
  if (!info->data_length || info->data_length % mnt->bytes_per_cluster ||
      info->data_length > UINT32_MAX)
    return -EIO;
  exfat_dir_cursor_t cursor;
  if (exfat_cursor_init(&cursor, mnt, info->first_cluster, info->contig) != 0)
    return -EIO;
  cursor.clusters_left = (uint32_t)(info->data_length / mnt->bytes_per_cluster - 1);
  cursor.bounded = true;
  const uint8_t *slot;
  int rc = 0;
  while ((slot = exfat_cursor_raw_next(&cursor))) {
    if (slot[0] & 0x80) {
      rc = -ENOTEMPTY;
      break;
    }
  }
  if (cursor.error)
    rc = cursor.error;
  exfat_cursor_free(&cursor);
  return rc;
}

static int exfat_clear_set(exfat_transaction_t *tx,
                           const exfat_entry_info_t *info) {
  if (!info->checksum_ok)
    return -EOPNOTSUPP;
  for (uint8_t i = 0; i < info->location.count; i++) {
    uint8_t *slot = exfat_tx_slot(tx, info->location.slots[i], true);
    if (!slot)
      return tx->error;
    slot[0] &= 0x7f;
  }
  return 0;
}

static int exfat_remove_set(exfat_transaction_t *tx,
                            const exfat_entry_info_t *info, exfat_node_t *live) {
  int rc = exfat_clear_set(tx, info);
  if (rc)
    return rc;
  if (live) {
    tx->orphan_delta++;
    return 0;
  }
  exfat_stream_t stream = {info->first_cluster, info->data_length,
                           info->valid_data_length, info->contig};
  return exfat_resize_stream(tx, &stream, 0);
}

static void exfat_mark_unlinked(exfat_mount_t *mnt, exfat_node_t *entry) {
  spinlock_acquire(&mnt->nodes_lock);
  entry->unlinked = true;
  spinlock_release(&mnt->nodes_lock);
}

static int exfat_remove_node(vfs_node_t *dir, const char *name, bool directory) {
  exfat_mount_t *mnt = dir->device;
  int rc = exfat_begin_write(mnt);
  if (rc)
    return rc;
  exfat_transaction_t tx = {.mnt = mnt};
  exfat_entry_info_t info;
  exfat_node_t *live = NULL;
  rc = mnt->write_failed ? -EROFS : exfat_lookup_info(dir, name, &info);
  if (rc)
    goto out;
  if (directory != !!(info.attributes & EXFAT_ATTR_DIR)) {
    rc = directory ? -ENOTDIR : -EISDIR;
    goto out;
  }
  if (directory && (rc = exfat_empty_directory(mnt, &info)))
    goto out;
  live = exfat_live_ref(mnt, info.location.slots[0]);
  rc = exfat_remove_set(&tx, &info, live);
  if (!rc)
    rc = exfat_tx_commit(&tx);
  if (!rc && live)
    exfat_mark_unlinked(mnt, live);
out:
  exfat_tx_discard(&tx);
  exfat_end_write(mnt);
  if (live) {
    if (!rc && directory)
      vfs_node_retire(&live->node);
    vfs_close(&live->node);
  }
  return rc;
}

static int exfat_unlink_impl(vfs_node_t *node, char *name) {
  return exfat_remove_node(node, name, false);
}

static int exfat_rmdir_impl(vfs_node_t *node, char *name) {
  return exfat_remove_node(node, name, true);
}

typedef struct {
  uint32_t cluster;
  uint64_t data_length;
  bool contig;
} exfat_dir_visit_t;

static int exfat_subtree_contains(exfat_mount_t *mnt,
                                  const exfat_entry_info_t *source,
                                  uint32_t target_cluster) {
  if (!source->data_length || source->data_length % mnt->bytes_per_cluster ||
      source->data_length > UINT32_MAX ||
      !exfat_is_valid_cluster(mnt, source->first_cluster))
    return -EIO;
  if (source->first_cluster == target_cluster)
    return -EINVAL;

  size_t cap = 32;
  size_t head = 0, tail = 0;
  exfat_dir_visit_t *queue = kmalloc(cap * sizeof(*queue));
  if (!queue)
    return -ENOMEM;
  queue[tail++] = (exfat_dir_visit_t){
      source->first_cluster, source->data_length, source->contig};

  int rc = 0;
  while (head < tail && !rc) {
    if (tail > mnt->cluster_count) {
      rc = -EIO;
      break;
    }
    exfat_dir_visit_t dir = queue[head++];
    exfat_dir_cursor_t cur;
    if (exfat_cursor_init(&cur, mnt, dir.cluster, dir.contig) != 0) {
      rc = -EIO;
      break;
    }
    cur.clusters_left = (uint32_t)(dir.data_length / mnt->bytes_per_cluster - 1);
    cur.bounded = true;
    exfat_entry_info_t child;
    while (exfat_cursor_next_item(&cur, &child)) {
      if (!(child.attributes & EXFAT_ATTR_DIR))
        continue;
      if (child.first_cluster == target_cluster) {
        rc = -EINVAL;
        break;
      }
      if (!child.data_length || child.data_length % mnt->bytes_per_cluster ||
          child.data_length > UINT32_MAX ||
          !exfat_is_valid_cluster(mnt, child.first_cluster)) {
        rc = -EIO;
        break;
      }
      if (tail == cap) {
        if (cap >= mnt->cluster_count) {
          rc = -EIO;
          break;
        }
        size_t new_cap = cap * 2;
        if (new_cap > mnt->cluster_count + 1)
          new_cap = mnt->cluster_count + 1;
        exfat_dir_visit_t *grown = kmalloc(new_cap * sizeof(*grown));
        if (!grown) {
          rc = -ENOMEM;
          break;
        }
        memcpy(grown, queue, tail * sizeof(*queue));
        kfree(queue);
        queue = grown;
        cap = new_cap;
      }
      queue[tail++] = (exfat_dir_visit_t){
          child.first_cluster, child.data_length, child.contig};
    }
    if (!rc && cur.error)
      rc = cur.error;
    exfat_cursor_free(&cur);
  }
  kfree(queue);
  return rc;
}

static int exfat_check_move(exfat_mount_t *mnt, const exfat_entry_info_t *source,
                            uint32_t parent) {
  int rc = 0;
  bool need_disk_scan = false;
  spinlock_acquire(&mnt->nodes_lock);
  uint32_t depth = 0;
  uint32_t cur = parent;
  while (cur != mnt->root_cluster) {
    if (cur == source->first_cluster) {
      rc = -EINVAL;
      break;
    }
    exfat_node_t *node = mnt->nodes;
    while (node && (node->node.impl != cur || node->unlinked ||
                    (node->node.flags & FS_TYPE_MASK) != FS_DIRECTORY))
      node = node->next;
    if (!node) {
      need_disk_scan = true;
      break;
    }
    if (++depth > mnt->cluster_count) {
      rc = -EIO;
      break;
    }
    cur = node->parent_cluster;
  }
  spinlock_release(&mnt->nodes_lock);
  if (!rc && need_disk_scan)
    return exfat_subtree_contains(mnt, source, parent);
  return rc;
}

int exfat_rename_cross(vfs_node_t *old_dir, char *old_name,
                       vfs_node_t *new_dir, char *new_name) {
  exfat_mount_t *mnt = old_dir->device;
  if (new_dir->device != mnt)
    return -EXDEV;
  if (strlen(new_name) >= sizeof(new_dir->name))
    return -ENAMETOOLONG;
  uint16_t utf16[255];
  int length = exfat_utf8_to_utf16(new_name, utf16, true);
  if (length < 0)
    return length;
  int rc = exfat_begin_write(mnt);
  if (rc)
    return rc;
  exfat_transaction_t tx = {.mnt = mnt};
  exfat_entry_info_t source, target;
  exfat_node_t *src_live = NULL, *dst_live = NULL;
  exfat_entry_location_t location;
  exfat_stream_t grown = {0};
  uint8_t set[EXFAT_MAX_ENTRIES][EXFAT_ENTRY_SIZE];
  rc = mnt->write_failed ? -EROFS : exfat_lookup_info(old_dir, old_name, &source);
  if (rc)
    goto out;
  if ((source.attributes & EXFAT_ATTR_DIR) &&
      (rc = exfat_check_move(mnt, &source, new_dir->impl)))
    goto out;
  rc = exfat_lookup_info(new_dir, new_name, &target);
  if (!rc && target.location.slots[0] != source.location.slots[0]) {
    if ((source.attributes ^ target.attributes) & EXFAT_ATTR_DIR) {
      rc = (source.attributes & EXFAT_ATTR_DIR) ? -ENOTDIR : -EISDIR;
      goto out;
    }
    if ((target.attributes & EXFAT_ATTR_DIR) && (rc = exfat_empty_directory(mnt, &target)))
      goto out;
    dst_live = exfat_live_ref(mnt, target.location.slots[0]);
    rc = exfat_remove_set(&tx, &target, dst_live);
    if (rc)
      goto out;
  } else if (rc && rc != -ENOENT) {
    goto out;
  }
  rc = exfat_load_set(&tx, &source.location, set);
  if (!rc)
    rc = exfat_clear_set(&tx, &source);
  if (rc)
    goto out;
  unsigned old_names = (set[1][3] + 14) / 15;
  unsigned extra = source.location.count - 2 - old_names;
  unsigned names = (length + 14) / 15;
  if (2 + names + extra > EXFAT_MAX_ENTRIES) {
    rc = -ENAMETOOLONG;
    goto out;
  }
  if (extra)
    memmove(set[2 + names], set[2 + old_names], extra * EXFAT_ENTRY_SIZE);
  exfat_set_name(mnt, set, utf16, (unsigned)length);
  set[0][1] += extra;
  rc = exfat_find_slots(&tx, new_dir, 2 + names + extra, &location, &grown);
  if (!rc && grown.length)
    rc = exfat_update_stream(&tx, (exfat_node_t *)new_dir, &grown,
                              (uint32_t)rtc_get_timestamp());
  if (!rc)
    rc = exfat_store_set(&tx, &location, set);
  if (!rc)
    rc = exfat_tx_commit(&tx);
  if (!rc) {
    if (grown.length)
      exfat_apply_stream((exfat_node_t *)new_dir, &grown);
    if (dst_live)
      exfat_mark_unlinked(mnt, dst_live);
    src_live = exfat_live_ref(mnt, source.location.slots[0]);
    if (src_live) {
      spinlock_acquire(&mnt->nodes_lock);
      src_live->location = location;
      src_live->parent_cluster = new_dir->impl;
      memcpy(src_live->node.name, new_name, strlen(new_name) + 1);
      spinlock_release(&mnt->nodes_lock);
    }
  }
out:
  exfat_tx_discard(&tx);
  exfat_end_write(mnt);
  if (src_live)
    vfs_close(&src_live->node);
  if (dst_live) {
    if (!rc)
      vfs_node_retire(&dst_live->node);
    vfs_close(&dst_live->node);
  }
  return rc;
}

int exfat_rename_impl(vfs_node_t *dir, char *old_name, char *new_name) {
  return exfat_rename_cross(dir, old_name, dir, new_name);
}

static uint64_t exfat_mmap_overwrite(vfs_node_t *node, uint64_t address,
                                    uint64_t length, uint64_t prot,
                                    uint64_t flags, uint64_t offset) {
  (void)node;
  (void)address;
  (void)length;
  (void)prot;
  (void)flags;
  (void)offset;
  return (uint64_t)-EOPNOTSUPP;
}

static uint32_t exfat_read_locked(vfs_node_t *node, uint64_t offset,
                                   uint32_t size, uint8_t *buffer) {
  exfat_mount_t *mnt = node->device;
  if (!mnt->writable)
    return exfat_read_impl(node, offset, size, buffer);
  int rc = exfat_begin_write(mnt);
  if (rc)
    return (uint32_t)rc;
  uint32_t result = exfat_read_impl(node, offset, size, buffer);
  exfat_end_write(mnt);
  return result;
}

static uint32_t exfat_read_legacy(vfs_node_t *node, uint32_t offset,
                                  uint32_t size, uint8_t *buffer) {
  return exfat_read_locked(node, offset, size, buffer);
}

static uint32_t exfat_write_legacy(vfs_node_t *node, uint32_t offset,
                                   uint32_t size, uint8_t *buffer) {
  return exfat_write_file(node, offset, size, buffer, NULL, true);
}

static uint64_t exfat_size(vfs_node_t *node) {
  return __atomic_load_n(&((exfat_node_t *)node)->data_length, __ATOMIC_ACQUIRE);
}

static const struct vfs_io64 exfat_read_ops = {
    .size = exfat_size, .read = exfat_read_locked,
};
static const struct vfs_io64 exfat_overwrite_ops = {
    .size = exfat_size, .read = exfat_read_locked, .write = exfat_write_impl,
};
static const struct vfs_io64 exfat_write_ops = {
    .size = exfat_size, .read = exfat_read_locked, .write = exfat_write_impl,
    .append = exfat_append_impl, .truncate = exfat_truncate_impl,
};

static void exfat_copy_utf8_name(char *dst, size_t cap, const char *src) {
  size_t len = strlen(src);
  if (len >= cap) {
    len = cap - 1;
    while (len > 0 && (((const uint8_t *)src)[len] & 0xC0u) == 0x80u)
      len--;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static uint64_t exfat_file_capacity(const exfat_mount_t *mnt,
                                    uint32_t first_cluster, bool contig) {
  uint32_t available = contig && first_cluster >= 2 &&
                               first_cluster <= mnt->cluster_count + 1
                           ? mnt->cluster_count + 2 - first_cluster
                           : mnt->cluster_count;
  return (uint64_t)available * mnt->bytes_per_cluster;
}

static int exfat_fallocate_impl(vfs_node_t *node, int mode, uint32_t offset,
                                uint32_t len) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE || !node->device ||
      !len || len > UINT32_MAX - offset)
    return -EINVAL;
  if (mode != 0)
    return -EOPNOTSUPP;
  exfat_mount_t *mnt = node->device;
  if (!mnt->writable)
    return -EROFS;
  if (mnt->overwrite_only)
    return -EOPNOTSUPP;

  exfat_node_t *entry = (exfat_node_t *)node;
  int rc = exfat_begin_write(mnt);
  if (rc)
    return rc;
  exfat_transaction_t tx = {.mnt = mnt};
  exfat_stream_t stream = exfat_node_stream(entry);
  uint64_t old_length = stream.length;
  uint64_t end = (uint64_t)offset + len;
  rc = mnt->write_failed ? -EROFS : exfat_validate_stream(&tx, entry);
  if (!rc && end > stream.length) {
    uint32_t now = (uint32_t)rtc_get_timestamp();
    rc = exfat_resize_stream(&tx, &stream, end);
    if (!rc)
      rc = exfat_update_stream(&tx, entry, &stream, now);
    if (!rc)
      rc = exfat_tx_commit(&tx);
    if (!rc) {
      exfat_apply_stream(entry, &stream);
      node->mtime = node->ctime = now;
      if (old_length <= UINT32_MAX && stream.length > UINT32_MAX)
        vfs_cache_invalidate(node, 0xFFFFF000u);
    }
  }
  exfat_tx_discard(&tx);
  exfat_end_write(mnt);
  return rc;
}

int exfat_chmod_impl(vfs_node_t *node, uint16_t permission) {
  if (!node || !node->device)
    return -EINVAL;
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt->writable || mnt->write_failed)
    return -EROFS;
  if (mnt->overwrite_only)
    return -EOPNOTSUPP;
  if (exfat_is_root(mnt, node)) {
    node->mask = (permission & 0222) ? 0755 : 0555;
    return 0;
  }

  exfat_node_t *entry = (exfat_node_t *)node;
  bool want_write = (permission & 0222) != 0;
  if (!want_write && (node->flags & FS_TYPE_MASK) == FS_FILE)
    vfs_cache_sync(node);

  int rc = exfat_begin_write(mnt);
  if (rc)
    return rc;
  exfat_transaction_t tx = {.mnt = mnt};
  uint8_t set[EXFAT_MAX_ENTRIES][EXFAT_ENTRY_SIZE];
  uint16_t attrs = 0;
  rc = mnt->write_failed ? -EROFS : 0;
  if (!rc && !entry->unlinked) {
    rc = exfat_load_set(&tx, &entry->location, set);
    if (!rc && (entry->location.count < 3 || set[0][0] != EXFAT_TYPE_FILE))
      rc = -EIO;
    if (!rc) {
      attrs = exfat_le16(set[0] + 4);
      if (want_write)
        attrs &= (uint16_t)~EXFAT_ATTR_READONLY;
      else
        attrs |= EXFAT_ATTR_READONLY;
      exfat_store16(set[0] + 4, attrs);
      rc = exfat_store_set(&tx, &entry->location, set);
    }
    if (!rc)
      rc = exfat_tx_commit(&tx);
  }
  if (!rc) {
    node->mask = want_write ? 0755 : 0555;
    if ((node->flags & FS_TYPE_MASK) == FS_FILE) {
      uint64_t capacity = exfat_file_capacity(
          mnt, node->impl, (node->flags & FS_EXFAT_CONTIG) != 0);
      if (want_write && entry->data_length <= capacity) {
        node->flags &= ~FS_READONLY;
        node->write = exfat_write_legacy;
        node->io64 = &exfat_write_ops;
        node->fallocate = exfat_fallocate_impl;
      } else {
        node->flags |= FS_READONLY;
        node->write = NULL;
        node->io64 = &exfat_read_ops;
        node->fallocate = NULL;
      }
    }
  }
  exfat_tx_discard(&tx);
  exfat_end_write(mnt);
  return rc;
}

int exfat_utime_impl(vfs_node_t *node, uint32_t atime, uint32_t mtime) {
  if (!node || !node->device)
    return -EINVAL;
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt->writable || mnt->write_failed)
    return -EROFS;
  if (mnt->overwrite_only)
    return -EOPNOTSUPP;
  if (exfat_is_root(mnt, node)) {
    node->atime = atime;
    node->mtime = mtime;
    return 0;
  }

  exfat_node_t *entry = (exfat_node_t *)node;
  int rc = exfat_begin_write(mnt);
  if (rc)
    return rc;
  exfat_transaction_t tx = {.mnt = mnt};
  uint8_t set[EXFAT_MAX_ENTRIES][EXFAT_ENTRY_SIZE];
  rc = mnt->write_failed ? -EROFS : 0;
  if (!rc && !entry->unlinked) {
    rc = exfat_load_set(&tx, &entry->location, set);
    if (!rc && (entry->location.count < 3 || set[0][0] != EXFAT_TYPE_FILE))
      rc = -EIO;
    if (!rc) {
      exfat_store32(set[0] + 12, exfat_disk_time(mtime));
      set[0][21] = exfat_disk_cs(mtime);
      set[0][23] = 0x80; /* UTC */
      exfat_store32(set[0] + 16, exfat_disk_time(atime));
      set[0][24] = 0x80; /* UTC */
      rc = exfat_store_set(&tx, &entry->location, set);
    }
    if (!rc)
      rc = exfat_tx_commit(&tx);
  }
  if (!rc) {
    node->atime = atime;
    node->mtime = mtime;
  }
  exfat_tx_discard(&tx);
  exfat_end_write(mnt);
  return rc;
}

static void exfat_set_dir_ops(vfs_node_t *node, exfat_mount_t *mnt) {
  node->readdir = exfat_readdir_impl;
  node->finddir = exfat_finddir_impl;
  node->mask = 0555;
  if (mnt->writable && !mnt->overwrite_only) {
    node->flags |= FS_DENTRY_NOCACHE;
    node->mask = 0755;
    node->create = exfat_create_impl;
    node->mkdir = exfat_mkdir_impl;
    node->unlink = exfat_unlink_impl;
    node->rmdir = exfat_rmdir_impl;
    node->rename = exfat_rename_impl;
    node->chmod = exfat_chmod_impl;
  }
}

static uint32_t exfat_inode(const exfat_entry_info_t *info) {
  uint64_t slot = info->location.slots[0] / EXFAT_ENTRY_SIZE;
  return (uint32_t)(slot ^ (slot >> 32)) | 0x80000000u;
}

static vfs_node_t *exfat_make_vfs_node(vfs_node_t *parent,
                                       const exfat_entry_info_t *info) {
  exfat_mount_t *mnt = parent->device;
  bool full = mnt->writable && !mnt->overwrite_only;
  exfat_node_t *entry = full ? exfat_live_ref(mnt, info->location.slots[0]) : NULL;
  if (entry)
    return &entry->node;
  entry = kcalloc(1, sizeof(*entry));
  if (!entry)
    return NULL;
  vfs_node_t *node = &entry->node;
  vfs_node_init(node);
  entry->location = info->location;
  entry->parent_cluster = parent->impl;

  exfat_copy_utf8_name(node->name, sizeof(node->name), info->name);

  node->impl = info->first_cluster;
  node->inode = exfat_inode(info);
  node->length = (info->data_length > UINT32_MAX) ? UINT32_MAX
                                                  : (uint32_t)info->data_length;
  entry->data_length = info->data_length;
  entry->valid_data_length = info->valid_data_length;
  node->device = mnt;
  node->atime = info->atime;
  node->mtime = info->mtime;
  node->ctime = info->ctime;

  uint32_t contig_flag = info->contig ? FS_EXFAT_CONTIG : 0;
  node->mask = 0555;
  if (info->attributes & EXFAT_ATTR_DIR) {
    node->flags = FS_DIRECTORY | FS_AUTODESTROY | contig_flag;
    if (!mnt->writable || !info->checksum_ok || info->data_length > UINT32_MAX)
      node->flags |= FS_READONLY;
    exfat_set_dir_ops(node, mnt);
    if (full && !info->checksum_ok)
      node->chmod = NULL;
    else if (full && (info->attributes & EXFAT_ATTR_READONLY))
      node->mask = 0555;
  } else {
    node->flags = FS_FILE | FS_AUTODESTROY | contig_flag;
    node->read = exfat_read_legacy;
    node->io64 = &exfat_read_ops;
    uint64_t capacity = exfat_file_capacity(mnt, info->first_cluster, info->contig);
    if (mnt->writable && info->checksum_ok &&
        !(info->attributes & EXFAT_ATTR_READONLY) &&
        (full || info->valid_data_length) && info->data_length <= capacity) {
      node->mask = 0755;
      node->write = exfat_write_legacy;
      node->io64 = full ? &exfat_write_ops : &exfat_overwrite_ops;
      if (full) {
        node->flags |= FS_PAGE_CACHE;
        node->fallocate = exfat_fallocate_impl;
      } else {
        node->mmap = exfat_mmap_overwrite;
      }
    } else {
      node->flags |= FS_PAGE_CACHE | FS_READONLY;
    }
    if (full && info->checksum_ok && info->data_length <= capacity)
      node->chmod = exfat_chmod_impl;
  }
  node->statfs = exfat_statfs_impl;
  node->destroy = exfat_node_destroy;
  __atomic_add_fetch(&mnt->refs, 1, __ATOMIC_ACQ_REL);
  if (full) {
    node->flags |= FS_DENTRY_NOCACHE;
    spinlock_acquire(&mnt->nodes_lock);
    entry->next = mnt->nodes;
    mnt->nodes = entry;
    spinlock_release(&mnt->nodes_lock);
  }
  return node;
}

static struct dirent *exfat_readdir_impl(vfs_node_t *node, uint64_t index) {
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt)
    return NULL;

  bool locked = mnt->writable && !mnt->overwrite_only;
  if (locked && exfat_begin_write(mnt))
    return NULL;
  exfat_dir_cursor_t cur;
  if (exfat_dir_init(&cur, node) != 0) {
    if (locked)
      exfat_end_write(mnt);
    return NULL;
  }

  struct dirent *ret = NULL;
  struct dirent *dre = vfs_readdir_buffer();
  memset(dre, 0, sizeof(*dre));

  exfat_entry_info_t info;
  uint64_t idx = 0;
  while (exfat_cursor_next_item(&cur, &info)) {
    if (idx == index) {
      exfat_copy_utf8_name(dre->name, sizeof(dre->name), info.name);
      dre->ino = exfat_inode(&info);
      dre->d_type = (info.attributes & EXFAT_ATTR_DIR) ? DT_DIR : DT_REG;
      ret = dre;
      break;
    }
    idx++;
  }

  exfat_cursor_free(&cur);
  if (locked)
    exfat_end_write(mnt);
  return ret;
}

static vfs_node_t *exfat_finddir_impl(vfs_node_t *node, char *name) {
  exfat_mount_t *mnt = (exfat_mount_t *)node->device;
  if (!mnt || !name)
    return NULL;

  bool locked = mnt->writable && !mnt->overwrite_only;
  if (locked && exfat_begin_write(mnt))
    return NULL;

  vfs_node_t *result = NULL;
  exfat_entry_info_t info;
  if (exfat_lookup_info(node, name, &info) == 0)
    result = exfat_make_vfs_node(node, &info);
  if (locked)
    exfat_end_write(mnt);
  return result;
}

int exfat_statfs_impl(vfs_node_t *node, struct statfs_buf *buf) {
  exfat_mount_t *mnt = node ? (exfat_mount_t *)node->device : NULL;
  if (!mnt || !buf)
    return -1;

  bool locked = mnt->writable && !mnt->overwrite_only;
  if (locked && exfat_begin_write(mnt))
    return -EINTR;
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
  buf->f_namelen = sizeof(node->name) - 1;
  buf->f_frsize = (int64_t)mnt->bytes_per_cluster;
  buf->f_flags = mnt->writable && !mnt->write_failed ? 0 : 1; /* ST_RDONLY */
  if (locked)
    exfat_end_write(mnt);
  return 0;
}

static int exfat_load_bitmap_chain(exfat_mount_t *mnt) {
  uint32_t bytes = (uint32_t)(((uint64_t)mnt->cluster_count + 7) / 8);
  if (mnt->bitmap_length < bytes || mnt->bitmap_length > UINT32_MAX)
    return -EIO;
  uint32_t count = (bytes - 1) / mnt->bytes_per_cluster + 1;
  mnt->bitmap_chain = kmalloc_fallback((size_t)count * sizeof(*mnt->bitmap_chain));
  if (!mnt->bitmap_chain)
    return -ENOMEM;
  exfat_transaction_t tx = {.mnt = mnt};
  uint32_t cluster = mnt->bitmap_cluster;
  for (uint32_t i = 0; i < count; i++) {
    mnt->bitmap_chain[i] = cluster;
    cluster = exfat_tx_fat(&tx, cluster, false, 0);
  }
  int rc = tx.error ? tx.error : (cluster == EXFAT_EOC ? 0 : -EIO);
  exfat_tx_discard(&tx);
  return rc;
}

static int exfat_load_upcase(exfat_mount_t *mnt, uint32_t cluster,
                             uint64_t length, uint32_t expected) {
  if (!length || length > 65536 * 2 || length % 2)
    return -EIO;
  uint32_t size = mnt->bytes_per_sector;
  uint8_t *sector = kmalloc(size);
  uint16_t *table = kmalloc_fallback(65536 * sizeof(*table));
  if (!sector || !table) {
    kfree(sector);
    kfree(table);
    return -ENOMEM;
  }
  uint32_t checksum = 0, code = 0;
  bool run = false;
  int rc = -EIO;
  uint32_t pos = 0;
  while (pos < length && exfat_is_valid_cluster(mnt, cluster)) {
    uint64_t lba = exfat_cluster_lba(mnt, cluster);
    for (uint32_t s = 0; s < mnt->sectors_per_cluster && pos < length; s++) {
      if (exfat_read_vol_sectors(mnt, lba + s, 1, sector) != 0)
        goto out;
      uint32_t count = length - pos < size ? (uint32_t)(length - pos) : size;
      for (uint32_t i = 0; i < count; i++)
        checksum = (checksum >> 1) + ((checksum & 1u) << 31) + sector[i];
      for (uint32_t i = 0; i < count; i += 2) {
        uint16_t value = exfat_le16(sector + i);
        if (code >= 65536)
          goto out;
        if (run) {
          if (!value || value > 65536 - code)
            goto out;
          for (uint32_t end = code + value; code < end; code++)
            table[code] = (uint16_t)code;
          run = false;
        } else if (value == 0xffff && code != 0xffff) {
          run = true;
        } else {
          table[code++] = value;
        }
      }
      pos += count;
    }
    if (pos < length)
      cluster = exfat_next_cluster(mnt, cluster, false);
  }
  if (pos == length && code == 65536 && !run && checksum == expected) {
    mnt->upcase = table;
    table = NULL;
    rc = 0;
  }
out:
  kfree(sector);
  kfree(table);
  return rc;
}

static int exfat_verify_boot(exfat_mount_t *mnt) {
  uint8_t *sector = kmalloc(mnt->bytes_per_sector);
  if (!sector)
    return -ENOMEM;
  uint32_t checksum = 0;
  int rc = -EIO;
  for (uint32_t lba = 0; lba < 11; lba++) {
    if (exfat_read_vol_sectors(mnt, lba, 1, sector) != 0)
      goto out;
    for (uint32_t i = 0; i < mnt->bytes_per_sector; i++) {
      if (!lba && (i == 106 || i == 107 || i == 112))
        continue;
      checksum = (checksum >> 1) + ((checksum & 1u) << 31) + sector[i];
    }
  }
  if (exfat_read_vol_sectors(mnt, 11, 1, sector) != 0)
    goto out;
  for (uint32_t i = 0; i < mnt->bytes_per_sector; i += 4) {
    if (exfat_le32(sector + i) != checksum)
      goto out;
  }
  rc = 0;
out:
  kfree(sector);
  return rc;
}

int exfat_mount(struct block_device *dev, vfs_node_t *mountpoint,
                bool read_only, bool overwrite_only) {
  if (!dev || !dev->read_sectors || !mountpoint)
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
  uint16_t volume_flags = exfat_le16(boot + 106);
  bool writable = !read_only && !dev->read_only && dev->write_sectors;
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

  if (cluster_count == 0 || cluster_count > 0xfffffff5u || root_cluster < 2 ||
      root_cluster >= cluster_count + 2 ||
      (uint64_t)cluster_heap_offset +
              (uint64_t)cluster_count * sectors_per_cluster >
          volume_length) {
    kfree(boot);
    return -1;
  }
  if (writable && (bytes_per_sector % dev_ss || num_fats != 1 || volume_flags ||
      volume_length > dev->total_sectors / (bytes_per_sector / dev_ss) || fat_offset < 24 ||
      (uint64_t)fat_offset + fat_length > cluster_heap_offset ||
      (uint64_t)fat_length * bytes_per_sector < ((uint64_t)cluster_count + 2) * 4)) {
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
  mnt->fat_offset = fat_offset + ((num_fats == 2 && (volume_flags & 1)) ? fat_length : 0);
  mnt->fat_length = fat_length;
  mnt->cluster_heap_offset = cluster_heap_offset;
  mnt->cluster_count = cluster_count;
  mnt->root_cluster = root_cluster;
  mnt->volume_length = volume_length;
  mnt->writable = writable;
  mnt->overwrite_only = overwrite_only;
  mnt->alloc_hint = 2;
  spinlock_init(&mnt->write_lock);
  spinlock_init(&mnt->nodes_lock);
  wait_queue_init(&mnt->write_waiters);

  vfs_node_t *root_vfs = kmalloc(sizeof(vfs_node_t));
  if (!root_vfs) {
    kfree(mnt);
    return -1;
  }
  vfs_node_init(root_vfs);
  memcpy(root_vfs->name, "mnt", 4);
  root_vfs->flags = FS_DIRECTORY | (writable ? 0 : FS_READONLY);
  root_vfs->impl = mnt->root_cluster;
  root_vfs->inode = mnt->root_cluster;
  root_vfs->device = mnt;
  exfat_set_dir_ops(root_vfs, mnt);
  root_vfs->statfs = exfat_statfs_impl;

  mnt->root_node = root_vfs;
  mnt->dev = block_ref(dev);
  mnt->refs = 1;

  spinlock_acquire(&exfat_mount_lock);
  for (exfat_mount_t *other = exfat_mounts; other; other = other->next) {
    if (other->dev == dev && (writable || other->writable)) {
      spinlock_release(&exfat_mount_lock);
      kfree(root_vfs);
      block_put(mnt->dev);
      kfree(mnt);
      return -1;
    }
  }
  mnt->next = exfat_mounts;
  exfat_mounts = mnt;
  spinlock_release(&exfat_mount_lock);

  if (writable && exfat_verify_boot(mnt)) {
    exfat_mount_put(mnt);
    return -1;
  }

  uint32_t upcase_cluster = 0, upcase_checksum = 0;
  uint64_t upcase_length = 0;
  exfat_dir_cursor_t bcur;
  if (exfat_cursor_init(&bcur, mnt, mnt->root_cluster, false) == 0) {
    const uint8_t *be;
    while ((be = exfat_cursor_raw_next(&bcur)) != NULL) {
      if (be[0] == EXFAT_TYPE_BITMAP && (be[1] & 1) == (volume_flags & 1)) {
        mnt->bitmap_cluster = exfat_le32(be + 20);
        mnt->bitmap_length = exfat_le64(be + 24);
      } else if (be[0] == EXFAT_TYPE_UPCASE) {
        upcase_cluster = exfat_le32(be + 20);
        upcase_length = exfat_le64(be + 24);
        upcase_checksum = exfat_le32(be + 4);
      }
      if (mnt->bitmap_cluster && upcase_cluster)
        break;
    }
    exfat_cursor_free(&bcur);
  }
  int upcase_rc = exfat_load_upcase(mnt, upcase_cluster, upcase_length, upcase_checksum);
  if (writable && ((!overwrite_only && upcase_rc) || exfat_load_bitmap_chain(mnt))) {
    exfat_mount_put(mnt);
    return -1;
  }

  mountpoint->flags = root_vfs->flags;
  mountpoint->inode = mnt->root_cluster;
  mountpoint->impl = mnt->root_cluster;
  mountpoint->device = mnt;
  mountpoint->mask = root_vfs->mask;
  mountpoint->io64 = NULL;
  mountpoint->read = NULL;
  mountpoint->write = NULL;
  mountpoint->append = NULL;
  mountpoint->mknod = NULL;
  mountpoint->readlink = NULL;
  mountpoint->symlink = NULL;
  mountpoint->link = NULL;
  mountpoint->rename = root_vfs->rename;
  mountpoint->chmod = root_vfs->chmod;
  mountpoint->chown = NULL;
  mountpoint->truncate = NULL;
  mountpoint->mmap = NULL;
  mountpoint->fallocate = NULL;
  mountpoint->get_nlink = NULL;
  mountpoint->create = root_vfs->create;
  mountpoint->mkdir = root_vfs->mkdir;
  mountpoint->unlink = root_vfs->unlink;
  mountpoint->rmdir = root_vfs->rmdir;
  mountpoint->readdir = exfat_readdir_impl;
  mountpoint->finddir = exfat_finddir_impl;
  mountpoint->statfs = exfat_statfs_impl;

  klog_puts(!writable ? "[OK] exFAT filesystem mounted (read-only).\n"
      : (overwrite_only ? "[OK] exFAT filesystem mounted (overwrite).\n"
                        : "[OK] exFAT filesystem mounted (read-write).\n"));
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
