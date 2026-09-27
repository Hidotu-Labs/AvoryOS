#ifndef STORAGE_GPT_H
#define STORAGE_GPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "drivers/storage/block.h"

#define GPT_SIGNATURE "EFI PART"
#define GPT_REVISION_1_0 0x00010000
#define GPT_MIN_HEADER_SIZE 92
#define GPT_DEFAULT_ENTRY_SIZE 128
#define GPT_MAX_PARTITIONS 1024

/* Standard UEFI GUID Partition Table Header */
struct gpt_header {
  uint8_t signature[8];           /* "EFI PART" */
  uint32_t revision;              /* Typically 0x00010000 */
  uint32_t header_size;           /* Size of header in bytes (usually 92) */
  uint32_t header_crc32;          /* CRC32 of header with this field zeroed */
  uint32_t reserved;              /* Must be zero */
  uint64_t current_lba;           /* LBA of this header block */
  uint64_t backup_lba;            /* LBA of alternate GPT header */
  uint64_t first_usable_lba;      /* First usable LBA for partitions */
  uint64_t last_usable_lba;       /* Last usable LBA for partitions */
  uint8_t disk_guid[16];          /* Unique disk GUID */
  uint64_t partition_entry_lba;   /* Starting LBA of partition entries */
  uint32_t num_partition_entries; /* Number of partition entries (usually 128) */
  uint32_t partition_entry_size;  /* Size of each entry (usually 128) */
  uint32_t partition_array_crc32; /* CRC32 of entire partition entries array */
} __attribute__((packed));

/* Standard UEFI GUID Partition Table Entry */
struct gpt_entry {
  uint8_t type_guid[16];          /* Partition type GUID (all zero = unused) */
  uint8_t unique_guid[16];        /* Unique partition GUID */
  uint64_t starting_lba;          /* First LBA of the partition */
  uint64_t ending_lba;            /* Last LBA of the partition (inclusive) */
  uint64_t attributes;            /* Attribute flags */
  uint16_t name[36];              /* Partition label (UTF-16LE) */
} __attribute__((packed));

/* Convert a mixed-endian UEFI GUID to lowercase standard UUID string (36 chars + '\0') */
void gpt_guid_to_str(const uint8_t guid[16], char *str, size_t size);

/* Convert a UTF-16LE string to UTF-8 */
void gpt_utf16le_to_utf8(const uint16_t *src, size_t src_len, char *dst, size_t dst_size);

/* Classify a partition type GUID into an enum partition_type */
uint8_t gpt_classify_type_guid(const uint8_t guid[16]);

/* Return human-readable string for partition type */
const char *gpt_partition_type_str(uint8_t partition_type);

/* Scan and register GPT partitions on a block device.
 * Checks primary GPT, verifies CRC32 checksums, falls back to backup GPT
 * if primary is corrupted, validates partition intervals, and registers
 * child block devices via block_add_partition_ex.
 *
 * Returns number of partitions registered (>0 on success, 0 if not GPT, <0 on error). */
int gpt_scan_partitions(struct block_device *dev);

#endif /* STORAGE_GPT_H */
