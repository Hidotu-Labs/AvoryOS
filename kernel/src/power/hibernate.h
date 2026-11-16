#ifndef AVORY_POWER_HIBERNATE_H
#define AVORY_POWER_HIBERNATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct block_device;

#define HIBERNATE_MAGIC   0x4156525953344849ULL /* "AVRYS4HI" */
#define HIBERNATE_VERSION 1u
#define HIBERNATE_PAGE_SIZE 4096u

/* Callee-saved and architectural BSP state needed to resume from S4. */
struct hibernate_cpu_state {
  uint64_t cr3;
  uint64_t rsp;
  uint64_t rip;
  uint64_t rflags;
  uint64_t rbx;
  uint64_t rbp;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
};

/* Contiguous run of saved physical pages mapped to contiguous disk sectors. */
struct hibernate_extent {
  uint64_t phys_addr;   /* 4 KiB page-aligned physical base address */
  uint64_t page_count;  /* Number of 4 KiB pages in this run */
  uint64_t disk_sector; /* Starting sector on the hibernation block device */
};

/* On-disk header stored at sector 0 of the hibernation target.
 * Padded to 512 bytes so it fits in a single sector on both 512B and 4096B
 * block devices. */
struct hibernate_header {
  uint64_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t page_size;
  uint32_t header_crc32; /* IEEE CRC32 over this struct with header_crc32=0 */
  uint32_t data_crc32;   /* IEEE CRC32 over the extent table + page payload */
  uint32_t extent_count;
  uint64_t total_pages;
  uint64_t extent_table_sector;
  uint64_t extent_table_sectors;
  struct hibernate_cpu_state cpu;
  uint8_t reserved[512 - 136];
};

_Static_assert(sizeof(struct hibernate_header) == 512,
               "hibernate_header must be 512 bytes");

/* Compute standard IEEE 802.3 CRC32 (and incremental helper). */
uint32_t hibernate_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t hibernate_crc32(const void *data, size_t len);

/* Locate the hibernation target block device from kernel cmdline (`resume=...`)
 * or fall back to the first GPT Linux Swap partition. */
struct block_device *hibernate_find_target(void);

/* Read and validate the hibernation header at sector 0 of `dev`. */
int hibernate_read_header(struct block_device *dev, struct hibernate_header *out);

/* Finalize header CRC32 and durably write it to sector 0 of `dev`. */
int hibernate_write_header(struct block_device *dev,
                           struct hibernate_header *hdr);

/* Validate header fields, CRC32, and bounds against `dev`. */
bool hibernate_header_valid(const struct hibernate_header *hdr,
                            uint64_t dev_total_sectors, uint32_t sector_size);

/* Durably zero sector 0 on `dev` so a stale image is never replayed. */
int hibernate_invalidate_header(struct block_device *dev);

/* Phase 3: Snapshot allocated physical memory to the hibernation target and
 * enter ACPI S4 (falling back to S5 if _S4_ is absent). */
int power_s4_enter(void);
int power_s4_snapshot_and_sleep(uint64_t resume_rsp, uint64_t resume_rip);
int power_s4_resume(void);

/* Phase 4: Early-boot hibernation probe and memory relocation resume.
 * Must run after storage drivers initialize and before rootfs is mounted. */
void power_s4_try_resume(void);

#endif
