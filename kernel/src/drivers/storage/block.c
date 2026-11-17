#include "drivers/storage/block.h"
#include "drivers/storage/gpt.h"
#include "drivers/storage/mbr.h"
#include <stddef.h>

#include "fs/devfs.h"
#include "fs/sysfs.h"
#include "lock/spinlock.h"

/* registered[] is compacted on removal.  next_minor only grows, so a
 * replugged disk never reuses the minor of a live device. */
static struct block_device *registered[BLOCK_MAX_DEVICES];
static int num_devices = 0;
static uint32_t next_minor = 0;
static spinlock_t block_table_lock = SPINLOCK_INIT;

#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/vmm.h"

static uint32_t block_vfs_read(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  struct block_device *dev = (struct block_device *)node->device;
  if (!dev || !dev->read_sectors)
    return 0;

  uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
  uint32_t sector = offset / sector_size;
  uint32_t count = size / sector_size;

  if (size % sector_size != 0)
    return 0; // Enforce sector aligned logical reads for now

  int err = dev->read_sectors(dev, sector, count, buffer);
  if (err)
    return 0;
  return size;
}

// Raw block-device writes.  Like reads, the request must be sector aligned;
// the driver is responsible for bouncing user or otherwise unaligned buffers.
static uint32_t block_vfs_write(struct vfs_node *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  struct block_device *dev = (struct block_device *)node->device;
  if (!dev || !dev->write_sectors)
    return 0;

  uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
  uint32_t sector = offset / sector_size;
  uint32_t count = size / sector_size;

  if (size % sector_size != 0)
    return 0;

  int err = dev->write_sectors(dev, sector, count, buffer);
  if (err)
    return 0;
  return size;
}

// Partition Wrapper

struct partition_wrapper {
  struct block_device *parent;
  uint64_t start_lba;
};

static struct block_device *partition_parent(const struct block_device *dev) {
  const struct partition_wrapper *wrap =
      (const struct partition_wrapper *)dev->driver_data;
  return wrap ? wrap->parent : NULL;
}

/* A partition is unusable once it or its disk is being removed. */
static bool partition_gone(const struct block_device *dev) {
  struct block_device *parent = partition_parent(dev);
  return block_device_gone(dev) || (parent && block_device_gone(parent));
}

/* Batch members must each get a verdict, even when the whole batch fails. */
static void block_batch_fail(struct block_io_req *reqs, unsigned n) {
  for (unsigned i = 0; i < n; i++)
    reqs[i].result = -1;
}

static int partition_read(struct block_device *dev, uint64_t lba,
                          uint32_t count, void *buf) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (partition_gone(dev) || !wrap || !wrap->parent || !buf || count == 0 ||
      lba >= dev->total_sectors ||
      (uint64_t)count > dev->total_sectors - lba)
    return -1;
  int ret = wrap->parent->read_sectors(wrap->parent, lba + wrap->start_lba,
                                       count, buf);
  if (ret == 0)
    block_stat_read(dev, count);
  return ret;
}

static int partition_write(struct block_device *dev, uint64_t lba,
                           uint32_t count, const void *buf) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (partition_gone(dev) || !wrap || !wrap->parent || !wrap->parent->write_sectors || !buf ||
      count == 0 || lba >= dev->total_sectors ||
      (uint64_t)count > dev->total_sectors - lba)
    return -1;
  int ret = wrap->parent->write_sectors(wrap->parent, lba + wrap->start_lba,
                                        count, buf);
  if (ret == 0)
    block_stat_write(dev, count);
  return ret;
}

static int partition_write_fua(struct block_device *dev, uint64_t lba,
                               uint32_t count, const void *buf) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (partition_gone(dev) || !wrap || !wrap->parent || !wrap->parent->write_sectors || !buf ||
      count == 0 || lba >= dev->total_sectors ||
      (uint64_t)count > dev->total_sectors - lba)
    return -1;
  uint64_t parent_lba = lba + wrap->start_lba;
  struct block_device *parent = wrap->parent;
  int ret;
  if (parent->write_sectors_fua)
    ret = parent->write_sectors_fua(parent, parent_lba, count, buf);
  else {
    if (parent->write_sectors(parent, parent_lba, count, buf) != 0)
      return -1;
    ret = block_flush(parent);
  }
  if (ret == 0)
    block_stat_write(dev, count);
  return ret;
}

static int partition_flush(struct block_device *dev) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (partition_gone(dev) || !wrap || !wrap->parent)
    return -1;
  return block_flush(wrap->parent);
}

/* Pipelined partition forwarding.  Each valid member's LBA moves by the
 * partition start and then the run goes through the parent's batch op (or its
 * single-request fallback).  Invalid members fail on their own and split the
 * forwarded runs, so the parent never sees a request outside the partition. */
static bool partition_batch_member_valid(const struct block_device *dev,
                                         const struct block_io_req *req) {
  return req->buf && req->count != 0 && req->lba < dev->total_sectors &&
         (uint64_t)req->count <= dev->total_sectors - req->lba;
}

static int partition_read_batch(struct block_device *dev,
                                struct block_io_req *reqs, unsigned n) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (!reqs || n == 0)
    return -1;
  if (partition_gone(dev) || !wrap || !wrap->parent ||
      !wrap->parent->read_sectors) {
    block_batch_fail(reqs, n);
    return -1;
  }
  int worst = 0;
  unsigned i = 0;
  while (i < n) {
    if (!partition_batch_member_valid(dev, &reqs[i])) {
      reqs[i].result = -1;
      if (worst > -1)
        worst = -1;
      i++;
      continue;
    }
    unsigned j = i;
    while (j < n && partition_batch_member_valid(dev, &reqs[j])) {
      reqs[j].lba += wrap->start_lba;
      j++;
    }
    int rc = block_read_batch(wrap->parent, &reqs[i], j - i);
    for (unsigned k = i; k < j; k++)
      if (reqs[k].result == 0)
        block_stat_read(dev, reqs[k].count);
    if (rc < worst)
      worst = rc;
    i = j;
  }
  return worst;
}

static int partition_write_batch(struct block_device *dev,
                                 struct block_io_req *reqs, unsigned n) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (!reqs || n == 0)
    return -1;
  if (partition_gone(dev) || !wrap || !wrap->parent ||
      !wrap->parent->write_sectors) {
    block_batch_fail(reqs, n);
    return -1;
  }
  int worst = 0;
  unsigned i = 0;
  while (i < n) {
    if (!partition_batch_member_valid(dev, &reqs[i])) {
      reqs[i].result = -1;
      if (worst > -1)
        worst = -1;
      i++;
      continue;
    }
    unsigned j = i;
    while (j < n && partition_batch_member_valid(dev, &reqs[j])) {
      reqs[j].lba += wrap->start_lba;
      j++;
    }
    int rc = block_write_batch(wrap->parent, &reqs[i], j - i);
    for (unsigned k = i; k < j; k++)
      if (reqs[k].result == 0)
        block_stat_write(dev, reqs[k].count);
    if (rc < worst)
      worst = rc;
    i = j;
  }
  return worst;
}

/* NVMe namespace names ("nvme0n1") take a "p" before the partition index so
 * nvme0n1p2 cannot be confused with namespace 12; other disks keep the
 * historical "sda1" spelling. */
static bool block_name_is_nvme_ns(const char *name) {
  if (strncmp(name, "nvme", 4) != 0)
    return false;
  const char *p = name + 4;
  if (*p < '0' || *p > '9')
    return false;
  while (*p >= '0' && *p <= '9')
    p++;
  if (*p != 'n')
    return false;
  p++;
  if (*p < '0' || *p > '9')
    return false;
  while (*p >= '0' && *p <= '9')
    p++;
  return *p == '\0';
}

int block_add_partition_ex(struct block_device *parent, int part_index,
                           uint64_t start_lba, uint64_t total_sectors,
                           const struct partition_meta *meta) {
  if (!parent || part_index <= 0 || total_sectors == 0)
    return -1;

  if (start_lba >= parent->total_sectors ||
      total_sectors > parent->total_sectors - start_lba)
    return -1;

  struct partition_wrapper *wrap = kmalloc(sizeof(struct partition_wrapper));
  if (!wrap)
    return -1;

  wrap->parent = parent;
  wrap->start_lba = start_lba;

  struct block_device *pdev = kmalloc(sizeof(struct block_device));
  if (!pdev) {
    kfree(wrap);
    return -1;
  }

  memset(pdev, 0, sizeof(struct block_device));

  // Name: "sda" + "1" -> "sda1", "nvme0n1" + "p1" -> "nvme0n1p1"
  if (block_name_is_nvme_ns(parent->name)) {
    snprintf(pdev->name, sizeof(pdev->name), "%sp%d", parent->name, part_index);
  } else {
    snprintf(pdev->name, sizeof(pdev->name), "%s%d", parent->name, part_index);
  }

  pdev->sector_size = parent->sector_size;
  pdev->total_sectors = total_sectors;
  pdev->read_sectors = partition_read;
  if (parent->write_sectors)
    pdev->write_sectors = partition_write;
  if (parent->write_sectors_fua)
    pdev->write_sectors_fua = partition_write_fua;
  if (parent->flush)
    pdev->flush = partition_flush;
  if (parent->read_sectors)
    pdev->read_batch = partition_read_batch;
  if (parent->write_sectors)
    pdev->write_batch = partition_write_batch;
  pdev->driver_data = wrap;

  if (meta) {
    if (meta->partuuid)
      strncpy(pdev->partuuid, meta->partuuid, sizeof(pdev->partuuid) - 1);
    if (meta->partlabel)
      strncpy(pdev->partlabel, meta->partlabel, sizeof(pdev->partlabel) - 1);
    if (meta->type_guid)
      memcpy(pdev->type_guid, meta->type_guid, 16);
    pdev->partition_type = meta->partition_type;
  }

  return block_register(pdev);
}

int block_add_partition(struct block_device *parent, int part_index,
                        uint64_t start_lba, uint64_t total_sectors) {
  return block_add_partition_ex(parent, part_index, start_lba, total_sectors, NULL);
}

struct block_device *block_find_by_partuuid(const char *uuid) {
  if (!uuid || !*uuid)
    return NULL;
  for (int i = 0; i < num_devices; i++) {
    struct block_device *dev = registered[i];
    if (dev && dev->partuuid[0] && strcasecmp(dev->partuuid, uuid) == 0)
      return dev;
  }
  return NULL;
}

struct block_device *block_find_by_partlabel(const char *label) {
  if (!label || !*label)
    return NULL;
  for (int i = 0; i < num_devices; i++) {
    struct block_device *dev = registered[i];
    if (dev && dev->partlabel[0] && strcmp(dev->partlabel, label) == 0)
      return dev;
  }
  return NULL;
}

struct block_device *block_find_by_type(uint8_t partition_type) {
  if (partition_type == PART_TYPE_UNKNOWN)
    return NULL;
  for (int i = 0; i < num_devices; i++) {
    struct block_device *dev = registered[i];
    if (dev && dev->partition_type == partition_type)
      return dev;
  }
  return NULL;
}

bool block_is_partition(const struct block_device *dev) {
  if (!dev)
    return false;
  return dev->read_sectors == partition_read;
}

void block_scan_partitions(struct block_device *dev) {
  if (!dev)
    return;
  if (gpt_scan_partitions(dev) > 0)
    return;
  mbr_scan_partitions(dev);
}

/* Publish a block device as a /dev node.  devfs_register_node() is the
 * canonical registration path used by every other device: it records the node
 * so lookups stay valid and mounts it in the ramfs /dev directory.  Hand-rolling
 * ramfs_create()/vfs_finddir()/vfs_close() here used to free the node while it
 * was still linked in the directory. */
struct hd_geometry {
  unsigned char heads;
  unsigned char sectors;
  unsigned short cylinders;
  unsigned long start;
};

static int block_vfs_ioctl(struct vfs_node *node, uint32_t cmd, uint64_t arg) {
  struct block_device *dev = (struct block_device *)node->device;
  if (!dev)
    return -9; // -EBADF

  uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
  uint64_t total_sectors = dev->total_sectors;
  uint64_t total_bytes = total_sectors * sector_size;

  switch (cmd) {
  case 0x0301: { // HDIO_GETGEO
    struct hd_geometry *geo = (struct hd_geometry *)arg;
    if (!geo || !vmm_is_user_addr_range_valid(arg, sizeof(struct hd_geometry)))
      return -14; // -EFAULT

    geo->heads = 255;
    geo->sectors = 63;
    uint64_t cyl = total_sectors / (255 * 63);
    geo->cylinders = (cyl > 65535) ? 65535 : (unsigned short)cyl;

    uint64_t start_sect = 0;
    if (block_is_partition(dev)) {
      struct partition_wrapper *wrap =
          (struct partition_wrapper *)dev->driver_data;
      if (wrap)
        start_sect = wrap->start_lba;
    }
    geo->start = (unsigned long)start_sect;
    return 0;
  }

  case 0x1260: { // BLKGETSIZE (in 512-byte sectors, unsigned long *)
    unsigned long *out = (unsigned long *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(unsigned long)))
      return -14;
    *out = (unsigned long)(total_bytes / 512);
    return 0;
  }

  case 0x1272:
  case 0x80081272: { // BLKGETSIZE64 (in bytes, uint64_t *)
    uint64_t *out = (uint64_t *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(uint64_t)))
      return -14;
    *out = total_bytes;
    return 0;
  }

  case 0x1268: { // BLKSSZGET (logical sector size, int *)
    int *out = (int *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
      return -14;
    *out = (int)sector_size;
    return 0;
  }

  case 0x1270:
  case 0x126A:
  case 0x80081270: { // BLKBSZGET (block size, int * or size_t *)
    int *out = (int *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
      return -14;
    *out = (int)sector_size;
    return 0;
  }

  case 0x40081271:
  case 0x126B: { // BLKBSZSET
    return 0;
  }

  case 0x125E: { // BLKROGET (read only status, int *)
    int *out = (int *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
      return -14;
    *out = (dev->read_only || !dev->write_sectors) ? 1 : 0;
    return 0;
  }

  case 0x125D: { // BLKROSET (set read only status)
    return 0;
  }

  case 0x1278: { // BLKIOMIN (minimum I/O size, unsigned int *)
    unsigned int *out = (unsigned int *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(unsigned int)))
      return -14;
    *out = sector_size;
    return 0;
  }

  case 0x1279: { // BLKIOOPT (optimal I/O size, unsigned int *)
    unsigned int *out = (unsigned int *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(unsigned int)))
      return -14;
    *out = sector_size;
    return 0;
  }

  case 0x1276: { // BLKALIGNOFF (alignment offset, int *)
    int *out = (int *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
      return -14;
    *out = 0;
    return 0;
  }

  case 0x80041284:
  case 0x1284: { // BLKPBSZGET (physical block size, unsigned int *)
    unsigned int *out = (unsigned int *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(unsigned int)))
      return -14;
    *out = sector_size;
    return 0;
  }

  case 0x127E: { // BLKROTATIONAL (unsigned short *)
    unsigned short *out = (unsigned short *)arg;
    if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(unsigned short)))
      return -14;
    *out = 0; // Non-rotational
    return 0;
  }

  case 0x125F: { // BLKRRPART (re-read partition table)
    if (!block_is_partition(dev)) {
      block_scan_partitions(dev);
      return 0;
    }
    return -22; // -EINVAL on a partition
  }

  case 0x1261: { // BLKFLSBUF (flush buffer cache)
    return block_flush(dev);
  }

  default:
    return -25; // -ENOTTY
  }
}

/* Publish a block device as a /dev node.  devfs_register_node() is the
 * canonical registration path used by every other device: it records the node
 * so lookups stay valid and mounts it in the ramfs /dev directory.  Hand-rolling
 * ramfs_create()/vfs_finddir()/vfs_close() here used to free the node while it
 * was still linked in the directory. */
static void block_publish_node(struct block_device *dev) {
  if (!fs_root || !dev)
    return;

  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;
  vfs_node_init(node);
  strncpy(node->name, dev->name, 127);
  node->flags = FS_BLOCKDEV;
  node->mask = 0600;
  node->length =
      dev->total_sectors * (dev->sector_size ? dev->sector_size : 512);
  node->device = dev;
  node->read = block_vfs_read;
  node->write = block_vfs_write;
  node->ioctl = block_vfs_ioctl;
  /* stat() on /dev/<name> must report the same major:minor that
   * /sys/block, /proc/partitions and /proc/self/mountinfo advertise,
   * otherwise UDisks2/libmount cannot match the device to its mount and
   * Plasma's Disk Usage stays at 0 B. fill_kstat() reports inode verbatim
   * as st_rdev, so store the userspace dev_t encoding here. */
  node->inode = block_device_userspace_rdev(dev);

  devfs_register_node(dev->name, node);
}

uint32_t block_device_userspace_rdev(const struct block_device *dev) {
  if (!dev)
    return 0;
  uint32_t major = dev->linux_major;
  uint32_t minor = dev->linux_minor;
  return (minor & 0xffu) | (major << 8) | ((minor & ~0xffu) << 12);
}

struct block_device *block_get_by_name(const char *name) {
  if (!name || !*name)
    return NULL;
  /* Accept "/dev/nvme0n1p1" as well as bare "nvme0n1p1". */
  if (strncmp(name, "/dev/", 5) == 0)
    name += 5;
  for (int i = 0; i < num_devices; i++) {
    struct block_device *dev = registered[i];
    if (dev && strcmp(dev->name, name) == 0)
      return dev;
  }
  return NULL;
}

int block_register(struct block_device *dev) {
  if (!dev)
    return -1;
  spinlock_acquire(&block_table_lock);
  if (num_devices >= BLOCK_MAX_DEVICES) {
    spinlock_release(&block_table_lock);
    return -1;
  }
  /* Assign a stable Linux-compatible device number once. NVMe uses major
   * 259 like Linux; everything else uses major 8. Minor comes from a counter
   * that is never reused, so removal cannot make two live devices collide. */
  if (dev->linux_major == 0 && dev->linux_minor == 0) {
    if (strncmp(dev->name, "nvme", 4) == 0)
      dev->linux_major = 259;
    else
      dev->linux_major = 8;
    dev->linux_minor = next_minor++;
  }
  registered[num_devices++] = dev;
  spinlock_release(&block_table_lock);

  // Register to VFS if root exists
  if (fs_root)
    block_publish_node(dev);

  // Hot-plug: announce the node with an "add" uevent.  A no-op during early
  // boot, where the sysfs populate pass publishes it instead.  Done before
  // the partition scan so a disk's "add" always precedes its partitions'.
  sysfs_block_device_added(dev);

  // Auto-scan for partitions if this isn't already a partition
  if (!block_is_partition(dev)) {
    block_scan_partitions(dev);
  }

  return 0;
}

/* Remove a device and, first, every partition on it.  Order: stop I/O (gone),
 * drop the /dev node, emit "remove" and drop the sysfs entries, then free the
 * registry slot.  The struct itself is never freed: partitions, /dev vnodes
 * and open descriptors may still point at it, and they all see gone. */
void block_unregister(struct block_device *dev) {
  if (!dev)
    return;
  __atomic_store_n(&dev->gone, true, __ATOMIC_SEQ_CST);

  for (;;) {
    struct block_device *child = NULL;
    spinlock_acquire(&block_table_lock);
    for (int i = 0; i < num_devices && !child; i++) {
      struct block_device *d = registered[i];
      if (d && d != dev && block_is_partition(d) && partition_parent(d) == dev)
        child = d;
    }
    spinlock_release(&block_table_lock);
    if (!child)
      break;
    block_unregister(child);
  }

  spinlock_acquire(&block_table_lock);
  bool present = false;
  for (int i = 0; i < num_devices; i++)
    if (registered[i] == dev) {
      present = true;
      break;
    }
  spinlock_release(&block_table_lock);
  if (!present)
    return;

  vfs_node_t *node = devfs_lookup(dev->name);
  if (node && node->device == dev)
    devfs_unregister_node(dev->name, node);

  sysfs_block_device_removed(dev);

  spinlock_acquire(&block_table_lock);
  for (int i = 0; i < num_devices; i++) {
    if (registered[i] == dev) {
      for (int j = i + 1; j < num_devices; j++)
        registered[j - 1] = registered[j];
      num_devices--;
      registered[num_devices] = NULL;
      break;
    }
  }
  spinlock_release(&block_table_lock);
}

struct block_device *block_get(int index) {
  spinlock_acquire(&block_table_lock);
  struct block_device *dev =
      (index < 0 || index >= num_devices) ? NULL : registered[index];
  spinlock_release(&block_table_lock);
  return dev;
}

int block_count(void) {
  spinlock_acquire(&block_table_lock);
  int n = num_devices;
  spinlock_release(&block_table_lock);
  return n;
}

int block_flush(struct block_device *dev) {
  if (!dev || block_device_gone(dev))
    return -1;
  return dev->flush ? dev->flush(dev) : 0;
}

int block_write_fua(struct block_device *dev, uint64_t lba, uint32_t count,
                    const void *buf) {
  if (!dev || !buf || block_device_gone(dev))
    return -1;
  if (dev->write_sectors_fua)
    return dev->write_sectors_fua(dev, lba, count, buf);
  if (!dev->write_sectors || dev->write_sectors(dev, lba, count, buf) != 0)
    return -1;
  return block_flush(dev);
}

/* Most severe member result: 0 when every member succeeded, -1 on any member
 * error, -2 when a transport failure left no verdict for some member. */
static int block_batch_result(const struct block_io_req *reqs, unsigned n) {
  int worst = 0;
  for (unsigned i = 0; i < n; i++)
    if (reqs[i].result < worst)
      worst = reqs[i].result;
  return worst;
}

int block_read_batch(struct block_device *dev, struct block_io_req *reqs,
                     unsigned n) {
  if (!dev || !reqs || n == 0)
    return -1;
  if (block_device_gone(dev)) {
    block_batch_fail(reqs, n);
    return -1;
  }
  if (dev->read_batch)
    return dev->read_batch(dev, reqs, n);
  for (unsigned i = 0; i < n; i++)
    reqs[i].result = dev->read_sectors
                         ? dev->read_sectors(dev, reqs[i].lba, reqs[i].count,
                                             reqs[i].buf)
                         : -1;
  return block_batch_result(reqs, n);
}

int block_write_batch(struct block_device *dev, struct block_io_req *reqs,
                      unsigned n) {
  if (!dev || !reqs || n == 0)
    return -1;
  if (block_device_gone(dev)) {
    block_batch_fail(reqs, n);
    return -1;
  }
  if (dev->write_batch)
    return dev->write_batch(dev, reqs, n);
  for (unsigned i = 0; i < n; i++)
    reqs[i].result = dev->write_sectors
                         ? dev->write_sectors(dev, reqs[i].lba, reqs[i].count,
                                              reqs[i].buf)
                         : -1;
  return block_batch_result(reqs, n);
}

void block_repopulate_devices(void) {
  if (!fs_root)
    return;

  for (int i = 0; i < num_devices; i++) {
    struct block_device *dev = registered[i];
    if (dev)
      block_publish_node(dev);
  }
}
