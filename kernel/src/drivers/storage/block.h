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

/* Millisecond elapsed-time samples; buckets: <1, <2, <4, <8, <16,
 * <32, <64, >=64 ms. Counts include failures, totals overlap across CPUs. */
struct storage_latency {
    uint64_t count, total_ms, max_ms, errors, buckets[8];
};
static inline void storage_latency_record(struct storage_latency *s,
                                          uint64_t ms, bool failed) {
    __atomic_add_fetch(&s->count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->total_ms, ms, __ATOMIC_RELAXED);
    if (failed) __atomic_add_fetch(&s->errors, 1, __ATOMIC_RELAXED);
    uint64_t old = __atomic_load_n(&s->max_ms, __ATOMIC_RELAXED);
    while (old < ms && !__atomic_compare_exchange_n(&s->max_ms, &old, ms,
             false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    unsigned bucket = 0;
    while (bucket < 7 && ms >= (1ULL << bucket)) bucket++;
    __atomic_add_fetch(&s->buckets[bucket], 1, __ATOMIC_RELAXED);
}

static inline void storage_latency_snapshot(const struct storage_latency *s,
                                            struct storage_latency *out) {
    out->count = __atomic_load_n(&s->count, __ATOMIC_RELAXED);
    out->total_ms = __atomic_load_n(&s->total_ms, __ATOMIC_RELAXED);
    out->max_ms = __atomic_load_n(&s->max_ms, __ATOMIC_RELAXED);
    out->errors = __atomic_load_n(&s->errors, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < 8; i++)
        out->buckets[i] = __atomic_load_n(&s->buckets[i], __ATOMIC_RELAXED);
}

/* One member of a pipelined block I/O batch.  A batch lets a single submitter
 * keep several requests on the device queue instead of paying a completion
 * wait per request.  Members must be independent and disjoint: a driver may
 * complete them out of order.  `buf` is a kernel buffer that stays valid until
 * the batch call returns.  `result` is filled by the call: 0 success, -1 I/O
 * or pre-submit error, -2 transport failure. */
struct block_io_req {
    uint64_t lba;       // first sector, in the device's own sector units
    uint32_t count;     // sectors
    void *buf;
    int result;
};

struct block_device {
    char name[16];              // e.g. "ata0", "ata1"
    uint32_t sector_size;       // Usually 512
    uint64_t total_sectors;     // Total number of sectors on the device
    /* Linux-compatible major:minor for this device. Assigned once in
     * block_register() and never changed: NVMe devices use major 259 like
     * Linux, everything else uses major 8. Minor is the registration index,
     * unique across whole disks and partitions. All of sysfs dev, proc
     * partitions, diskstats, mountinfo and stat st_rdev/st_dev must agree
     * on these numbers or UDisks2/Solid cannot match a mount to its block
     * device and Plasma Disk Usage widget stays at 0 B. */
    uint32_t linux_major;
    uint32_t linux_minor;
    int (*read_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    // Optional: write with FUA (force unit access) so the device makes the
    // data durable before completing; callers that need that guarantee may
    // prefer it over write_sectors + flush when available.
    int (*write_sectors_fua)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    // Optional pipelined variants: submit every member before waiting, so one
    // submitter can use more than one device queue slot.  Implementations must
    // fill reqs[i].result for every member; block_read_batch()/block_write_batch()
    // fall back to one read_sectors/write_sectors call per member when unset.
    int (*read_batch)(struct block_device *dev, struct block_io_req *reqs, unsigned n);
    int (*write_batch)(struct block_device *dev, struct block_io_req *reqs, unsigned n);
    int (*flush)(struct block_device *dev);
    void *driver_data;          // Opaque pointer for the specific driver

    /* I/O statistics for /proc/diskstats (all in 512-byte sector units,
     * matching Linux which always reports 512B units regardless of the
     * hardware sector size). Updated with lock-free atomics at the driver
     * entry points, so no per-device lock is needed. Partition devices
     * count their own I/O while the parent counts the same request again
     * when the partition wrapper forwards it — exactly like Linux, which
     * accounts both the partition and the whole disk. Zero-initialized
     * with the rest of the struct by every driver (memset). */
    uint64_t stat_reads;        // completed read operations
    uint64_t stat_read_sectors; // 512B sectors read
    uint64_t stat_writes;       // completed write operations
    uint64_t stat_write_sectors;// 512B sectors written

    /* Actual submitted NVMe commands, including retries and failures.
     * Namespace-only: partition forwarding does not double-count these. */
    uint64_t hw_reads, hw_writes, hw_read_bytes, hw_write_bytes, hw_errors;
    struct storage_latency hw_flush;

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

// Pipelined forms: submit the whole batch (or as much of it as the device can
// hold) before waiting.  They fill every member's `result` and return 0 when
// all succeeded, -1 on any command status error, -2 when a transport failure
// left members unfinished.  Devices without batch ops run the members through
// the single-request path, which keeps the same per-member results.
int block_read_batch(struct block_device *dev, struct block_io_req *reqs,
                     unsigned n);
int block_write_batch(struct block_device *dev, struct block_io_req *reqs,
                      unsigned n);

// Account one completed I/O on `dev` for /proc/diskstats. `count` is in
// units of the device's own sector size; statistics are kept in
// Linux-compatible 512-byte units. Call only on success from the driver
// read/write entry points (and the partition wrappers); lock-free atomics
// mean no per-device lock is needed.
static inline void block_stat_read(struct block_device *dev, uint32_t count) {
  if (!dev || count == 0)
    return;
  uint64_t s512 =
      (uint64_t)count * (dev->sector_size ? dev->sector_size : 512) / 512;
  __atomic_add_fetch(&dev->stat_reads, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&dev->stat_read_sectors, s512, __ATOMIC_RELAXED);
}

static inline void block_stat_write(struct block_device *dev, uint32_t count) {
  if (!dev || count == 0)
    return;
  uint64_t s512 =
      (uint64_t)count * (dev->sector_size ? dev->sector_size : 512) / 512;
  __atomic_add_fetch(&dev->stat_writes, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&dev->stat_write_sectors, s512, __ATOMIC_RELAXED);
}

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

// Lookup a block device by name. Accepts both bare names ("nvme0n1p1") and
// full dev paths ("/dev/nvme0n1p1"). Returns NULL when not found.
struct block_device *block_get_by_name(const char *name);

// Userspace dev_t (glibc/musl sysmacros encoding) for stat()->st_rdev/st_dev:
// (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12). Matches the
// kpi_userspace_devt() convention used by every native chardev.
uint32_t block_device_userspace_rdev(const struct block_device *dev);

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
