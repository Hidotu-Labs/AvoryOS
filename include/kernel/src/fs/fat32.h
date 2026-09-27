#ifndef FS_FAT32_H
#define FS_FAT32_H

#include "drivers/storage/block.h"
#include "vfs.h"
#include <stdbool.h>
#include <stdint.h>

// FAT32 Constants

#define FAT32_EOF_MARKER 0x0FFFFFF8 // End of chain marker
#define FAT32_BAD_MARKER 0x0FFFFFF7 // Bad cluster marker

// FAT32 Boot Parameter Block (BPB)

typedef struct __attribute__((packed)) {
  uint8_t jmp_boot[3];       // Jump instruction
  uint8_t oem_name[8];       // OEM name
  uint16_t bytes_per_sector; // Bytes per sector (512, 1024, 2048, 4096)
  uint8_t
      sectors_per_cluster; // Sectors per cluster (1, 2, 4, 8, 16, 32, 64, 128)
  uint16_t reserved_sectors;  // Reserved sectors from sector 0
  uint8_t num_fats;           // Number of FATs (usually 2)
  uint16_t root_entry_count;  // 0 for FAT32
  uint16_t total_sectors_16;  // 0 for FAT32
  uint8_t media_type;         // Media type (0xF8 for fixed disk)
  uint16_t fat_size_16;       // 0 for FAT32
  uint16_t sectors_per_track; // Sectors per track
  uint16_t num_heads;         // Number of heads
  uint32_t hidden_sectors;    // Hidden sectors
  uint32_t total_sectors_32;  // Total sectors (if total_sectors_16 is 0)
} fat32_bpb_t;

// FAT32 Extended BIOS Parameter Block (EBPB)

typedef struct __attribute__((packed)) {
  uint32_t fat_size_32;        // Sectors per FAT
  uint16_t ext_flags;          // Extended flags
  uint16_t fs_version;         // Filesystem version (0:0)
  uint32_t root_cluster;       // First cluster of root directory
  uint16_t fs_info_sector;     // Sector number of FSINFO structure
  uint16_t backup_boot_sector; // Sector number of backup boot sector
  uint8_t reserved[12];        // Reserved
  uint8_t drive_number;        // Drive number
  uint8_t reserved1;           // Reserved
  uint8_t boot_signature;      // Extended boot signature (0x29)
  uint32_t volume_serial;      // Volume serial number
  uint8_t volume_label[11];    // Volume label
  uint8_t fs_type[8];          // "FAT32   "
} fat32_ebpb_t;

// Complete FAT32 Boot Sector

typedef struct __attribute__((packed)) {
  fat32_bpb_t bpb;
  fat32_ebpb_t ebpb;
  uint8_t boot_code[420]; // Boot code
  uint16_t signature;     // Boot signature (0xAA55)
} fat32_boot_sector_t;

// FAT32 Short Filename Directory Entry (SFN)

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN 0x02
#define FAT32_ATTR_SYSTEM 0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE 0x20
#define FAT32_ATTR_LFN 0x0F // LFN entry marker (RHSV bits set)

typedef struct __attribute__((packed)) {
  uint8_t name[11];          // 8.3 filename (space-padded)
  uint8_t attr;              // Attributes
  uint8_t reserved;          // Reserved (used for NTRes)
  uint8_t create_time_tenth; // Creation time, tenths of a second
  uint16_t create_time;      // Creation time
  uint16_t create_date;      // Creation date
  uint16_t last_access_date; // Last access date
  uint16_t cluster_high;     // High 16 bits of first cluster
  uint16_t modify_time;      // Modification time
  uint16_t modify_date;      // Modification date
  uint16_t cluster_low;      // Low 16 bits of first cluster
  uint32_t file_size;        // File size in bytes
} fat32_dir_entry_t;

// FAT32 Long Filename Directory Entry (LFN)

#define FAT32_LFN_LAST_ENTRY 0x40 // Last LFN entry in sequence

typedef struct __attribute__((packed)) {
  uint8_t seq;       // Sequence number (OR with 0x40 for last)
  uint16_t name1[5]; // First 5 characters of name portion
  uint8_t attr;      // Always 0x0F for LFN
  uint8_t type;      // Always 0
  uint8_t checksum;  // Checksum of SFN
  uint16_t name2[6]; // Next 6 characters
  uint16_t reserved; // Always 0
  uint16_t name3[2]; // Last 2 characters
} fat32_lfn_entry_t;

// FAT32 FSINFO Structure

typedef struct __attribute__((packed)) {
  uint32_t lead_signature;   // 0x41615252
  uint8_t reserved1[480];    // Reserved
  uint32_t struct_signature; // 0x61417272
  uint32_t free_clusters;    // Count of free clusters (0xFFFFFFFF if unknown)
  uint32_t next_free;        // First free cluster hint
  uint8_t reserved2[12];     // Reserved
  uint32_t trail_signature;  // 0xAA550000
} fat32_fsinfo_t;

// FAT32 Special Cluster Values

#define FAT32_FREE_CLUSTER 0x00000000
#define FAT32_RESERVED_CLUSTER 0x0FFFFFF0
#define FAT32_BAD_CLUSTER 0x0FFFFFF7
#define FAT32_EOF_CLUSTER_MIN 0x0FFFFFF8
#define FAT32_EOF_CLUSTER_MAX 0x0FFFFFFF
#define FAT32_IS_EOF(c) ((c) >= FAT32_EOF_CLUSTER_MIN)
#define FAT32_IS_VALID(c) ((c) >= 2 && (c) < FAT32_EOF_CLUSTER_MIN)

// FAT32 Mount Context

typedef struct {
  struct block_device *dev;     // Underlying block device
  fat32_boot_sector_t boot;     // Cached boot sector
  uint32_t bytes_per_sector;    // Bytes per sector
  uint32_t sectors_per_cluster; // Sectors per cluster
  uint32_t bytes_per_cluster;   // Bytes per cluster
  uint32_t fat_start_sector;    // First sector of FAT
  uint32_t fat_size_sectors;    // Size of one FAT in sectors
  uint32_t data_start_sector;   // First sector of data area
  uint32_t root_cluster;        // Root directory cluster
  uint32_t total_clusters;      // Total data clusters
  uint32_t cluster_count;       // Number of clusters
  vfs_node_t *root_node;        // VFS node for root directory
} fat32_mount_t;

// Public API

// Mount a FAT32 filesystem from the given block device onto the given VFS node.
// Returns 0 on success, -1 on failure.
int fat32_mount(struct block_device *dev, vfs_node_t *mountpoint);

// Mount FAT32 as the root filesystem, replacing the current fs_root.
// Returns 0 on success, -1 on failure.
int fat32_mount_root(struct block_device *dev);

// Read a cluster from the filesystem.
// Returns 0 on success, -1 on failure.
int fat32_read_cluster(fat32_mount_t *mnt, uint32_t cluster, void *buffer);

// Get the next cluster in the chain from the FAT.
// Returns the next cluster number, or 0 on error/EOF.
uint32_t fat32_get_next_cluster(fat32_mount_t *mnt, uint32_t cluster);

// Self-test function for FAT32 driver (phases 1-4)
void fat32_self_test(void);

#endif // FS_FAT32_H
