#include "drivers/storage/mbr.h"
#include "drivers/storage/block.h"
#include "mm/heap.h"

int mbr_scan_partitions(struct block_device *dev) {
  if (!dev || !dev->read_sectors)
    return -1;

  /* read_sectors transfers one whole device sector. The MBR layout uses
   * only its first 512 bytes, but 4 KiB-sector devices (including zram)
   * still write 4096 bytes into the supplied buffer. */
  uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
  if (sector_size < sizeof(struct mbr))
    return -1;
  struct mbr *mbr = kmalloc(sector_size);
  if (!mbr)
    return -1;

  if (dev->read_sectors(dev, 0, 1, mbr) != 0) {
    kfree(mbr);
    return -1;
  }

  if (mbr->signature != MBR_SIGNATURE) {
    kfree(mbr);
    return -1;
  }

  int count = 0;
  for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
    struct mbr_partition *p = &mbr->partitions[i];
    if (p->type == 0 || p->total_sectors == 0)
      continue;

    if (block_add_partition(dev, i + 1, p->start_lba, p->total_sectors) == 0) {
      count++;
    }
  }

  kfree(mbr);
  return count;
}
