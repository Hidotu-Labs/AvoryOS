#include "drivers/storage/block.h"
#include "drivers/storage/gpt.h"
#include "drivers/storage/mbr.h"
#include <stddef.h>

#include "fs/devfs.h"

static struct block_device *registered[BLOCK_MAX_DEVICES];
static int num_devices = 0;

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

static int partition_read(struct block_device *dev, uint64_t lba,
                          uint32_t count, void *buf) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (!wrap || !wrap->parent || !buf || count == 0 ||
      lba >= dev->total_sectors ||
      (uint64_t)count > dev->total_sectors - lba)
    return -1;
  return wrap->parent->read_sectors(wrap->parent, lba + wrap->start_lba, count,
                                    buf);
}

static int partition_write(struct block_device *dev, uint64_t lba,
                           uint32_t count, const void *buf) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (!wrap || !wrap->parent || !wrap->parent->write_sectors || !buf ||
      count == 0 || lba >= dev->total_sectors ||
      (uint64_t)count > dev->total_sectors - lba)
    return -1;
  return wrap->parent->write_sectors(wrap->parent, lba + wrap->start_lba, count,
                                     buf);
}

static int partition_write_fua(struct block_device *dev, uint64_t lba,
                               uint32_t count, const void *buf) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (!wrap || !wrap->parent || !wrap->parent->write_sectors || !buf ||
      count == 0 || lba >= dev->total_sectors ||
      (uint64_t)count > dev->total_sectors - lba)
    return -1;
  uint64_t parent_lba = lba + wrap->start_lba;
  struct block_device *parent = wrap->parent;
  if (parent->write_sectors_fua)
    return parent->write_sectors_fua(parent, parent_lba, count, buf);
  if (parent->write_sectors(parent, parent_lba, count, buf) != 0)
    return -1;
  return block_flush(parent);
}

static int partition_flush(struct block_device *dev) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (!wrap || !wrap->parent)
    return -1;
  return block_flush(wrap->parent);
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
    *out = 0; // Read-write
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

  devfs_register_node(dev->name, node);
}

int block_register(struct block_device *dev) {
  if (num_devices >= BLOCK_MAX_DEVICES)
    return -1;
  registered[num_devices++] = dev;

  // Register to VFS if root exists
  if (fs_root)
    block_publish_node(dev);

  // Auto-scan for partitions if this isn't already a partition
  if (!block_is_partition(dev)) {
    block_scan_partitions(dev);
  }

  return 0;
}

struct block_device *block_get(int index) {
  if (index < 0 || index >= num_devices)
    return NULL;
  return registered[index];
}

int block_count(void) { return num_devices; }

int block_flush(struct block_device *dev) {
  if (!dev)
    return -1;
  return dev->flush ? dev->flush(dev) : 0;
}

int block_write_fua(struct block_device *dev, uint64_t lba, uint32_t count,
                    const void *buf) {
  if (!dev || !buf)
    return -1;
  if (dev->write_sectors_fua)
    return dev->write_sectors_fua(dev, lba, count, buf);
  if (!dev->write_sectors || dev->write_sectors(dev, lba, count, buf) != 0)
    return -1;
  return block_flush(dev);
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
