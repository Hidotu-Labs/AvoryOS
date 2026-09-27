#include "drivers/storage/gpt.h"
#include "console/klog.h"
#include "drivers/storage/block.h"
#include "lib/string.h"
#include "mm/heap.h"

#define GPT_MAX_TABLE_BYTES (256 * 1024)

static const uint8_t GUID_ESP[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
}; /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */

static const uint8_t GUID_LINUX_ROOT_X86_64[16] = {
    0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d,
    0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09
}; /* 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709 */

static const uint8_t GUID_LINUX_GENERIC[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
}; /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */

static const uint8_t GUID_LINUX_SWAP[16] = {
    0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
    0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f
}; /* 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F */

static const uint8_t GUID_BASIC_DATA[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
}; /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */

static uint32_t crc32_tab[256];
static bool crc32_tab_inited = false;

static void crc32_init_tab(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++) {
      if (c & 1)
        c = 0xEDB88320u ^ (c >> 1);
      else
        c = c >> 1;
    }
    crc32_tab[i] = c;
  }
  crc32_tab_inited = true;
}

/* Calculate IEEE 802.3 standard 32-bit CRC */
static uint32_t gpt_crc32(const void *buf, size_t len) {
  if (!crc32_tab_inited)
    crc32_init_tab();

  const uint8_t *p = (const uint8_t *)buf;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc = crc32_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

static bool gpt_guid_is_zero(const uint8_t guid[16]) {
  for (int i = 0; i < 16; i++) {
    if (guid[i] != 0)
      return false;
  }
  return true;
}

void gpt_guid_to_str(const uint8_t guid[16], char *str, size_t size) {
  if (size < 37) {
    if (size > 0)
      str[0] = '\0';
    return;
  }
  uint32_t d1 = (uint32_t)guid[0] | ((uint32_t)guid[1] << 8) |
                ((uint32_t)guid[2] << 16) | ((uint32_t)guid[3] << 24);
  uint16_t d2 = (uint16_t)guid[4] | ((uint16_t)guid[5] << 8);
  uint16_t d3 = (uint16_t)guid[6] | ((uint16_t)guid[7] << 8);
  snprintf(str, size,
           "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           d1, d2, d3,
           guid[8], guid[9], guid[10], guid[11],
           guid[12], guid[13], guid[14], guid[15]);
}

void gpt_utf16le_to_utf8(const uint16_t *src, size_t src_len, char *dst, size_t dst_size) {
  if (!dst || dst_size == 0)
    return;

  size_t out = 0;
  for (size_t i = 0; i < src_len && src[i] != 0; i++) {
    uint16_t ch = src[i];
    if (ch < 0x80) {
      if (out + 1 >= dst_size)
        break;
      dst[out++] = (char)ch;
    } else if (ch < 0x800) {
      if (out + 2 >= dst_size)
        break;
      dst[out++] = (char)(0xC0 | (ch >> 6));
      dst[out++] = (char)(0x80 | (ch & 0x3F));
    } else {
      if (out + 3 >= dst_size)
        break;
      dst[out++] = (char)(0xE0 | (ch >> 12));
      dst[out++] = (char)(0x80 | ((ch >> 6) & 0x3F));
      dst[out++] = (char)(0x80 | (ch & 0x3F));
    }
  }
  dst[out] = '\0';
}

uint8_t gpt_classify_type_guid(const uint8_t guid[16]) {
  if (memcmp(guid, GUID_LINUX_ROOT_X86_64, 16) == 0)
    return PART_TYPE_LINUX_ROOT;
  if (memcmp(guid, GUID_ESP, 16) == 0)
    return PART_TYPE_ESP;
  if (memcmp(guid, GUID_LINUX_GENERIC, 16) == 0)
    return PART_TYPE_LINUX_GENERIC;
  if (memcmp(guid, GUID_LINUX_SWAP, 16) == 0)
    return PART_TYPE_LINUX_SWAP;
  if (memcmp(guid, GUID_BASIC_DATA, 16) == 0)
    return PART_TYPE_BASIC_DATA;
  return PART_TYPE_UNKNOWN;
}

const char *gpt_partition_type_str(uint8_t partition_type) {
  switch (partition_type) {
  case PART_TYPE_LINUX_ROOT:
    return "Linux root (x86-64)";
  case PART_TYPE_ESP:
    return "EFI System Partition";
  case PART_TYPE_LINUX_GENERIC:
    return "Linux generic filesystem";
  case PART_TYPE_LINUX_SWAP:
    return "Linux swap";
  case PART_TYPE_BASIC_DATA:
    return "Basic data";
  default:
    return "Generic/Unknown";
  }
}

enum pmbr_type {
  PMBR_ABSENT = 0,
  PMBR_VALID,
  PMBR_HYBRID,
};

static enum pmbr_type gpt_check_pmbr(struct block_device *dev, uint32_t sector_size) {
  uint8_t *mbr_buf = kmalloc(sector_size);
  if (!mbr_buf)
    return PMBR_ABSENT;

  if (dev->read_sectors(dev, 0, 1, mbr_buf) != 0) {
    kfree(mbr_buf);
    return PMBR_ABSENT;
  }

  uint16_t sig = (uint16_t)mbr_buf[510] | ((uint16_t)mbr_buf[511] << 8);
  if (sig != 0xAA55) {
    kfree(mbr_buf);
    return PMBR_ABSENT;
  }

  bool has_ee = false;
  bool has_other = false;

  for (int i = 0; i < 4; i++) {
    uint8_t *part = mbr_buf + 446 + (i * 16);
    uint8_t type = part[4];
    if (type == 0xEE) {
      has_ee = true;
    } else if (type != 0x00) {
      has_other = true;
    }
  }

  kfree(mbr_buf);

  if (has_ee && has_other)
    return PMBR_HYBRID;
  if (has_ee)
    return PMBR_VALID;
  return PMBR_ABSENT;
}

static bool gpt_validate_header(const struct gpt_header *hdr,
                                uint64_t expected_lba, uint64_t total_sectors,
                                uint32_t sector_size) {
  if (memcmp(hdr->signature, GPT_SIGNATURE, 8) != 0)
    return false;

  if (hdr->revision != GPT_REVISION_1_0)
    return false;

  if (hdr->header_size < GPT_MIN_HEADER_SIZE || hdr->header_size > sector_size)
    return false;

  if (hdr->current_lba != expected_lba)
    return false;

  if (hdr->first_usable_lba > hdr->last_usable_lba ||
      hdr->last_usable_lba >= total_sectors)
    return false;

  if (hdr->num_partition_entries == 0 ||
      hdr->num_partition_entries > GPT_MAX_PARTITIONS)
    return false;

  if (hdr->partition_entry_size < sizeof(struct gpt_entry) ||
      (hdr->partition_entry_size % 8) != 0)
    return false;

  uint64_t total_table_bytes =
      (uint64_t)hdr->num_partition_entries * hdr->partition_entry_size;
  if (total_table_bytes > GPT_MAX_TABLE_BYTES)
    return false;

  // Validate header CRC32 with header_crc32 field zeroed
  struct gpt_header copy;
  memcpy(&copy, hdr, hdr->header_size);
  copy.header_crc32 = 0;
  uint32_t calc_crc = gpt_crc32(&copy, copy.header_size);
  if (calc_crc != hdr->header_crc32)
    return false;

  return true;
}

static uint8_t *gpt_read_entries(struct block_device *dev,
                                 const struct gpt_header *hdr,
                                 uint32_t sector_size) {
  uint64_t total_bytes =
      (uint64_t)hdr->num_partition_entries * hdr->partition_entry_size;
  uint32_t num_sectors =
      (uint32_t)((total_bytes + sector_size - 1) / sector_size);

  uint8_t *buf = kmalloc(num_sectors * sector_size);
  if (!buf)
    return NULL;

  if (dev->read_sectors(dev, hdr->partition_entry_lba, num_sectors, buf) != 0) {
    kfree(buf);
    return NULL;
  }

  uint32_t calc_crc = gpt_crc32(buf, (size_t)total_bytes);
  if (calc_crc != hdr->partition_array_crc32) {
    kfree(buf);
    return NULL;
  }

  return buf;
}

struct partition_range {
  uint64_t start;
  uint64_t end;
};

int gpt_scan_partitions(struct block_device *dev) {
  if (!dev || !dev->read_sectors || dev->total_sectors < 34)
    return 0;

  uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;

  // Check Protective MBR and warn if Hybrid MBR is detected
  enum pmbr_type pmbr = gpt_check_pmbr(dev, sector_size);
  if (pmbr == PMBR_HYBRID) {
    klogf("[WARN] %s: Hybrid MBR detected! MBR and GPT partition tables may conflict.\n",
          dev->name);
  }

  uint8_t *hdr_buf = kmalloc(sector_size);
  if (!hdr_buf)
    return -1;

  struct gpt_header active_hdr;
  uint8_t *entries = NULL;
  bool is_gpt = false;
  bool primary_corrupt = false;

  // 1. Try reading and validating Primary GPT Header at LBA 1
  if (dev->read_sectors(dev, 1, 1, hdr_buf) == 0) {
    struct gpt_header *prim = (struct gpt_header *)hdr_buf;
    if (memcmp(prim->signature, GPT_SIGNATURE, 8) == 0) {
      is_gpt = true;
      if (gpt_validate_header(prim, 1, dev->total_sectors, sector_size)) {
        entries = gpt_read_entries(dev, prim, sector_size);
        if (entries) {
          memcpy(&active_hdr, prim, sizeof(active_hdr));
        } else {
          primary_corrupt = true;
        }
      } else {
        primary_corrupt = true;
      }
    }
  }

  // 2. If Primary is corrupted or missing, try Backup GPT Header at last sector
  if (!entries) {
    uint64_t backup_lba = dev->total_sectors - 1;
    if (dev->read_sectors(dev, backup_lba, 1, hdr_buf) == 0) {
      struct gpt_header *backup = (struct gpt_header *)hdr_buf;
      if (memcmp(backup->signature, GPT_SIGNATURE, 8) == 0) {
        is_gpt = true;
        if (gpt_validate_header(backup, backup_lba, dev->total_sectors,
                                sector_size)) {
          entries = gpt_read_entries(dev, backup, sector_size);
          if (entries) {
            memcpy(&active_hdr, backup, sizeof(active_hdr));
            if (primary_corrupt) {
              klog_puts("[WARN] Primary GPT corrupted. Recovered partition table "
                        "from Backup GPT!\n");
            }
          }
        }
      }
    }
  }

  kfree(hdr_buf);

  // If not a GPT disk or both headers corrupted, return 0 to allow fallback to MBR
  if (!entries) {
    if (is_gpt) {
      klog_puts("[FAIL] Both primary and backup GPT tables are corrupted!\n");
    }
    return 0;
  }

  // 3. Process partition entries
  struct partition_range ranges[GPT_MAX_PARTITIONS];
  int range_count = 0;
  int count = 0;

  for (uint32_t i = 0; i < active_hdr.num_partition_entries; i++) {
    struct gpt_entry *entry = (struct gpt_entry *)(entries +
                                                   (i * active_hdr.partition_entry_size));

    if (gpt_guid_is_zero(entry->type_guid))
      continue;

    // Boundary checks
    if (entry->starting_lba < active_hdr.first_usable_lba ||
        entry->ending_lba > active_hdr.last_usable_lba ||
        entry->starting_lba > entry->ending_lba) {
      continue;
    }

    // Overlap checks against existing accepted partitions
    bool overlap = false;
    for (int r = 0; r < range_count; r++) {
      if (!(entry->ending_lba < ranges[r].start ||
            entry->starting_lba > ranges[r].end)) {
        overlap = true;
        break;
      }
    }
    if (overlap) {
      klog_puts("[WARN] GPT partition overlap detected, skipping entry\n");
      continue;
    }

    uint64_t total_sectors = (entry->ending_lba - entry->starting_lba) + 1;

    char partuuid[37];
    char partlabel[64];
    gpt_guid_to_str(entry->unique_guid, partuuid, sizeof(partuuid));
    gpt_utf16le_to_utf8(entry->name, 36, partlabel, sizeof(partlabel));
    uint8_t ptype = gpt_classify_type_guid(entry->type_guid);

    struct partition_meta meta = {
        .partuuid = partuuid,
        .partlabel = partlabel,
        .type_guid = entry->type_guid,
        .partition_type = ptype,
    };

    if (block_add_partition_ex(dev, i + 1, entry->starting_lba, total_sectors,
                               &meta) == 0) {
      if (range_count < GPT_MAX_PARTITIONS) {
        ranges[range_count].start = entry->starting_lba;
        ranges[range_count].end = entry->ending_lba;
        range_count++;
      }
      klogf("[  OK  ] %s: GPT part %u: \"%s\" [PARTUUID=%s] (%s)\n",
            dev->name, i + 1, partlabel[0] ? partlabel : "(unlabeled)",
            partuuid, gpt_partition_type_str(ptype));
      count++;
    }
  }

  kfree(entries);
  return count;
}
