#ifndef STORAGE_MBR_H
#define STORAGE_MBR_H

#include <stdbool.h>
#include <stdint.h>

#define MBR_SIGNATURE 0xAA55
#define MBR_PARTITION_COUNT 4

/* Standard partition entry in the Master Boot Record */
struct mbr_partition {
  uint8_t status;
  uint8_t start_chs[3];
  uint8_t type;
  uint8_t end_chs[3];
  uint32_t start_lba;
  uint32_t total_sectors;
} __attribute__((packed));

/* Master Boot Record sector structure */
struct mbr {
  uint8_t bootstrap[446];
  struct mbr_partition partitions[MBR_PARTITION_COUNT];
  uint16_t signature;
} __attribute__((packed));

struct block_device;

/* Scan for MBR partitions on a block device.
 * Returns the number of partitions registered, or negative on error. */
int mbr_scan_partitions(struct block_device *dev);

#endif /* STORAGE_MBR_H */
