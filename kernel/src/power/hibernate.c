#include "hibernate.h"
#include "suspend.h"

#include "../acpi/acpi.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../console/console.h"
#include "../drivers/manager/device.h"
#include "../drivers/pci/pci.h"
#include "../drivers/storage/block.h"
#include "../drivers/storage/nvme.h"
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/pmm_internal.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"

extern const char *kernel_boot_cmdline;
extern void sys_sync_all_open_files(void);
extern int power_s4_enter_arch(void);
extern const uint8_t hibernate_reloc_trampoline_start[];
extern const uint8_t hibernate_reloc_trampoline_end[];
extern void hibernate_switch_ctx_stub(void);

#define HIBERNATE_MAX_SECTOR_SIZE 4096u
#define HIBERNATE_AP_TIMEOUT_MS   1000u
#define HIBERNATE_IO_CHUNK_PAGES  128u

static struct block_device *s4_active_target;
static void *s4_extents_phys;
static size_t s4_extents_alloc_pages;
static void *s4_bounce_phys;

uint32_t hibernate_crc32_update(uint32_t crc, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc;
}

uint32_t hibernate_crc32(const void *data, size_t len) {
  return ~hibernate_crc32_update(0xFFFFFFFFu, data, len);
}

static bool hibernate_extract_resume_arg(char *out, size_t out_cap) {
  if (!kernel_boot_cmdline || !out || out_cap == 0)
    return false;

  const char *p = kernel_boot_cmdline;
  while (*p) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;

    const char *start = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;

    size_t tok_len = (size_t)(p - start);
    if (tok_len > 7 && strncmp(start, "resume=", 7) == 0) {
      size_t val_len = tok_len - 7;
      if (val_len >= out_cap)
        val_len = out_cap - 1;
      memcpy(out, start + 7, val_len);
      out[val_len] = '\0';
      return val_len > 0;
    }
  }
  return false;
}

/* Returns a block reference that the S4 paths never drop: the image target
 * stays pinned, which matters only if it is a removable disk (its struct then
 * outlives an unplug instead of being freed under a resume in progress). */
struct block_device *hibernate_find_target(void) {
  char spec[128];
  if (hibernate_extract_resume_arg(spec, sizeof(spec))) {
    if (strncmp(spec, "PARTUUID=", 9) == 0 || strncmp(spec, "partuuid:", 9) == 0)
      return block_find_by_partuuid(spec + 9);
    if (strncmp(spec, "PARTLABEL=", 10) == 0 ||
        strncmp(spec, "partlabel:", 10) == 0)
      return block_find_by_partlabel(spec + 10);
    return block_get_by_name(spec);
  }

  return block_find_by_type(PART_TYPE_LINUX_SWAP);
}

static uint32_t hibernate_header_crc(const struct hibernate_header *hdr) {
  struct hibernate_header copy = *hdr;
  copy.header_crc32 = 0;
  return hibernate_crc32(&copy, sizeof(copy));
}

bool hibernate_header_valid(const struct hibernate_header *hdr,
                            uint64_t dev_total_sectors, uint32_t sector_size) {
  if (!hdr || sector_size < sizeof(*hdr) ||
      sector_size > HIBERNATE_MAX_SECTOR_SIZE ||
      (sector_size & (sector_size - 1)) != 0)
    return false;
  if (hdr->magic != HIBERNATE_MAGIC || hdr->version != HIBERNATE_VERSION)
    return false;
  if (hdr->page_size != HIBERNATE_PAGE_SIZE)
    return false;
  if (hdr->header_crc32 != hibernate_header_crc(hdr))
    return false;
  if (hdr->total_pages == 0 || hdr->extent_count == 0)
    return false;
  if (hdr->extent_table_sector == 0 || hdr->extent_table_sectors == 0)
    return false;
  if (hdr->extent_table_sector >= dev_total_sectors ||
      hdr->extent_table_sectors > dev_total_sectors - hdr->extent_table_sector)
    return false;

  uint64_t pages_sectors =
      (hdr->total_pages * (uint64_t)hdr->page_size + sector_size - 1) /
      sector_size;
  uint64_t needed = hdr->extent_table_sector + hdr->extent_table_sectors;
  if (pages_sectors > dev_total_sectors ||
      needed > dev_total_sectors - pages_sectors)
    return false;

  return true;
}

int hibernate_read_header(struct block_device *dev,
                          struct hibernate_header *out) {
  if (!dev || !out || !dev->read_sectors)
    return -22;

  uint32_t ssz = dev->sector_size ? dev->sector_size : BLOCK_SECTOR_SIZE;
  if (ssz < sizeof(*out) || ssz > HIBERNATE_MAX_SECTOR_SIZE)
    return -22;

  uint8_t sector_buf[HIBERNATE_MAX_SECTOR_SIZE];
  if (dev->read_sectors(dev, 0, 1, sector_buf) != 0)
    return -5;

  memcpy(out, sector_buf, sizeof(*out));
  return 0;
}

int hibernate_write_header(struct block_device *dev,
                           struct hibernate_header *hdr) {
  if (!dev || !hdr)
    return -22;

  uint32_t ssz = dev->sector_size ? dev->sector_size : BLOCK_SECTOR_SIZE;
  if (ssz < sizeof(*hdr) || ssz > HIBERNATE_MAX_SECTOR_SIZE)
    return -22;

  hdr->magic = HIBERNATE_MAGIC;
  hdr->version = HIBERNATE_VERSION;
  hdr->page_size = HIBERNATE_PAGE_SIZE;
  hdr->header_crc32 = hibernate_header_crc(hdr);

  uint8_t sector_buf[HIBERNATE_MAX_SECTOR_SIZE];
  memset(sector_buf, 0, ssz);
  memcpy(sector_buf, hdr, sizeof(*hdr));

  if (block_write_fua(dev, 0, 1, sector_buf) != 0)
    return -5;
  return 0;
}

int hibernate_invalidate_header(struct block_device *dev) {
  if (!dev)
    return -22;

  uint32_t ssz = dev->sector_size ? dev->sector_size : BLOCK_SECTOR_SIZE;
  if (ssz < sizeof(struct hibernate_header) || ssz > HIBERNATE_MAX_SECTOR_SIZE)
    return -22;

  uint8_t sector_buf[HIBERNATE_MAX_SECTOR_SIZE];
  memset(sector_buf, 0, ssz);
  if (block_write_fua(dev, 0, 1, sector_buf) != 0) {
    klog_puts("[S4] Warning: failed to invalidate hibernation header.\n");
    return -5;
  }
  return 0;
}

static void hibernate_sort_extents(struct hibernate_extent *extents,
                                   size_t count) {
  for (size_t i = 1; i < count; i++) {
    struct hibernate_extent key = extents[i];
    size_t j = i;
    while (j > 0 && extents[j - 1].phys_addr > key.phys_addr) {
      extents[j] = extents[j - 1];
      j--;
    }
    extents[j] = key;
  }
}

static size_t hibernate_scan_pmm_extents(struct hibernate_extent *out,
                                         size_t cap, uint64_t *out_total_pages) {
  size_t count = 0;
  uint64_t total = 0;

  /* Include kernel/module memory (LIMINE_MEMMAP_EXECUTABLE_AND_MODULES), which
   * holds .data/.bss globals and is outside pmm_managed_bitmap. */
  for (size_t i = 0; i < pmm_kernel_range_count; i++) {
    uint64_t base = pmm_kernel_ranges[i].base;
    uint64_t len = pmm_kernel_ranges[i].length;
    if (len == 0)
      continue;
    uint64_t start = base & ~(uint64_t)(HIBERNATE_PAGE_SIZE - 1);
    uint64_t end = (base + len + HIBERNATE_PAGE_SIZE - 1) &
                   ~(uint64_t)(HIBERNATE_PAGE_SIZE - 1);
    uint64_t pages = (end - start) / HIBERNATE_PAGE_SIZE;
    if (pages == 0)
      continue;
    if (out && count < cap) {
      out[count].phys_addr = start;
      out[count].page_count = pages;
      out[count].disk_sector = 0;
    }
    count++;
    total += pages;
  }

  bool in_run = false;
  uint64_t run_start_pfn = 0;
  uint64_t run_pages = 0;

  for (uint64_t pfn = pmm_lowest_page; pfn < pmm_highest_page; pfn++) {
    size_t bit = (size_t)(pfn - pmm_lowest_page);
    bool used = pmm_bitmap_test(pmm_managed_bitmap, bit) &&
                pmm_bitmap_test(pmm_bitmap, bit);
    if (used) {
      if (!in_run) {
        in_run = true;
        run_start_pfn = pfn;
        run_pages = 1;
      } else {
        run_pages++;
      }
      total++;
    } else if (in_run) {
      if (out && count < cap) {
        out[count].phys_addr = run_start_pfn * HIBERNATE_PAGE_SIZE;
        out[count].page_count = run_pages;
        out[count].disk_sector = 0;
      }
      count++;
      in_run = false;
    }
  }

  if (in_run) {
    if (out && count < cap) {
      out[count].phys_addr = run_start_pfn * HIBERNATE_PAGE_SIZE;
      out[count].page_count = run_pages;
      out[count].disk_sector = 0;
    }
    count++;
  }

  if (out && count <= cap)
    hibernate_sort_extents(out, count);

  if (out_total_pages)
    *out_total_pages = total;
  return count;
}

static int hibernate_write_bytes(struct block_device *dev, uint64_t start_sector,
                                 const void *buf, size_t bytes,
                                 uint32_t sector_size) {
  const uint8_t *p = (const uint8_t *)buf;
  uint64_t full_sectors = bytes / sector_size;
  size_t rem = bytes % sector_size;

  if (full_sectors > 0) {
    if (dev->write_sectors(dev, start_sector, (uint32_t)full_sectors, p) != 0)
      return -5;
    p += full_sectors * sector_size;
    start_sector += full_sectors;
  }

  if (rem > 0) {
    uint8_t tail[HIBERNATE_MAX_SECTOR_SIZE];
    memset(tail, 0, sector_size);
    memcpy(tail, p, rem);
    if (dev->write_sectors(dev, start_sector, 1, tail) != 0)
      return -5;
  }
  return 0;
}

static void hibernate_capture_cpu_state(struct hibernate_cpu_state *cpu,
                                        uint64_t resume_rsp,
                                        uint64_t resume_rip) {
  const uint64_t *stack = (const uint64_t *)resume_rsp;
  uint64_t cr3 = 0;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

  cpu->cr3 = cr3;
  cpu->rsp = resume_rsp;
  cpu->rip = resume_rip;
  cpu->r15 = stack[0];
  cpu->r14 = stack[1];
  cpu->r13 = stack[2];
  cpu->r12 = stack[3];
  cpu->rbp = stack[4];
  cpu->rbx = stack[5];
  cpu->rflags = stack[6];
}

int power_s4_snapshot_and_sleep(uint64_t resume_rsp, uint64_t resume_rip) {
  if (!resume_rsp || !resume_rip)
    return -22;

  struct block_device *dev = hibernate_find_target();
  if (!dev || !dev->write_sectors || !dev->read_sectors) {
    klog_puts("[S4] Refusing hibernate: no valid resume block target.\n");
    return -19;
  }
  if (dev->partition_type == PART_TYPE_LINUX_ROOT ||
      dev->partition_type == PART_TYPE_ESP) {
    klog_puts("[S4] Refusing hibernate: target is root/ESP partition.\n");
    return -22;
  }

  uint32_t ssz = dev->sector_size ? dev->sector_size : BLOCK_SECTOR_SIZE;
  if (ssz < sizeof(struct hibernate_header) || ssz > HIBERNATE_MAX_SECTOR_SIZE ||
      (HIBERNATE_PAGE_SIZE % ssz) != 0) {
    klog_puts("[S4] Refusing hibernate: unsupported target sector size.\n");
    return -22;
  }

  sys_sync_all_open_files();

  if (!cpu_quiesce_aps(HIBERNATE_AP_TIMEOUT_MS)) {
    klog_puts("[S4] Refusing hibernate: an application processor is busy.\n");
    return -16;
  }

  if (!sched_power_freeze_current()) {
    cpu_release_aps();
    return -16;
  }

  pmm_pcp_drain_all();
  pmm_zero_pool_drain_all();
  cpu_save_s3_context();
  pci_save_platform_s3_state();
  lapic_timer_save_s4_state();

  /* Keep NVMe active for the image write while recording pm_suspended=true
   * in the saved image; dm_suspend_devices() suspends all other devices. */
  nvme_s4_pre_snapshot();
  int dev_res = dm_suspend_devices();
  if (dev_res != 0) {
    sched_power_thaw_current();
    cpu_release_aps();
    return dev_res;
  }

  /* Quiesce BSP interrupts and LAPIC timer before scanning/snapshotting memory
   * so timer ticks (sched_tick / eevfd) cannot mutate pages mid-snapshot. */
  hal_irq_disable();
  lapic_timer_suspend_for_s3();

  /* Allocate extent table and bounce buffer BEFORE scanning PMM so no PMM
   * allocations/frees occur after hibernate_scan_pmm_extents(). */
  size_t est_extents = hibernate_scan_pmm_extents(NULL, 0, NULL);
  size_t ext_cap = est_extents + 16;
  size_t ext_bytes = ext_cap * sizeof(struct hibernate_extent);
  size_t ext_alloc_pages =
      (ext_bytes + HIBERNATE_PAGE_SIZE - 1) / HIBERNATE_PAGE_SIZE;
  if (ext_alloc_pages == 0)
    ext_alloc_pages = 1;

  void *ext_phys = pmm_alloc_pages(ext_alloc_pages);
  void *bounce_phys = pmm_alloc_pages(HIBERNATE_IO_CHUNK_PAGES);
  if (!ext_phys || !bounce_phys) {
    if (ext_phys)
      pmm_free_pages(ext_phys, ext_alloc_pages);
    if (bounce_phys)
      pmm_free_pages(bounce_phys, HIBERNATE_IO_CHUNK_PAGES);
    lapic_timer_resume_from_s3(false);
    hal_irq_enable();
    (void)dm_resume_devices();
    sched_power_thaw_current();
    cpu_release_aps();
    return -12;
  }

  s4_active_target = dev;
  s4_extents_phys = ext_phys;
  s4_extents_alloc_pages = ext_alloc_pages;
  s4_bounce_phys = bounce_phys;

  uint64_t hhdm = pmm_get_hhdm_offset();
  struct hibernate_extent *extents =
      (struct hibernate_extent *)((uint64_t)ext_phys + hhdm);
  void *bounce_virt = (void *)((uint64_t)bounce_phys + hhdm);

  uint64_t total_pages = 0;
  size_t extent_count = hibernate_scan_pmm_extents(extents, ext_cap, &total_pages);
  int fail_rc = -12;
  if (extent_count == 0 || extent_count > ext_cap)
    goto fail_resume;

  uint64_t extent_table_bytes =
      (uint64_t)extent_count * sizeof(struct hibernate_extent);
  uint64_t extent_table_sectors = (extent_table_bytes + ssz - 1) / ssz;
  uint32_t sectors_per_page = HIBERNATE_PAGE_SIZE / ssz;
  uint64_t data_sectors = total_pages * sectors_per_page;
  uint64_t total_sectors_needed = 1 + extent_table_sectors + data_sectors;

  if (total_sectors_needed > dev->total_sectors) {
    klog_puts("[S4] Refusing hibernate: target device is too small.\n");
    fail_rc = -28;
    goto fail_resume;
  }

  uint64_t cur_sector = 1 + extent_table_sectors;
  for (size_t i = 0; i < extent_count; i++) {
    extents[i].disk_sector = cur_sector;
    cur_sector += extents[i].page_count * sectors_per_page;
  }

  /* Write extent table and page payload, accumulating data CRC32. */
  uint32_t data_crc = 0xFFFFFFFFu;
  data_crc = hibernate_crc32_update(data_crc, extents, extent_table_bytes);
  if (hibernate_write_bytes(dev, 1, extents, extent_table_bytes, ssz) != 0) {
    fail_rc = -5;
    goto fail_resume;
  }

  for (size_t i = 0; i < extent_count; i++) {
    uint64_t remaining = extents[i].page_count;
    uint64_t phys = extents[i].phys_addr;
    uint64_t lba = extents[i].disk_sector;

    while (remaining > 0) {
      uint64_t chunk =
          remaining > HIBERNATE_IO_CHUNK_PAGES ? HIBERNATE_IO_CHUNK_PAGES
                                               : remaining;
      const void *src = (const void *)(phys + hhdm);
      size_t chunk_bytes = (size_t)(chunk * HIBERNATE_PAGE_SIZE);
      memcpy(bounce_virt, src, chunk_bytes);
      data_crc = hibernate_crc32_update(data_crc, bounce_virt, chunk_bytes);

      if (dev->write_sectors(dev, lba, (uint32_t)(chunk * sectors_per_page),
                             bounce_virt) != 0) {
        fail_rc = -5;
        goto fail_resume;
      }

      phys += chunk * HIBERNATE_PAGE_SIZE;
      lba += chunk * sectors_per_page;
      remaining -= chunk;
    }
  }

  struct hibernate_header hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.extent_count = (uint32_t)extent_count;
  hdr.total_pages = total_pages;
  hdr.extent_table_sector = 1;
  hdr.extent_table_sectors = extent_table_sectors;
  hdr.data_crc32 = ~data_crc;
  hibernate_capture_cpu_state(&hdr.cpu, resume_rsp, resume_rip);

  if (hibernate_write_header(dev, &hdr) != 0) {
    (void)hibernate_invalidate_header(dev);
    fail_rc = -5;
    goto fail_resume;
  }

  (void)block_flush(dev);
  nvme_s4_post_snapshot_quiesce();

  if (acpi_s4_supported()) {
    (void)acpi_enter_s4();
  } else {
    klog_puts("[S4] ACPI _S4_ unavailable; falling back to S5 poweroff.\n");
    acpi_poweroff();
  }

  /* Only reached if SLP_EN failed without powering off. */
  (void)hibernate_invalidate_header(dev);
  fail_rc = -5;

fail_resume:
  s4_active_target = NULL;
  s4_extents_phys = NULL;
  s4_extents_alloc_pages = 0;
  s4_bounce_phys = NULL;
  lapic_timer_resume_from_s3(false);
  hal_irq_enable();
  (void)dm_resume_devices();
  cpu_release_aps();
  sched_power_thaw_current();
  pmm_free_pages(bounce_phys, HIBERNATE_IO_CHUNK_PAGES);
  pmm_free_pages(ext_phys, ext_alloc_pages);
  return fail_rc;
}

int power_s4_resume(void) {
  power_s3_restore_bsp_early();
  pmm_rebuild_free_lists_from_bitmap();
  hal_irq_enable();
  cpu_resume_aps();
  sched_resume_from_s3();

  int result = dm_resume_devices();
  if (s4_active_target) {
    (void)hibernate_invalidate_header(s4_active_target);
    s4_active_target = NULL;
  }
  if (s4_bounce_phys) {
    pmm_free_pages(s4_bounce_phys, HIBERNATE_IO_CHUNK_PAGES);
    s4_bounce_phys = NULL;
  }
  if (s4_extents_phys) {
    pmm_free_pages(s4_extents_phys, s4_extents_alloc_pages);
    s4_extents_phys = NULL;
    s4_extents_alloc_pages = 0;
  }
  sched_power_thaw_current();
  console_clear();
  return result;
}

int power_s4_enter(void) { return power_s4_enter_arch(); }

struct hibernate_copy_op {
  uint64_t src_phys;
  uint64_t dst_phys;
};

#define HIBERNATE_OPS_PER_PAGE 255u

struct hibernate_ops_page {
  struct hibernate_copy_op entries[HIBERNATE_OPS_PER_PAGE];
  uint32_t count;
  uint32_t pad;
  uint64_t next_phys;
};

_Static_assert(sizeof(struct hibernate_ops_page) == 4096,
               "hibernate_ops_page must be 4096 bytes");

typedef void (*hibernate_trampoline_fn)(uint64_t first_ops_phys,
                                        uint64_t unused, uint64_t hhdm,
                                        uint64_t saved_cr3, uint64_t saved_rsp,
                                        uint64_t saved_rip,
                                        uint64_t switch_stub);

static bool hibernate_cmdline_has_flag(const char *flag) {
  if (!kernel_boot_cmdline || !flag)
    return false;
  size_t flen = strlen(flag);
  const char *p = kernel_boot_cmdline;
  while (*p) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if ((size_t)(p - start) == flen && strncmp(start, flag, flen) == 0)
      return true;
  }
  return false;
}

static bool hibernate_phys_in_extents(uint64_t phys,
                                      const struct hibernate_extent *extents,
                                      size_t extent_count) {
  size_t lo = 0;
  size_t hi = extent_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    uint64_t start = extents[mid].phys_addr;
    uint64_t end = start + extents[mid].page_count * HIBERNATE_PAGE_SIZE;
    if (phys < start) {
      hi = mid;
    } else if (phys >= end) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

static void *hibernate_alloc_disjoint_pages(size_t num_pages,
                                            const struct hibernate_extent *ext,
                                            size_t ext_count);

struct hibernate_ops_list {
  uint64_t head_phys;
  uint64_t tail_phys;
  struct hibernate_ops_page *tail_virt;
  uint64_t hhdm;
  const struct hibernate_extent *extents;
  size_t extent_count;
};

static bool hibernate_emit_op(struct hibernate_ops_list *list, uint64_t src_phys,
                              uint64_t dst_phys) {
  if (!list->tail_virt || list->tail_virt->count >= HIBERNATE_OPS_PER_PAGE) {
    void *page_phys =
        hibernate_alloc_disjoint_pages(1, list->extents, list->extent_count);
    if (!page_phys)
      return false;

    struct hibernate_ops_page *page_virt =
        (struct hibernate_ops_page *)((uint64_t)page_phys + list->hhdm);
    page_virt->count = 0;
    page_virt->pad = 0;
    page_virt->next_phys = 0;

    if (list->tail_virt)
      list->tail_virt->next_phys = (uint64_t)page_phys;
    else
      list->head_phys = (uint64_t)page_phys;

    list->tail_phys = (uint64_t)page_phys;
    list->tail_virt = page_virt;
  }

  uint32_t idx = list->tail_virt->count++;
  list->tail_virt->entries[idx].src_phys = src_phys;
  list->tail_virt->entries[idx].dst_phys = dst_phys;
  return true;
}

static void hibernate_free_ops_list(uint64_t head_phys, uint64_t hhdm) {
  uint64_t cur = head_phys;
  while (cur) {
    struct hibernate_ops_page *p = (struct hibernate_ops_page *)(cur + hhdm);
    uint64_t next = p->next_phys;
    pmm_free_page((void *)cur);
    cur = next;
  }
}
/* Allocate `num_pages` contiguous physical pages that do not overlap any
 * destination page in `extents`. Holds colliding allocations temporarily
 * and releases them once a disjoint block is found. */
static void *hibernate_alloc_disjoint_pages(size_t num_pages,
                                            const struct hibernate_extent *ext,
                                            size_t ext_count) {
  void *held[64];
  size_t held_count = 0;
  void *found = NULL;

  for (int attempt = 0; attempt < 256; attempt++) {
    void *cand = pmm_alloc_pages(num_pages);
    if (!cand)
      break;

    bool overlaps = false;
    for (size_t p = 0; p < num_pages; p++) {
      uint64_t pa = (uint64_t)cand + p * HIBERNATE_PAGE_SIZE;
      if (hibernate_phys_in_extents(pa, ext, ext_count)) {
        overlaps = true;
        break;
      }
    }

    if (!overlaps) {
      found = cand;
      break;
    }

    if (held_count < 64) {
      held[held_count++] = cand;
    } else {
      pmm_free_pages(cand, num_pages);
      break;
    }
  }

  for (size_t i = 0; i < held_count; i++)
    pmm_free_pages(held[i], num_pages);

  return found;
}

struct hibernate_src_slot {
  uint64_t phys;
  uint32_t idx;
  uint32_t used;
};

static uint64_t hibernate_hash_u64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

static int64_t hibernate_src_lookup(const struct hibernate_src_slot *ht,
                                    size_t ht_mask, uint64_t phys) {
  size_t pos = (size_t)hibernate_hash_u64(phys) & ht_mask;
  while (ht[pos].used) {
    if (ht[pos].phys == phys)
      return (int64_t)ht[pos].idx;
    pos = (pos + 1) & ht_mask;
  }
  return -1;
}

static int hibernate_read_bytes(struct block_device *dev, uint64_t start_sector,
                                void *buf, size_t bytes, uint32_t sector_size) {
  uint8_t *p = (uint8_t *)buf;
  uint64_t full_sectors = bytes / sector_size;
  size_t rem = bytes % sector_size;

  if (full_sectors > 0) {
    if (dev->read_sectors(dev, start_sector, (uint32_t)full_sectors, p) != 0)
      return -5;
    p += full_sectors * sector_size;
    start_sector += full_sectors;
  }

  if (rem > 0) {
    uint8_t tail[HIBERNATE_MAX_SECTOR_SIZE];
    if (dev->read_sectors(dev, start_sector, 1, tail) != 0)
      return -5;
    memcpy(p, tail, rem);
  }
  return 0;
}

void power_s4_try_resume(void) {
  if (hibernate_cmdline_has_flag("noresume")) {
    klog_puts("[S4] 'noresume' set; skipping hibernation resume.\n");
    return;
  }

  struct block_device *dev = hibernate_find_target();
  if (!dev || !dev->read_sectors)
    return;

  uint32_t ssz = dev->sector_size ? dev->sector_size : BLOCK_SECTOR_SIZE;
  if (ssz < sizeof(struct hibernate_header) || ssz > HIBERNATE_MAX_SECTOR_SIZE ||
      (HIBERNATE_PAGE_SIZE % ssz) != 0)
    return;

  struct hibernate_header hdr;
  if (hibernate_read_header(dev, &hdr) != 0)
    return;
  if (!hibernate_header_valid(&hdr, dev->total_sectors, ssz))
    return;

  klog_puts("[S4] Valid hibernation image detected; loading...\n");

  uint64_t hhdm = pmm_get_hhdm_offset();
  uint32_t sectors_per_page = HIBERNATE_PAGE_SIZE / ssz;

  size_t ext_bytes = (size_t)hdr.extent_count * sizeof(struct hibernate_extent);
  size_t ext_alloc_pages =
      (ext_bytes + HIBERNATE_PAGE_SIZE - 1) / HIBERNATE_PAGE_SIZE;
  void *ext_phys = pmm_alloc_pages(ext_alloc_pages);
  if (!ext_phys)
    return;
  struct hibernate_extent *extents =
      (struct hibernate_extent *)((uint64_t)ext_phys + hhdm);

  if (hibernate_read_bytes(dev, hdr.extent_table_sector, extents, ext_bytes,
                           ssz) != 0) {
    klog_puts("[S4] Failed to read extent table.\n");
    pmm_free_pages(ext_phys, ext_alloc_pages);
    return;
  }

  size_t n = (size_t)hdr.total_pages;
  size_t src_arr_pages =
      (n * sizeof(uint64_t) + HIBERNATE_PAGE_SIZE - 1) / HIBERNATE_PAGE_SIZE;
  size_t dst_arr_pages = src_arr_pages;
  void *src_arr_phys = pmm_alloc_pages(src_arr_pages);
  void *dst_arr_phys = pmm_alloc_pages(dst_arr_pages);
  if (!src_arr_phys || !dst_arr_phys) {
    if (src_arr_phys)
      pmm_free_pages(src_arr_phys, src_arr_pages);
    if (dst_arr_phys)
      pmm_free_pages(dst_arr_phys, dst_arr_pages);
    pmm_free_pages(ext_phys, ext_alloc_pages);
    return;
  }

  uint64_t *src_pages = (uint64_t *)((uint64_t)src_arr_phys + hhdm);
  uint64_t *dst_pages = (uint64_t *)((uint64_t)dst_arr_phys + hhdm);

  uint32_t data_crc = 0xFFFFFFFFu;
  data_crc = hibernate_crc32_update(data_crc, extents, ext_bytes);

  size_t loaded = 0;
  bool read_ok = true;
  for (uint32_t i = 0; i < hdr.extent_count && read_ok; i++) {
    uint64_t phys = extents[i].phys_addr;
    uint64_t lba = extents[i].disk_sector;
    for (uint64_t p = 0; p < extents[i].page_count; p++) {
      if (loaded >= n) {
        read_ok = false;
        break;
      }
      void *tmp_phys = pmm_alloc_page();
      if (!tmp_phys) {
        read_ok = false;
        break;
      }
      void *tmp_virt = (void *)((uint64_t)tmp_phys + hhdm);
      if (dev->read_sectors(dev, lba, sectors_per_page, tmp_virt) != 0) {
        pmm_free_page(tmp_phys);
        read_ok = false;
        break;
      }
      data_crc =
          hibernate_crc32_update(data_crc, tmp_virt, HIBERNATE_PAGE_SIZE);
      src_pages[loaded] = (uint64_t)tmp_phys;
      dst_pages[loaded] = phys;
      loaded++;
      phys += HIBERNATE_PAGE_SIZE;
      lba += sectors_per_page;
    }
  }

  if (!read_ok || loaded != n || (~data_crc) != hdr.data_crc32) {
    if (!read_ok)
      klog_puts("[S4] Image read failed; aborting resume.\n");
    else if (loaded != n)
      klog_puts("[S4] Image page count mismatch; aborting resume.\n");
    else
      klog_puts("[S4] Image CRC32 verification failed; aborting resume.\n");
    for (size_t i = 0; i < loaded; i++)
      pmm_free_page((void *)src_pages[i]);
    pmm_free_pages(dst_arr_phys, dst_arr_pages);
    pmm_free_pages(src_arr_phys, src_arr_pages);
    pmm_free_pages(ext_phys, ext_alloc_pages);
    (void)hibernate_invalidate_header(dev);
    return;
  }

  /* Build conflict-free relocation plan in disjoint 1-page blocks. */
  size_t ht_cap = 16;
  while (ht_cap < n * 2)
    ht_cap <<= 1;
  size_t ht_pages = (ht_cap * sizeof(struct hibernate_src_slot) +
                     HIBERNATE_PAGE_SIZE - 1) /
                    HIBERNATE_PAGE_SIZE;
  size_t state_pages = (n + HIBERNATE_PAGE_SIZE - 1) / HIBERNATE_PAGE_SIZE;
  size_t chain_pages =
      (n * sizeof(uint32_t) + HIBERNATE_PAGE_SIZE - 1) / HIBERNATE_PAGE_SIZE;

  void *ht_phys = pmm_alloc_pages(ht_pages);
  void *state_phys = pmm_alloc_pages(state_pages);
  void *chain_phys = pmm_alloc_pages(chain_pages);
  void *scratch_phys =
      hibernate_alloc_disjoint_pages(1, extents, hdr.extent_count);
  void *tramp_phys =
      hibernate_alloc_disjoint_pages(1, extents, hdr.extent_count);

  if (!ht_phys || !state_phys || !chain_phys || !scratch_phys || !tramp_phys) {
    klog_puts("[S4] Failed to allocate relocation buffers; aborting resume.\n");
    if (ht_phys)
      pmm_free_pages(ht_phys, ht_pages);
    if (state_phys)
      pmm_free_pages(state_phys, state_pages);
    if (chain_phys)
      pmm_free_pages(chain_phys, chain_pages);
    if (scratch_phys)
      pmm_free_page(scratch_phys);
    if (tramp_phys)
      pmm_free_page(tramp_phys);
    for (size_t i = 0; i < loaded; i++)
      pmm_free_page((void *)src_pages[i]);
    pmm_free_pages(dst_arr_phys, dst_arr_pages);
    pmm_free_pages(src_arr_phys, src_arr_pages);
    pmm_free_pages(ext_phys, ext_alloc_pages);
    return;
  }

  struct hibernate_src_slot *ht =
      (struct hibernate_src_slot *)((uint64_t)ht_phys + hhdm);
  memset(ht, 0, ht_cap * sizeof(*ht));
  size_t ht_mask = ht_cap - 1;
  for (size_t i = 0; i < n; i++) {
    size_t pos = (size_t)hibernate_hash_u64(src_pages[i]) & ht_mask;
    while (ht[pos].used)
      pos = (pos + 1) & ht_mask;
    ht[pos].phys = src_pages[i];
    ht[pos].idx = (uint32_t)i;
    ht[pos].used = 1;
  }

  uint8_t *state = (uint8_t *)((uint64_t)state_phys + hhdm);
  memset(state, 0, n);
  uint32_t *chain = (uint32_t *)((uint64_t)chain_phys + hhdm);

  struct hibernate_ops_list ops_list = {
      .head_phys = 0,
      .tail_phys = 0,
      .tail_virt = NULL,
      .hhdm = hhdm,
      .extents = extents,
      .extent_count = hdr.extent_count,
  };
  bool ops_ok = true;

  for (size_t i = 0; i < n && ops_ok; i++) {
    if (state[i] != 0)
      continue;
    if (src_pages[i] == dst_pages[i]) {
      state[i] = 2;
      continue;
    }

    size_t chain_len = 0;
    uint32_t cur = (uint32_t)i;
    bool is_cycle = false;
    size_t cycle_start = 0;
    for (;;) {
      state[cur] = 1;
      chain[chain_len++] = cur;
      int64_t next = hibernate_src_lookup(ht, ht_mask, dst_pages[cur]);
      if (next < 0 || state[next] == 2)
        break;
      if (state[next] == 1) {
        is_cycle = true;
        for (size_t c = 0; c < chain_len; c++) {
          if (chain[c] == (uint32_t)next) {
            cycle_start = c;
            break;
          }
        }
        break;
      }
      cur = (uint32_t)next;
    }

    if (!is_cycle) {
      for (size_t k = chain_len; k > 0 && ops_ok; k--) {
        uint32_t idx = chain[k - 1];
        ops_ok = hibernate_emit_op(&ops_list, src_pages[idx], dst_pages[idx]);
        state[idx] = 2;
      }
    } else {
      /* chain[cycle_start .. chain_len-1] is a cycle; chain[0 .. cycle_start-1]
       * (if cycle_start > 0) is a tail leading into chain[cycle_start]. Break
       * the cycle first so src[chain[cycle_start]] is free before the tail
       * writes to dst[chain[cycle_start - 1]] == src[chain[cycle_start]]. */
      uint32_t cyc_head = chain[cycle_start];
      ops_ok = hibernate_emit_op(&ops_list, src_pages[cyc_head],
                                 (uint64_t)scratch_phys);
      for (size_t k = chain_len; k > cycle_start + 1 && ops_ok; k--) {
        uint32_t idx = chain[k - 1];
        ops_ok = hibernate_emit_op(&ops_list, src_pages[idx], dst_pages[idx]);
      }
      if (ops_ok) {
        ops_ok = hibernate_emit_op(&ops_list, (uint64_t)scratch_phys,
                                   dst_pages[cyc_head]);
      }
      for (size_t k = cycle_start; k > 0 && ops_ok; k--) {
        uint32_t idx = chain[k - 1];
        ops_ok = hibernate_emit_op(&ops_list, src_pages[idx], dst_pages[idx]);
      }
      for (size_t k = 0; k < chain_len; k++)
        state[chain[k]] = 2;
    }
  }

  if (!ops_ok) {
    klog_puts("[S4] Failed to allocate ops page; aborting resume.\n");
    hibernate_free_ops_list(ops_list.head_phys, hhdm);
    pmm_free_pages(ht_phys, ht_pages);
    pmm_free_pages(state_phys, state_pages);
    pmm_free_pages(chain_phys, chain_pages);
    pmm_free_page(scratch_phys);
    pmm_free_page(tramp_phys);
    for (size_t i = 0; i < loaded; i++)
      pmm_free_page((void *)src_pages[i]);
    pmm_free_pages(dst_arr_phys, dst_arr_pages);
    pmm_free_pages(src_arr_phys, src_arr_pages);
    pmm_free_pages(ext_phys, ext_alloc_pages);
    return;
  }

  /* Build a 6-page disjoint temporary PML4 so the copy loop cannot overwrite
   * its own active page tables while copying destination pages. */
  void *pml4_tmp_phys =
      hibernate_alloc_disjoint_pages(1, extents, hdr.extent_count);
  void *pdpt_lo_phys =
      hibernate_alloc_disjoint_pages(1, extents, hdr.extent_count);
  void *pdpt_hhdm_phys =
      hibernate_alloc_disjoint_pages(1, extents, hdr.extent_count);
  void *pdpt_hi_phys =
      hibernate_alloc_disjoint_pages(1, extents, hdr.extent_count);
  void *pd_hi_phys =
      hibernate_alloc_disjoint_pages(1, extents, hdr.extent_count);
  void *pt_hi_phys =
      hibernate_alloc_disjoint_pages(1, extents, hdr.extent_count);

  if (!pml4_tmp_phys || !pdpt_lo_phys || !pdpt_hhdm_phys || !pdpt_hi_phys ||
      !pd_hi_phys || !pt_hi_phys) {
    klog_puts("[S4] Failed to allocate disjoint page tables; aborting resume.\n");
    return;
  }

  uint64_t stub_va = (uint64_t)hibernate_switch_ctx_stub;
  uint64_t stub_page_va = stub_va & ~0xFFFULL;
  uint64_t stub_page_pa =
      vmm_virt_to_phys(vmm_get_active_pml4(), stub_page_va);
  if (!stub_page_pa) {
    klog_puts("[S4] Failed to resolve switch stub physical address.\n");
    return;
  }

  uint64_t *pml4_tmp = (uint64_t *)((uint64_t)pml4_tmp_phys + hhdm);
  uint64_t *pdpt_lo = (uint64_t *)((uint64_t)pdpt_lo_phys + hhdm);
  uint64_t *pdpt_hhdm = (uint64_t *)((uint64_t)pdpt_hhdm_phys + hhdm);
  uint64_t *pdpt_hi = (uint64_t *)((uint64_t)pdpt_hi_phys + hhdm);
  uint64_t *pd_hi = (uint64_t *)((uint64_t)pd_hi_phys + hhdm);
  uint64_t *pt_hi = (uint64_t *)((uint64_t)pt_hi_phys + hhdm);

  memset(pml4_tmp, 0, HIBERNATE_PAGE_SIZE);
  memset(pdpt_hi, 0, HIBERNATE_PAGE_SIZE);
  memset(pd_hi, 0, HIBERNATE_PAGE_SIZE);
  memset(pt_hi, 0, HIBERNATE_PAGE_SIZE);

  for (uint64_t i = 0; i < 512; i++) {
    uint64_t entry =
        (i << 30) | PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_PS;
    pdpt_lo[i] = entry;
    pdpt_hhdm[i] = entry;
  }

  pml4_tmp[0] = (uint64_t)pdpt_lo_phys | PAGE_FLAG_PRESENT | PAGE_FLAG_RW;
  pml4_tmp[(hhdm >> 39) & 0x1FF] =
      (uint64_t)pdpt_hhdm_phys | PAGE_FLAG_PRESENT | PAGE_FLAG_RW;

  uint64_t pml4_idx = (stub_page_va >> 39) & 0x1FF;
  uint64_t pdpt_idx = (stub_page_va >> 30) & 0x1FF;
  uint64_t pd_idx = (stub_page_va >> 21) & 0x1FF;
  uint64_t pt_idx = (stub_page_va >> 12) & 0x1FF;

  pml4_tmp[pml4_idx] = (uint64_t)pdpt_hi_phys | PAGE_FLAG_PRESENT | PAGE_FLAG_RW;
  pdpt_hi[pdpt_idx] = (uint64_t)pd_hi_phys | PAGE_FLAG_PRESENT | PAGE_FLAG_RW;
  pd_hi[pd_idx] = (uint64_t)pt_hi_phys | PAGE_FLAG_PRESENT | PAGE_FLAG_RW;
  pt_hi[pt_idx] = (stub_page_pa & PAGE_MASK) | PAGE_FLAG_PRESENT | PAGE_FLAG_RW;

  /* Copy trampoline machine code to the disjoint page and identity-map it
   * in the active PML4 so the call into tramp_phys can switch CR3 to
   * pml4_tmp_phys safely. */
  size_t tramp_size = (size_t)(hibernate_reloc_trampoline_end -
                               hibernate_reloc_trampoline_start);
  memcpy((void *)((uint64_t)tramp_phys + hhdm),
         hibernate_reloc_trampoline_start, tramp_size);
  (void)vmm_map_page(vmm_get_active_pml4(), (uint64_t)tramp_phys,
                     (uint64_t)tramp_phys, PAGE_FLAG_PRESENT | PAGE_FLAG_RW);

  klog_puts("[S4] Quiescing APs, relocating memory, and jumping to saved context...\n");
  if (!cpu_quiesce_aps(HIBERNATE_AP_TIMEOUT_MS)) {
    klog_puts("[S4] Failed to quiesce APs; aborting resume.\n");
    return;
  }
  hal_irq_disable();
  hibernate_trampoline_fn tramp =
      (hibernate_trampoline_fn)(uint64_t)tramp_phys;
  tramp(ops_list.head_phys, (uint64_t)pml4_tmp_phys, hhdm, hdr.cpu.cr3,
        hdr.cpu.rsp, hdr.cpu.rip, (uint64_t)hibernate_switch_ctx_stub);
}
