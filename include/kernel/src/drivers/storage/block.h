#ifndef BLOCK_BLOCK_H
#define BLOCK_BLOCK_H

#include <stdint.h>

/* Whole disks plus one entry per partition; a multi-namespace NVMe config
 * with partitions easily exceeds the old 16. */
#define BLOCK_MAX_DEVICES 64
#define BLOCK_SECTOR_SIZE 512

struct block_device {
    char name[16];              // e.g. "ata0", "ata1"
    uint32_t sector_size;       // Usually 512
    uint64_t total_sectors;     // Total number of sectors on the device
    int (*read_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    // Optional: write with FUA (force unit access) so the device makes the
    // data durable before completing; callers that need that guarantee may
    // prefer it over write_sectors + flush when available.
    int (*write_sectors_fua)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct block_device *dev);
    void *driver_data;          // Opaque pointer for the specific driver
};

// Register a block device. Returns 0 on success, -1 if full.
int block_register(struct block_device *dev);

// Get a registered block device by index. Returns NULL if invalid.
struct block_device *block_get(int index);

// Get the number of registered block devices.
int block_count(void);
int block_flush(struct block_device *dev);

// Durable write: uses the device FUA path when available, otherwise a plain
// write followed by a flush. Returns 0 on success, -1 on failure.
int block_write_fua(struct block_device *dev, uint64_t lba, uint32_t count,
                    const void *buf);

// Scan for partitions on a block device (MBR).
void block_scan_partitions(struct block_device *dev);

// Re-register all devices to the current /dev directory.
// Call this after mounting a new root filesystem.
void block_repopulate_devices(void);

#endif
