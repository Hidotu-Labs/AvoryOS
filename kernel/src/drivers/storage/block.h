#ifndef BLOCK_BLOCK_H
#define BLOCK_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Whole disks plus one entry per partition; a multi-namespace NVMe config
 * with partitions easily exceeds the old 16. */
#define BLOCK_MAX_DEVICES 64
#define BLOCK_SECTOR_SIZE 512

enum partition_type {
    PART_TYPE_UNKNOWN = 0,
    PART_TYPE_ESP,              /* EFI System Partition */
    PART_TYPE_LINUX_ROOT,       /* Linux Root (x86-64 Discoverable Partitions) */
    PART_TYPE_LINUX_GENERIC,    /* Linux Generic Filesystem */
    PART_TYPE_LINUX_SWAP,       /* Linux Swap */
    PART_TYPE_BASIC_DATA,       /* Windows Basic Data / FAT / NTFS */
};

struct partition_meta {
    const char *partuuid;       /* Formatted UUID string, e.g. "4f68bce3-..." */
    const char *partlabel;      /* UTF-8 label, e.g. "rootfs" */
    const uint8_t *type_guid;   /* Raw 16-byte type GUID */
    uint8_t partition_type;     /* enum partition_type */
};

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

    // Partition metadata (if this device is a partition)
    char partuuid[37];          // Null-terminated PARTUUID string
    char partlabel[64];         // Null-terminated PARTLABEL string
    uint8_t type_guid[16];      // Raw 16-byte partition type GUID
    uint8_t partition_type;     // enum partition_type
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

// Create and register a partition on a parent block device.
// part_index is 1-based (e.g. 1 for sda1).
// Returns 0 on success, negative on error.
int block_add_partition(struct block_device *parent, int part_index,
                        uint64_t start_lba, uint64_t total_sectors);

// Create and register a partition on a parent block device with metadata.
int block_add_partition_ex(struct block_device *parent, int part_index,
                           uint64_t start_lba, uint64_t total_sectors,
                           const struct partition_meta *meta);

// Lookup a block device by PARTUUID (case-insensitive)
struct block_device *block_find_by_partuuid(const char *uuid);

// Lookup a block device by PARTLABEL (exact match)
struct block_device *block_find_by_partlabel(const char *label);

// Lookup a block device by partition type (e.g. PART_TYPE_LINUX_ROOT)
struct block_device *block_find_by_type(uint8_t partition_type);

// Check if a block device is a partition device.
bool block_is_partition(const struct block_device *dev);

// Scan for partitions on a block device.
void block_scan_partitions(struct block_device *dev);

// Re-register all devices to the current /dev directory.
// Call this after mounting a new root filesystem.
void block_repopulate_devices(void);

#endif
