#include "drivers/storage/block.h"
#include <stddef.h>

#include "fs/devfs.h"

static struct block_device *registered[BLOCK_MAX_DEVICES];
static int num_devices = 0;

#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"

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

// MBR Structures

struct mbr_partition {
  uint8_t status;
  uint8_t start_chs[3];
  uint8_t type;
  uint8_t end_chs[3];
  uint32_t start_lba;
  uint32_t total_sectors;
} __attribute__((packed));

struct mbr {
  uint8_t bootstrap[446];
  struct mbr_partition partitions[4];
  uint16_t signature;
} __attribute__((packed));

void block_scan_partitions(struct block_device *dev) {
  if (!dev->read_sectors)
    return;

  struct mbr *mbr = kmalloc(sizeof(struct mbr));
  if (!mbr)
    return;

  if (dev->read_sectors(dev, 0, 1, mbr) != 0) {
    kfree(mbr);
    return;
  }

  if (mbr->signature != 0xAA55) {
    kfree(mbr);
    return;
  }

  for (int i = 0; i < 4; i++) {
    struct mbr_partition *p = &mbr->partitions[i];
    if (p->type == 0 || p->total_sectors == 0)
      continue;

    if ((uint64_t)p->start_lba >= dev->total_sectors ||
        (uint64_t)p->total_sectors >
            dev->total_sectors - (uint64_t)p->start_lba)
      continue;

    struct partition_wrapper *wrap = kmalloc(sizeof(struct partition_wrapper));
    if (!wrap)
      continue;

    wrap->parent = dev;
    wrap->start_lba = p->start_lba;

    struct block_device *pdev = kmalloc(sizeof(struct block_device));
    if (!pdev) {
      kfree(wrap);
      continue;
    }

    memset(pdev, 0, sizeof(struct block_device));

    // Name: "sda" + "1" -> "sda1", "nvme0n1" + "p1" -> "nvme0n1p1"
    strncpy(pdev->name, dev->name, sizeof(pdev->name) - 3);
    int len = (int)strlen(pdev->name);
    if (block_name_is_nvme_ns(dev->name)) {
      pdev->name[len] = 'p';
      pdev->name[len + 1] = (char)('1' + i);
      pdev->name[len + 2] = '\0';
    } else {
      pdev->name[len] = (char)('1' + i);
      pdev->name[len + 1] = '\0';
    }

    pdev->sector_size = dev->sector_size;
    pdev->total_sectors = p->total_sectors;
    pdev->read_sectors = partition_read;
    if (dev->write_sectors)
      pdev->write_sectors = partition_write;
    if (dev->write_sectors_fua)
      pdev->write_sectors_fua = partition_write_fua;
    if (dev->flush)
      pdev->flush = partition_flush;
    pdev->driver_data = wrap;

    block_register(pdev);
  }

  kfree(mbr);
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
  if (dev->read_sectors != partition_read) {
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
