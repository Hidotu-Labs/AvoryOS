#include "mm/zram.h"
#include "../sched/capability.h"
#include "../sched/net_namespace.h"
#include "../sched/pid_namespace.h"
#include "inotify_limits.h"
#include "fs/procfs.h"
#include "console/klog.h"
#include "apic/lapic_timer.h"
#include "drivers/storage/block.h"
#include "drivers/gpu/drm/drm.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "drivers/timer/rtc.h"
#include "drivers/usb/usb.h"
#include "cpu/tsc.h"
#include "cpu/isr.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "fs/ext3/ext3.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/numa.h"
#include "mm/mempolicy.h"
#include "mm/vmm.h"
#include "mm/vmm_internal.h"
#include "mm/tlb_shootdown.h"
#include "mm/ksm.h"
#include "sched/sched.h"
#include "sched/namespace.h"
#include "smp/cpu.h"
#include "syscalls/syscall.h"
#include <stdint.h>
#include <linuxkpi/gpu_diag.h>

#define PROC_SUPER_MAGIC 0x9fa0

static int procfs_statfs(vfs_node_t *node, struct statfs_buf *buf) {
  (void)node;
  if (!buf)
    return -1;
  memset(buf, 0, sizeof(*buf));
  buf->f_type = PROC_SUPER_MAGIC;
  buf->f_bsize = 4096;
  buf->f_blocks = 0;
  buf->f_bfree = 0;
  buf->f_bavail = 0;
  buf->f_files = 0;
  buf->f_ffree = 0;
  buf->f_fsid[0] = 0;
  buf->f_fsid[1] = 0;
  buf->f_namelen = 255;
  buf->f_frsize = 4096;
  buf->f_flags = 0;
  return 0;
}

/* Process trees are created on demand without being grafted under the proc
 * root. Set the filesystem handler before ramfs directory construction so
 * every dynamic directory, file and symlink retains procfs identity. */
static void procfs_node_init(vfs_node_t *node) {
  vfs_node_init(node);
  node->statfs = procfs_statfs;
}

uint32_t procfs_meminfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  char buf[1024];

  uint64_t total_kb = pmm_get_usable_memory() / 1024;
  uint64_t free_kb  = (uint64_t)pmm_get_free_pages() * 4096 / 1024;

  extern uint64_t shm_total_pages(void) __attribute__((weak));
  extern uint64_t tmpfs_total_pages(void) __attribute__((weak));
  uint64_t shm_pages = shm_total_pages ? shm_total_pages() : 0;
  uint64_t tmpfs_pages = tmpfs_total_pages ? tmpfs_total_pages() : 0;
  uint64_t shmem_kb = (shm_pages + tmpfs_pages) * 4;

  uint64_t file_cached_kb = (uint64_t)vfs_cache_page_count() * 4096 / 1024;
  uint64_t cached_kb = file_cached_kb + shmem_kb;

  extern uint32_t global_thread_count;
  extern size_t kstack_cache_total_cached(void) __attribute__((weak));
  uint64_t kstack_kb = (uint64_t)global_thread_count * 16;
  if (kstack_cache_total_cached)
    kstack_kb += (uint64_t)kstack_cache_total_cached() * 16;

  extern uint64_t slab_total_pages(void) __attribute__((weak));
  extern uint64_t slab_reclaimable_pages(void) __attribute__((weak));
  uint64_t slab_kb = slab_total_pages ? slab_total_pages() * 4 : 0;
  uint64_t sreclaimable_kb = slab_reclaimable_pages ? slab_reclaimable_pages() * 4 : 0;
  uint64_t sunreclaim_kb = (slab_kb >= sreclaimable_kb) ? (slab_kb - sreclaimable_kb) : 0;

  extern size_t ext2_buffer_cache_total_kb(void) __attribute__((weak));
  uint64_t buffers_kb = ext2_buffer_cache_total_kb ? (uint64_t)ext2_buffer_cache_total_kb() : 0;

  uint64_t available_kb = free_kb + file_cached_kb + sreclaimable_kb + buffers_kb;
  if (available_kb > total_kb)
    available_kb = total_kb;

  uint64_t active_file_kb = file_cached_kb / 2;
  uint64_t inactive_file_kb = file_cached_kb - active_file_kb;

  uint64_t used_approx_kb = (total_kb > free_kb) ? (total_kb - free_kb) : 0;
  uint64_t non_anon_kb = cached_kb + buffers_kb + slab_kb + kstack_kb;
  uint64_t anon_kb = (used_approx_kb > non_anon_kb) ? (used_approx_kb - non_anon_kb) : 0;
  uint64_t active_anon_kb = anon_kb / 2;
  uint64_t inactive_anon_kb = anon_kb - active_anon_kb;

  uint64_t active_kb = active_file_kb + active_anon_kb;
  uint64_t inactive_kb = inactive_file_kb + inactive_anon_kb;

  struct ksm_stats kst;
  ksm_get_stats(&kst);
  uint64_t ksm_shared_kb = kst.pages_shared * 4;
  uint64_t ksm_sharing_kb = (kst.pages_sharing + kst.zero_pages_merged) * 4;

  struct zram_stats zst; zram_get_stats(&zst);
  int len = snprintf(buf, sizeof(buf),
      "MemTotal:       %llu kB\n"
      "MemFree:        %llu kB\n"
      "MemAvailable:   %llu kB\n"
      "Buffers:        %llu kB\n"
      "Cached:         %llu kB\n"
      "SwapCached:            0 kB\n"
      "Active:         %llu kB\n"
      "Inactive:       %llu kB\n"
      "Active(anon):   %llu kB\n"
      "Inactive(anon): %llu kB\n"
      "Active(file):   %llu kB\n"
      "Inactive(file): %llu kB\n"
      "Unevictable:           0 kB\n"
      "Mlocked:               0 kB\n"
      "SwapTotal:      %llu kB\n"
      "SwapFree:       %llu kB\n"
      "Dirty:                 0 kB\n"
      "Writeback:             0 kB\n"
      "AnonPages:      %llu kB\n"
      "Mapped:         %llu kB\n"
      "Shmem:          %llu kB\n"
      "KReclaimable:   %llu kB\n"
      "Slab:           %llu kB\n"
      "SReclaimable:   %llu kB\n"
      "SUnreclaim:     %llu kB\n"
      "KernelStack:    %llu kB\n"
      "PageTables:     %llu kB\n"
      "KsmShared:      %llu kB\n"
      "KsmSharing:     %llu kB\n"
      "MemUsable:      %llu kB\n",
      (unsigned long long)total_kb,
      (unsigned long long)free_kb,
      (unsigned long long)available_kb,
      (unsigned long long)buffers_kb,
      (unsigned long long)cached_kb,
      (unsigned long long)active_kb,
      (unsigned long long)inactive_kb,
      (unsigned long long)active_anon_kb,
      (unsigned long long)inactive_anon_kb,
      (unsigned long long)active_file_kb,
      (unsigned long long)inactive_file_kb,
      (unsigned long long)(zst.enabled ? zst.capacity_pages * 4 : 0),
      (unsigned long long)(zst.enabled ? (zst.capacity_pages - zst.stored_pages) * 4 : 0),
      (unsigned long long)anon_kb,
      (unsigned long long)(file_cached_kb + shmem_kb),
      (unsigned long long)shmem_kb,
      (unsigned long long)sreclaimable_kb,
      (unsigned long long)slab_kb,
      (unsigned long long)sreclaimable_kb,
      (unsigned long long)sunreclaim_kb,
      (unsigned long long)kstack_kb,
      (unsigned long long)(global_thread_count * 16),
      (unsigned long long)ksm_shared_kb,
      (unsigned long long)ksm_sharing_kb,
      (unsigned long long)total_kb);

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

static uint32_t procfs_zram_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
  struct zram_stats z; zram_get_stats(&z);
  char buf[768]; int len;
  if (!strcmp(node->name, "swaps")) {
    len = snprintf(buf, sizeof(buf), "Filename\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
    if (z.enabled) len += snprintf(buf + len, sizeof(buf) - len,
      "/dev/zram0\t\tpartition\t%llu\t\t%llu\t\t100\n",
      (unsigned long long)(z.capacity_pages * 4), (unsigned long long)(z.stored_pages * 4));
  } else {
    len = snprintf(buf, sizeof(buf),
      "enabled %u\ncapacity_pages %llu\nstored_pages %llu\ncompressed_bytes %llu\n"
      "memory_bytes %llu\nmemory_limit %llu\npeak_bytes %llu\nsame_pages %llu\n"
      "rejected_pages %llu\nallocation_failures %llu\nswapouts %llu\nswapins %llu\nscans %llu\npressure_yields %llu\n",
      z.enabled, (unsigned long long)z.capacity_pages, (unsigned long long)z.stored_pages,
      (unsigned long long)z.compressed_bytes, (unsigned long long)z.memory_bytes,
      (unsigned long long)z.memory_limit, (unsigned long long)z.peak_bytes,
      (unsigned long long)z.same_pages, (unsigned long long)z.rejected_pages,
      (unsigned long long)z.allocation_failures, (unsigned long long)z.swapouts,
      (unsigned long long)z.swapins, (unsigned long long)z.scans, (unsigned long long)z.pressure_yields);
  }
  if (offset >= (uint32_t)len) return 0;
  if (size > (uint32_t)len - offset) size = len - offset;
  memcpy(buffer, buf + offset, size); return size;
}

/* /proc/zoneinfo — memory zone watermarks and free-page counts. */
static uint32_t procfs_zoneinfo_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  char buf[2048];
  int len = 0;
  uint32_t node_count = numa_node_count();

  for (uint32_t i = 0; i < node_count && len < (int)sizeof(buf) - 256; i++) {
    struct numa_node *n = numa_get_node(i);
    if (!n->online)
      continue;
    uint64_t free_pages = (uint64_t)buddy_zone_free_pages(&n->zone);
    uint64_t total_pages = n->usable_memory / 4096;
    if (total_pages == 0)
      total_pages = pmm_get_usable_memory() / 4096;

    len += snprintf(buf + len, sizeof(buf) - len,
        "Node %u, zone   Normal\n"
        "  pages free     %llu\n"
        "        min      0\n"
        "        low      0\n"
        "        high     0\n"
        "        spanned  %llu\n"
        "        present  %llu\n"
        "        managed  %llu\n",
        (unsigned)i,
        (unsigned long long)free_pages,
        (unsigned long long)total_pages,
        (unsigned long long)total_pages,
        (unsigned long long)total_pages);
  }

  if (len <= 0)
    return 0;

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

static uint32_t procfs_numainfo_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  char buf[2048];
  int len = 0;
  uint32_t node_count = numa_node_count();

  len += snprintf(buf + len, sizeof(buf) - len,
                  "NUMA nodes: %u\nAvailable: %s\n\n",
                  node_count, numa_is_available() ? "yes" : "no (UMA fallback)");

  for (uint32_t i = 0; i < node_count && len < (int)sizeof(buf) - 256; i++) {
    struct numa_node *n = numa_get_node(i);
    if (!n->online)
      continue;
    uint64_t free_kb = (uint64_t)buddy_zone_free_pages(&n->zone) * 4;
    uint64_t total_kb = n->usable_memory / 1024;
    len += snprintf(buf + len, sizeof(buf) - len,
                    "Node %u MemTotal: %llu kB\n"
                    "Node %u MemFree:  %llu kB\n"
                    "Node %u CPUs:     %u\n"
                    "Node %u CPU Mask: 0x%llx\n",
                    i, (unsigned long long)total_kb,
                    i, (unsigned long long)free_kb,
                    i, (unsigned)n->cpu_count,
                    i, (unsigned long long)n->cpu_mask);
  }

  len += snprintf(buf + len, sizeof(buf) - len, "\nNode Distances:\nnode");
  for (uint32_t i = 0; i < node_count; i++)
    len += snprintf(buf + len, sizeof(buf) - len, " %3u", i);
  len += snprintf(buf + len, sizeof(buf) - len, "\n");

  for (uint32_t i = 0; i < node_count && len < (int)sizeof(buf) - 64; i++) {
    len += snprintf(buf + len, sizeof(buf) - len, "%3u: ", i);
    for (uint32_t j = 0; j < node_count; j++) {
      len += snprintf(buf + len, sizeof(buf) - len, " %3u", (unsigned)numa_distance(i, j));
    }
    len += snprintf(buf + len, sizeof(buf) - len, "\n");
  }

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

static uint32_t procfs_tlbstats_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer);

static uint32_t procfs_drmstats_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  char buf[4096];
  struct drm_stats stats;
  struct virtio_gpu_stats gpu_stats;
  ascentdrm_stats_snapshot(&stats);
  virtio_gpu_get_stats(&gpu_stats);

  uint64_t average_cycles = stats.copy_batches
                                ? stats.copy_cycles / stats.copy_batches : 0;
  int len = snprintf(buf, sizeof(buf),
      "commits: %llu\n"
      "full_commits: %llu\n"
      "damage_commits: %llu\n"
      "direct_scanout_commits: %llu\n"
      "empty_commits: %llu\n"
      "copy_batches: %llu\n"
      "bytes_copied: %llu\n"
      "copy_cycles: %llu\n"
      "average_copy_cycles: %llu\n"
      "max_copy_cycles: %llu\n"
      "damage_clips: %llu\n"
      "bbox_bytes: %llu\n"
      "tsc_khz: %llu\n"
      "virtio_commands_submitted: %llu\n"
      "virtio_commands_completed: %llu\n"
      "virtio_present_batches: %llu\n"
      "virtio_frames_presented: %llu\n"
      "virtio_damage_pixels: %llu\n"
      "virtio_scanout_pixels: %llu\n"
      "virtio_compared_pixels: %llu\n"
      "virtio_unchanged_frames: %llu\n"
      "virtio_present_wait_ms: %llu\n"
      "virtio_max_present_wait_ms: %llu\n"
      "virtio_present_failures: %llu\n"
      "virtio_interrupt_mode: %llu\n"
      "virtio_interrupts: %llu\n"
      "virtio_msix_interrupts: %llu\n"
      "virtio_intx_interrupts: %llu\n"
      "virtio_control_interrupts: %llu\n"
      "virtio_cursor_interrupts: %llu\n"
      "virtio_config_interrupts: %llu\n"
      "virtio_watchdog_polls: %llu\n"
      "virtio_completions_per_interrupt_x1000: %llu\n"
      "virtio_async_submitted: %llu\n"
      "virtio_async_completed: %llu\n"
      "virtio_async_dropped: %llu\n"
      "virtio_cursor_commands: %llu\n"
      "virtio_cursor_completions: %llu\n"
      "virtio_cursor_moves: %llu\n"
      "virtio_cursor_updates: %llu\n"
      "virtio_cursor_hides: %llu\n"
      "virtio_cursor_coalesced: %llu\n"
      "virtio_cursor_failures: %llu\n"
      "virtio_cursor_max_in_flight: %llu\n"
      "virtio_cursor_runtime_ms: %llu\n"
      "virtio_frames_in_flight: %llu\n"
      "virtio_max_frames_in_flight: %llu\n"
      "virtio_config_events: %llu\n"
      "virtio_display_refreshes: %llu\n"
      "virtio_hotplug_events: %llu\n"
      "virtio_edid_reads: %llu\n"
      "virtio_edid_failures: %llu\n"
      "virtio_edid_modes: %llu\n",
      (unsigned long long)stats.commits,
      (unsigned long long)stats.full_commits,
      (unsigned long long)stats.damage_commits,
      (unsigned long long)stats.direct_scanout_commits,
      (unsigned long long)stats.empty_commits,
      (unsigned long long)stats.copy_batches,
      (unsigned long long)stats.bytes_copied,
      (unsigned long long)stats.copy_cycles,
      (unsigned long long)average_cycles,
      (unsigned long long)stats.max_copy_cycles,
      (unsigned long long)stats.damage_clips,
      (unsigned long long)stats.bbox_bytes,
      (unsigned long long)tsc_get_freq_khz(),
      (unsigned long long)gpu_stats.commands_submitted,
      (unsigned long long)gpu_stats.commands_completed,
      (unsigned long long)gpu_stats.present_batches,
      (unsigned long long)gpu_stats.frames_presented,
      (unsigned long long)gpu_stats.damage_pixels,
      (unsigned long long)gpu_stats.scanout_pixels,
      (unsigned long long)gpu_stats.compared_pixels,
      (unsigned long long)gpu_stats.unchanged_frames,
      (unsigned long long)gpu_stats.present_wait_ms,
      (unsigned long long)gpu_stats.max_present_wait_ms,
      (unsigned long long)gpu_stats.present_failures,
      (unsigned long long)gpu_stats.interrupt_mode,
      (unsigned long long)gpu_stats.interrupts,
      (unsigned long long)gpu_stats.msix_interrupts,
      (unsigned long long)gpu_stats.intx_interrupts,
      (unsigned long long)gpu_stats.control_interrupts,
      (unsigned long long)gpu_stats.cursor_interrupts,
      (unsigned long long)gpu_stats.config_interrupts,
      (unsigned long long)gpu_stats.watchdog_polls,
      (unsigned long long)(gpu_stats.interrupts ?
          gpu_stats.commands_completed * 1000ULL / gpu_stats.interrupts : 0),
      (unsigned long long)gpu_stats.async_submitted,
      (unsigned long long)gpu_stats.async_completed,
      (unsigned long long)gpu_stats.async_dropped,
      (unsigned long long)gpu_stats.cursor_commands,
      (unsigned long long)gpu_stats.cursor_completions,
      (unsigned long long)gpu_stats.cursor_moves,
      (unsigned long long)gpu_stats.cursor_updates,
      (unsigned long long)gpu_stats.cursor_hides,
      (unsigned long long)gpu_stats.cursor_coalesced,
      (unsigned long long)gpu_stats.cursor_failures,
      (unsigned long long)gpu_stats.cursor_max_in_flight,
      (unsigned long long)gpu_stats.cursor_runtime_ms,
      (unsigned long long)gpu_stats.frames_in_flight,
      (unsigned long long)gpu_stats.max_frames_in_flight,
      (unsigned long long)gpu_stats.config_events,
      (unsigned long long)gpu_stats.display_refreshes,
      (unsigned long long)gpu_stats.hotplug_events,
      (unsigned long long)gpu_stats.edid_reads,
      (unsigned long long)gpu_stats.edid_failures,
      (unsigned long long)gpu_stats.edid_modes);

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

/* /proc/tlb_stats - what TLB invalidation is costing the system.
 *
 * Compare handler_full_flushes against shootdowns: every single-page
 * invalidation currently makes every other core throw away all of its
 * non-global translations, so the second number is a per-page multiplier on
 * cache-miss cost elsewhere in the system. ack_wait_ns is time spent holding
 * the global vmm_lock's neighbour (shootdown_lock) while spinning for peers. */
static uint32_t procfs_tlbstats_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  char buf[512];
  tlb_shootdown_stats_t s;
  tlb_shootdown_get_stats(&s);

  uint64_t avg_ns = s.shootdowns ? s.ack_wait_ns / s.shootdowns : 0;
  int len = snprintf(buf, sizeof(buf),
      "shootdowns: %llu\n"
      "ipis_sent: %llu\n"
      "broadcast_all: %llu\n"
      "handler_full_flushes: %llu\n"
      "ack_wait_ms: %llu\n"
      "average_ack_wait_us: %llu\n"
      "max_ack_wait_us: %llu\n",
      (unsigned long long)s.shootdowns,
      (unsigned long long)s.ipis_sent,
      (unsigned long long)s.broadcast_all,
      (unsigned long long)s.handler_full_flushes,
      (unsigned long long)(s.ack_wait_ns / 1000000ULL),
      (unsigned long long)(avg_ns / 1000ULL),
      (unsigned long long)(s.max_ack_wait_ns / 1000ULL));

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

uint32_t procfs_cpuinfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {  // 16KB is plenty for 64 cores
  char *buf = kmalloc(16384);
  if (!buf)
    return 0;

  uint32_t cpu_count = cpu_get_count();
  int pos = 0;

  for (uint32_t i = 0; i < cpu_count; i++) {
    uint32_t eax, ebx, ecx, edx;

    // Vendor ID
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    char vendor[13];
    memcpy(vendor,     &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    // Family, Model, Stepping
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    uint32_t features_ecx = ecx;
    uint32_t features_edx = edx;
    uint32_t stepping = eax & 0xF;
    uint32_t model    = (eax >> 4) & 0xF;
    uint32_t family   = (eax >> 8) & 0xF;
    if (family == 0xF)
      family += (eax >> 20) & 0xFF;
    if (family == 0x6 || family == 0xF)
      model += ((eax >> 16) & 0xF) << 4;

    pos += snprintf(buf + pos, 16384 - pos,
        "processor       : %u\n"
        "vendor_id       : %s\n"
        "cpu family      : %u\n"
        "model           : %u\n"
        "stepping        : %u\n",
        i, vendor, family, model, stepping);

    // Brand string
    uint32_t brand_eax;
    __asm__ volatile("cpuid" : "=a"(brand_eax) : "a"(0x80000000));
    if (brand_eax >= 0x80000004) {
      char model_name[49];
      uint32_t *mptr = (uint32_t *)model_name;
      for (uint32_t j = 0; j < 3; j++) {
        __asm__ volatile("cpuid"
                         : "=a"(mptr[j * 4]), "=b"(mptr[j * 4 + 1]),
                           "=c"(mptr[j * 4 + 2]), "=d"(mptr[j * 4 + 3])
                         : "a"(0x80000002 + j));
      }
      model_name[48] = '\0';
      char *trimmed = model_name;
      while (*trimmed == ' ')
        trimmed++;
      pos += snprintf(buf + pos, 16384 - pos, "model name      : %s\n", trimmed);
    }

    uint32_t max_leaf;
    __asm__ volatile("cpuid" : "=a"(max_leaf) : "a"(0));
    uint32_t leaf7_ebx = 0, leaf7_ecx = 0;
    if (max_leaf >= 7) {
      uint32_t unused_a, unused_d;
      __asm__ volatile("cpuid"
                       : "=a"(unused_a), "=b"(leaf7_ebx), "=c"(leaf7_ecx), "=d"(unused_d)
                       : "a"(7), "c"(0));
    }

    uint32_t ext_max = 0, ext_ecx = 0, ext_edx = 0;
    __asm__ volatile("cpuid" : "=a"(ext_max) : "a"(0x80000000));
    if (ext_max >= 0x80000001) {
      uint32_t unused_a, unused_b;
      __asm__ volatile("cpuid"
                       : "=a"(unused_a), "=b"(unused_b), "=c"(ext_ecx), "=d"(ext_edx)
                       : "a"(0x80000001));
    }

    pos += snprintf(buf + pos, 16384 - pos, "flags           :");
#define CPUINFO_FLAG(bits, bit, name)                                          \
    do {                                                                        \
      if ((bits) & (1U << (bit)))                                               \
        pos += snprintf(buf + pos, 16384 - pos, " " name);                     \
    } while (0)
    CPUINFO_FLAG(features_edx, 0, "fpu");
    CPUINFO_FLAG(features_edx, 4, "tsc");
    CPUINFO_FLAG(features_edx, 5, "msr");
    CPUINFO_FLAG(features_edx, 6, "pae");
    CPUINFO_FLAG(features_edx, 8, "cx8");
    CPUINFO_FLAG(features_edx, 9, "apic");
    CPUINFO_FLAG(features_edx, 11, "sep");
    CPUINFO_FLAG(features_edx, 15, "cmov");
    CPUINFO_FLAG(features_edx, 16, "pat");
    CPUINFO_FLAG(features_edx, 19, "clflush");
    CPUINFO_FLAG(features_edx, 23, "mmx");
    CPUINFO_FLAG(features_edx, 24, "fxsr");
    CPUINFO_FLAG(features_edx, 25, "sse");
    CPUINFO_FLAG(features_edx, 26, "sse2");
    CPUINFO_FLAG(features_ecx, 0, "sse3");
    CPUINFO_FLAG(features_ecx, 1, "pclmulqdq");
    CPUINFO_FLAG(features_ecx, 9, "ssse3");
    CPUINFO_FLAG(features_ecx, 12, "fma");
    CPUINFO_FLAG(features_ecx, 13, "cx16");
    CPUINFO_FLAG(features_ecx, 19, "sse4_1");
    CPUINFO_FLAG(features_ecx, 20, "sse4_2");
    CPUINFO_FLAG(features_ecx, 22, "movbe");
    CPUINFO_FLAG(features_ecx, 23, "popcnt");
    CPUINFO_FLAG(features_ecx, 25, "aes");
    CPUINFO_FLAG(features_ecx, 26, "xsave");
    CPUINFO_FLAG(features_ecx, 27, "osxsave");
    CPUINFO_FLAG(features_ecx, 28, "avx");
    CPUINFO_FLAG(features_ecx, 29, "f16c");
    CPUINFO_FLAG(features_ecx, 30, "rdrand");

    CPUINFO_FLAG(ext_edx, 11, "syscall");
    CPUINFO_FLAG(ext_edx, 20, "nx");
    CPUINFO_FLAG(ext_edx, 29, "lm");
    CPUINFO_FLAG(ext_ecx, 0, "lahf_lm");

    CPUINFO_FLAG(leaf7_ebx, 0, "fsgsbase");
    CPUINFO_FLAG(leaf7_ebx, 3, "bmi1");
    CPUINFO_FLAG(leaf7_ebx, 5, "avx2");
    CPUINFO_FLAG(leaf7_ebx, 7, "smep");
    CPUINFO_FLAG(leaf7_ebx, 8, "bmi2");
    CPUINFO_FLAG(leaf7_ebx, 10, "invpcid");
    CPUINFO_FLAG(leaf7_ebx, 20, "smap");
#undef CPUINFO_FLAG
    pos += snprintf(buf + pos, 16384 - pos, "\n\n");
  }

  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)pos)
    size = (uint32_t)pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_sched_debug_read(vfs_node_t *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
  char *buf = kmalloc(4096);
  if (!buf)
    return 0;

  int pos = 0;
  pos += snprintf(buf + pos, 4096 - pos,
                  "Sched Debug: EEVFD (Earliest Eligible Virtual Deadline First)\n"
                  "============================================================\n");

  uint32_t cpus = cpu_get_count();
  for (uint32_t i = 0; i < cpus && pos < 3800; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    hal_irq_state_t flags = hal_irq_save();
    spinlock_acquire(&cpu->queue_lock);

    pos += snprintf(buf + pos, 4096 - pos,
                    "\nCPU#%u: vtime=%llu nr_running=%u total_weight=%llu\n"
                    "  %-6s %-6s %-6s %-8s %-12s %-12s %-6s %-10s\n",
                    cpu->cpu_id,
                    (unsigned long long)cpu->eevfd.vtime,
                    cpu->eevfd.nr_running,
                    (unsigned long long)cpu->eevfd.total_weight,
                    "TID", "COMM", "NICE", "WEIGHT", "VRUNTIME", "DEADLINE", "STATE", "RUNTIME");

    if (cpu->current_thread && !cpu->current_thread->is_idle) {
      struct thread *t = cpu->current_thread;
      pos += snprintf(buf + pos, 4096 - pos,
                      "  %-6u %-6s %-6d %-8u %-12llu %-12llu %-6s %-10llu\n",
                      t->tid, t->comm, (int)t->nice_value, t->se.weight,
                      (unsigned long long)(t->se.vruntime / 1000000ULL),
                      (unsigned long long)(t->se.deadline / 1000000ULL),
                      "RUN",
                      (unsigned long long)t->runtime_total);
    }

    struct rb_node *n = rb_first(&cpu->eevfd.tasks_tree);
    while (n && pos < 3900) {
      struct sched_entity *se = rb_entry(n, struct sched_entity, rb_node);
      struct thread *t = rb_entry(se, struct thread, se);
      pos += snprintf(buf + pos, 4096 - pos,
                      "  %-6u %-6s %-6d %-8u %-12llu %-12llu %-6s %-10llu\n",
                      t->tid, t->comm, (int)t->nice_value, se->weight,
                      (unsigned long long)(se->vruntime / 1000000ULL),
                      (unsigned long long)(se->deadline / 1000000ULL),
                      "RDY",
                      (unsigned long long)t->runtime_total);
      n = rb_next(n);
    }

    spinlock_release(&cpu->queue_lock);
    hal_irq_restore(flags);
  }

  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)pos)
    size = (uint32_t)pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_sysdiag_read(vfs_node_t *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer) {
  (void)node;
  if (!buffer || size == 0) return 0;
  char *buf = kmalloc(8192);
  if (!buf) return 0;

  extern size_t sysdiag_format_report(char *buf, size_t max_size);
  size_t len = sysdiag_format_report(buf, 8192);

  uint32_t bytes_to_copy = 0;
  if (offset < len) {
    bytes_to_copy = (uint32_t)(len - offset);
    if (bytes_to_copy > size) bytes_to_copy = size;
    memcpy(buffer, buf + offset, bytes_to_copy);
  }
  kfree(buf);
  return bytes_to_copy;
}

uint32_t procfs_partitions_read(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {  char *buf = kmalloc(4096);
  if (!buf)
    return 0;

  int pos = snprintf(buf, 4096, "major minor  #blocks  name\n\n");
  int count = block_count();

  for (int i = 0; i < count && pos < 4000; i++) {
    struct block_device *dev = block_get(i);
    if (!dev)
      continue;
    uint64_t blocks =
        (dev->total_sectors * (dev->sector_size ? dev->sector_size : 512)) / 1024;
    /* Major:minor must match sysfs dev, proc diskstats and
     * /proc/self/mountinfo or UDisks2 cannot map partitions to disks. */
    int written = snprintf(buf + pos, 4096 - (size_t)pos,
        "   %u     %-8u%-10llu%s\n",
        dev->linux_major, dev->linux_minor,
        (unsigned long long)blocks, dev->name);
    if (written < 0 || written >= 4096 - pos)
      break;
    pos += written;
  }

  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)pos)
    size = (uint32_t)pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_diskstats_read(vfs_node_t *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  /* ksystemstats' disk plugin, iostat, and udisksd fall back to
   * /proc/diskstats when D-Bus/Solid yields nothing. Counters come from the
   * block layer (block_stat_* in drivers/storage/block.h), updated lock-free
   * at every driver entry point. Untracked columns (merged, timing) report 0,
   * exactly like an idle Linux disk. Field layout is the full Linux 5.4+
   * 17-value set so sscanf-based parsers accept the lines. */
  char *buf = kmalloc(4096);
  if (!buf)
    return 0;

  int pos = 0;
  int count = block_count();

  for (int i = 0; i < count && pos < 3800; i++) {
    struct block_device *dev = block_get(i);
    if (!dev || !dev->name[0])
      continue;
    uint64_t reads =
        __atomic_load_n(&dev->stat_reads, __ATOMIC_RELAXED);
    uint64_t read_sectors =
        __atomic_load_n(&dev->stat_read_sectors, __ATOMIC_RELAXED);
    uint64_t writes =
        __atomic_load_n(&dev->stat_writes, __ATOMIC_RELAXED);
    uint64_t write_sectors =
        __atomic_load_n(&dev->stat_write_sectors, __ATOMIC_RELAXED);
    int written = snprintf(buf + pos, 4096 - (size_t)pos,
        "   %u     %-8u%-12s %llu 0 %llu 0 %llu 0 %llu 0 0 0 0 0 0 0 0 0 0\n",
        dev->linux_major, dev->linux_minor, dev->name,
        (unsigned long long)reads, (unsigned long long)read_sectors,
        (unsigned long long)writes, (unsigned long long)write_sectors);
    if (written < 0 || written >= 4096 - pos)
      break;
    pos += written;
  }

  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)pos)
    size = (uint32_t)pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

static int procfs_latency_line(char *buf, size_t cap, const char *kind,
                                const char *device,
                                const struct storage_latency *s) {
  return scnprintf(buf, cap,
      "%s %s count=%llu total_ms=%llu max_ms=%llu errors=%llu "
      "buckets=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
      kind, device, (unsigned long long)s->count,
      (unsigned long long)s->total_ms, (unsigned long long)s->max_ms,
      (unsigned long long)s->errors,
      (unsigned long long)s->buckets[0], (unsigned long long)s->buckets[1],
      (unsigned long long)s->buckets[2], (unsigned long long)s->buckets[3],
      (unsigned long long)s->buckets[4], (unsigned long long)s->buckets[5],
      (unsigned long long)s->buckets[6], (unsigned long long)s->buckets[7]);
}

static uint32_t procfs_storage_perf_read(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
  const size_t cap = 32768;
  char *buf = kmalloc(cap);
  if (!buf) return 0;
  size_t pos = scnprintf(buf, cap,
      "# storage_perf v1 cumulative; latency buckets ms: <1,<2,<4,<8,<16,<32,<64,>=64\n"
      "# hw_*: NVMe submitted commands/bytes, including retries; namespace only\n");
  for (int i = 0; i < block_count() && pos < cap - 1024; i++) {
    struct block_device *dev = block_get(i);
    if (!dev) continue;
    pos += scnprintf(buf + pos, cap - pos,
        "io %s reads=%llu read_sectors=%llu writes=%llu write_sectors=%llu "
        "hw_reads=%llu hw_read_bytes=%llu hw_writes=%llu hw_write_bytes=%llu hw_errors=%llu\n",
        dev->name,
        (unsigned long long)__atomic_load_n(&dev->stat_reads, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&dev->stat_read_sectors, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&dev->stat_writes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&dev->stat_write_sectors, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&dev->hw_reads, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&dev->hw_read_bytes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&dev->hw_writes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&dev->hw_write_bytes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&dev->hw_errors, __ATOMIC_RELAXED));
    struct storage_latency flush;
    storage_latency_snapshot(&dev->hw_flush, &flush);
    pos += procfs_latency_line(buf + pos, cap - pos, "nvme_flush", dev->name, &flush);
  }
  for (unsigned i = 0; i < 8 && pos < cap - 1024; i++) {
    struct block_device *dev;
    struct storage_latency wait, hold;
    if (!ext3_perf_snapshot(i, &dev, &wait, &hold)) continue;
    const char *name = dev ? dev->name : "unknown";
    pos += procfs_latency_line(buf + pos, cap - pos, "journal_wait", name, &wait);
    pos += procfs_latency_line(buf + pos, cap - pos, "journal_hold", name, &hold);
  }
  node->length = pos;
  if (offset >= pos) { kfree(buf); return 0; }
  if (size > pos - offset) size = pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

#define PROCFS_MAX_MOUNTS 256
#define PROCFS_MOUNTS_BUFSZ 65536
#define PROCFS_MOUNTINFO_BUFSZ 65536

/* Canonicalize a mount-table source for libmount/Solid/UDisks matching.
 * Bare kernel names ("nvme0n1p1") never match UDisks' "/dev/..." Device
 * property, so the mount gets no Size/MountPoints and Plasma Disk Usage
 * stays at 0 B. Returns `src` unchanged for pseudo filesystems, otherwise
 * rewrites known block devices to their /dev/ path in `out`. */
static const char *procfs_mount_src(const char *src, char *out,
                                    size_t out_sz) {
  if (!src || !src[0])
    return "none";
  if (src[0] == '/' || strcmp(src, "none") == 0 ||
      strcmp(src, "tmpfs") == 0 || strcmp(src, "proc") == 0 ||
      strcmp(src, "sysfs") == 0 || strcmp(src, "devtmpfs") == 0 ||
      strcmp(src, "devpts") == 0 || strcmp(src, "cgroup") == 0 ||
      strcmp(src, "cgroup2") == 0)
    return src;
  if (block_get_by_name(src) != NULL) {
    snprintf(out, out_sz, "/dev/%s", src);
    return out;
  }
  return src;
}

static void procfs_mount_options(uint64_t flags, char options[80]) {
  snprintf(options, 80, "%s%s%s%s,relatime", flags & 1 ? "ro" : "rw",
           flags & 2 ? ",nosuid" : "", flags & 4 ? ",nodev" : "",
           flags & 8 ? ",noexec" : "");
}

uint32_t procfs_mounts_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  const size_t cap = PROCFS_MOUNTS_BUFSZ;
  char *buf = kmalloc(cap);
  if (!buf)
    return 0;

  /* Heap-allocate the namespace mount table to keep it off the stack. */
  vfs_mount_info_t *mounts =
      kmalloc(sizeof(vfs_mount_info_t) * PROCFS_MAX_MOUNTS);
  if (!mounts) {
    kfree(buf);
    return 0;
  }
  int count = node->impl ? sched_get_mount_snapshot(node->impl, mounts, PROCFS_MAX_MOUNTS)
                         : vfs_get_mounts(mounts, PROCFS_MAX_MOUNTS);
  size_t pos = 0;

  for (int i = 0; i < count; i++) {
    /* libmount/Solid/UDisks match the source string against the UDisks
     * Device property ("/dev/nvme0n1p1"). A bare "nvme0n1p1" never matches,
     * so the mount gets no Size/MountPoints and Plasma Disk Usage stays at
     * 0 B. Emit the /dev/ path whenever the name is a known block device. */
    const char *rawsrc =
        mounts[i].dev_name[0] ? mounts[i].dev_name : "none";
    char srcbuf[80];
    const char *src = procfs_mount_src(rawsrc, srcbuf, sizeof(srcbuf));
    char options[80]; procfs_mount_options(mounts[i].flags, options);
    int w = snprintf(buf + pos, cap - pos,
        "%s %s %s %s 0 0\n",
        src, mounts[i].mountpoint, mounts[i].fs_type, options);
    if (w < 0 || (size_t)w >= cap - pos)
      break;
    pos += (size_t)w;
  }

  if (count == 0 && pos < cap - 64) {
    int w = snprintf(buf + pos, cap - pos,
                     "none / ext4 rw,relatime 0 0\n");
    if (w > 0 && (size_t)w < cap - pos)
      pos += (size_t)w;
  }

  kfree(mounts);
  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)pos)
    size = (uint32_t)pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_uptime_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  char buf[64];
  uint64_t ms  = lapic_timer_get_ms();
  uint64_t sec = ms / 1000;
  uint32_t rem = (uint32_t)((ms % 1000) / 10);

  int len = snprintf(buf, sizeof(buf), "%llu.%02u 0.00\n",
                     (unsigned long long)sec, rem);

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

static uint32_t procfs_interrupts_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  uint32_t cpu_count = cpu_get_count();
  if (cpu_count == 0)
    cpu_count = 1;
  if (cpu_count > MAX_CPUS)
    cpu_count = MAX_CPUS;

  /* Include the legacy IRQ lines even before they have fired. Other vectors
   * are shown once they have received an interrupt. */
  bool show_vector[256] = {0};
  uint32_t row_count = 16;
  for (uint32_t vector = 32; vector < 48; vector++)
    show_vector[vector] = true;
  for (uint32_t vector = 48; vector < 256; vector++) {
    bool active = false;
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
      if (isr_get_vector_count(cpu, vector) != 0) {
        active = true;
        break;
      }
    }
    if (active) {
      show_vector[vector] = true;
      row_count++;
    }
  }

  /* A uint64_t field takes at most 20 digits plus a separator. Allocate from
   * the number of CPU columns and rows so even a 64-CPU system fits. */
  size_t capacity = 64 + (size_t)cpu_count * 21 +
                    (size_t)row_count * ((size_t)cpu_count * 21 + 64);
  char *text = kmalloc(capacity);
  if (!text)
    return 0;

  int pos = snprintf(text, capacity, "    ");
  for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
    char label[16];
    snprintf(label, sizeof(label), "CPU%u", cpu);
    pos += snprintf(text + pos, capacity - (size_t)pos, "%21s", label);
  }
  pos += snprintf(text + pos, capacity - (size_t)pos, "\n");

  for (uint32_t vector = 32; vector < 256; vector++) {
    if (!show_vector[vector])
      continue;

    if (vector < 48) {
      pos += snprintf(text + pos, capacity - (size_t)pos, "%3u:",
                      vector - 32);
    } else {
      pos += snprintf(text + pos, capacity - (size_t)pos, "%3u:", vector);
    }
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
      pos += snprintf(text + pos, capacity - (size_t)pos, " %20llu",
                      (unsigned long long)isr_get_vector_count(cpu, vector));
    }
    if (vector < 48) {
      pos += snprintf(text + pos, capacity - (size_t)pos, "  IRQ %u\n",
                      vector - 32);
    } else {
      pos += snprintf(text + pos, capacity - (size_t)pos, "  vector %u\n",
                      vector);
    }
  }

  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(text);
    return 0;
  }
  if (size > (uint32_t)pos - offset)
    size = (uint32_t)pos - offset;
  memcpy(buffer, text + offset, size);
  kfree(text);
  return size;
}

uint32_t procfs_stat_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
  char *buf = kmalloc(2048);
  if (!buf)
    return 0;

  uint32_t ncpus = cpu_get_count();
  if (ncpus == 0)
    ncpus = 1;

  /* Snapshot scheduler-accounted time per CPU. USER_HZ=100, so one jiffy is
   * 10 ms. Aggregate by summing the real CPU-local counters below. */
  uint64_t total_user_ms = 0, total_system_ms = 0, total_idle_ms = 0;
  uint64_t cpu_user_ms[MAX_CPUS] = {0};
  uint64_t cpu_system_ms[MAX_CPUS] = {0};
  uint64_t cpu_idle_ms[MAX_CPUS] = {0};
  if (ncpus > MAX_CPUS)
    ncpus = MAX_CPUS;
  for (uint32_t i = 0; i < ncpus; i++) {
    sched_get_cpu_times(i, &cpu_user_ms[i], &cpu_system_ms[i],
                        &cpu_idle_ms[i]);
    total_user_ms += cpu_user_ms[i];
    total_system_ms += cpu_system_ms[i];
    total_idle_ms += cpu_idle_ms[i];
  }

  uint64_t user_jiffies = total_user_ms / 10;
  uint64_t system_jiffies = total_system_ms / 10;
  uint64_t idle_jiffies = total_idle_ms / 10;

  /* "cpu" aggregate line: fields are user nice system idle iowait irq softirq */
  int pos = snprintf(buf, 2048,
      "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n",
      (unsigned long long)user_jiffies,
      (unsigned long long)system_jiffies,
      (unsigned long long)idle_jiffies);

  for (uint32_t i = 0; i < ncpus; i++) {
    pos += snprintf(buf + pos, 2048 - pos,
        "cpu%u %llu 0 %llu %llu 0 0 0 0 0 0\n",
        i,
        (unsigned long long)(cpu_user_ms[i] / 10),
        (unsigned long long)(cpu_system_ms[i] / 10),
        (unsigned long long)(cpu_idle_ms[i] / 10));
  }

  uint16_t nthreads = sched_get_thread_count();
  pos += snprintf(buf + pos, 2048 - pos,
      "intr 0\n"
      "ctxt 0\n"
      "btime %llu\n"
      "processes %u\n"
      "procs_running %u\n"
      "procs_blocked 0\n",
      (unsigned long long)rtc_get_boot_timestamp(),
      (unsigned int)nthreads,
      (unsigned int)sched_get_runnable_thread_count());

  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)pos)
    size = (uint32_t)pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}


// /proc/loadavg — "load1 load5 load15 running/total last_pid\n"
uint32_t procfs_loadavg_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  char buf[128];

  uint32_t ncpus = cpu_get_count();
  if (ncpus == 0)
    ncpus = 1;
  uint16_t nthreads = sched_get_thread_count();

  uint32_t running = sched_get_runnable_thread_count();
  if (running == 0)
    running = 1;

  // load = running / ncpus expressed as X.XX
  uint64_t load_int  = running / ncpus;
  uint64_t load_frac = (running * 100 / ncpus) % 100;

  int len = snprintf(buf, sizeof(buf),
      "%llu.%02llu %llu.%02llu %llu.%02llu %u/%u 1\n",
      (unsigned long long)load_int, (unsigned long long)load_frac,
      (unsigned long long)load_int, (unsigned long long)load_frac,
      (unsigned long long)load_int, (unsigned long long)load_frac,
      running, (unsigned int)nthreads);

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

uint32_t procfs_heapinfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  // 4KB should be plenty for heap info
  char *buf = kmalloc(4096);
  if (!buf)
    return 0;

  heap_get_info(buf);

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_version_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  char buf[256];
  int len = snprintf(buf, sizeof(buf),
      "Ascension version 2.0.0-beta (gcc) "
      "#1 SMP AscentOS\n");

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

uint32_t procfs_filesystems_read(vfs_node_t *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
  // List the filesystem types the kernel supports.
  // "nodev" prefix means the fs doesn't require a block device.
  // Unified-only: advertise cgroup2, not v1. Hybrid submounts still succeed
  // if attempted (harmless), but root validity (controllers content) decides.
  // NOTE: hybrid v1 lines in pid/cgroup + /proc/cgroups cover elogind's own
  // legacy controller lookup; filesystems list stays v2-only so OpenRC
  // defaults to unified root.
  const char *fs =
      "nodev\tsysfs\n"
      "nodev\ttmpfs\n"
      "nodev\tdevtmpfs\n"
      "nodev\tproc\n"
      "nodev\tdevfs\n"
      "nodev\tramfs\n"
      "nodev\tfuse\n"
      "nodev\tcgroup\n"
      "nodev\tcgroup2\n"
      "\text2\n"
      "\text3\n"
      "\text4\n"
      "\tvfat\n";

  uint32_t len = (uint32_t)strlen(fs);
  node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, fs + offset, size);
  return size;
}

/* /proc/cgroups — v1 hierarchy table matching pid/cgroup stub lines.
 * Header + single elogind controller (hierarchy 2, enabled). */
uint32_t procfs_cgroups_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  static const char cgroups_text[] =
      "#subsys_name\thierarchy\tnum_cgroups\tenabled\n"
      "elogind\t2\t1\t1\n";
  uint32_t len = (uint32_t)(sizeof(cgroups_text) - 1);
  if (node)
    node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, cgroups_text + offset, size);
  return size;
}

/* /proc/klog — the persistent boot-log ring (console/klog.c).  Reading from
 * offset 0 rewinds to the oldest retained byte. */
uint32_t procfs_klog_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
  uint32_t total = klog_ring_length();
  node->length = total;
  return klog_ring_read(offset, size, buffer);
}

/* /proc/bootlog — frozen boot-log snapshot (console/klog.c). */
uint32_t procfs_bootlog_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  uint32_t total = klog_bootlog_length();
  node->length = total;
  return klog_bootlog_read(offset, size, buffer);
}

/* /proc/usb — USB controller/device/HID-driver diagnostics (drivers/usb). */
uint32_t procfs_usb_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
  (void)node;
  return usb_diag_read(offset, size, buffer);
}

static uint32_t procfs_gpu_debug_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  (void)node; return gpu_diag_read(offset,size,buffer);
}
static uint32_t procfs_gpu_debug_write(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  (void)node; (void)offset; return gpu_diag_control(size,buffer);
}

/* /proc/usb_mouse, /proc/usb_xhci — short per-subsystem views so a single
 * `cat` on a console without scrollback shows the failing pipe. */
uint32_t procfs_usb_mouse_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                               uint8_t *buffer) {
  (void)node;
  return usb_mouse_diag_read(offset, size, buffer);
}

uint32_t procfs_usb_xhci_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  (void)node;
  return usb_xhci_diag_read(offset, size, buffer);
}

/* /proc/usb_debug — runtime switch for the verbose USB transfer trace.
 * `echo 1 > /proc/usb_debug` enables it, `echo 0` disables it. */
uint32_t procfs_usb_debug_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                               uint8_t *buffer) {
  (void)node;
  static const char on[] = "1\n";
  static const char off[] = "0\n";
  const char *s = usb_get_verbose() ? on : off;
  uint32_t len = 2;
  if (offset >= len || !buffer)
    return 0;
  uint32_t n = len - offset;
  if (n > size)
    n = size;
  for (uint32_t i = 0; i < n; i++)
    buffer[i] = (uint8_t)s[offset + i];
  return n;
}

uint32_t procfs_usb_debug_write(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (!buffer || !size)
    return 0;
  char c = (char)buffer[0];
  usb_set_verbose(c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T');
  return size;
}

/* The real Limine command line (kernel/src/kernel.c), matching upstream
 * /proc/cmdline.  Session helpers parse it (drm-pick.sh looks for the
 * kpi_emu_sink boot parameter); it used to be a hard-coded "Xfbdev" string. */
extern const char *kernel_boot_cmdline;

uint32_t procfs_cmdline_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  (void)node;
  const char *cmd = kernel_boot_cmdline ? kernel_boot_cmdline : "";
  uint32_t len = (uint32_t)strlen(cmd);
  uint32_t total = len + 1; /* upstream prints the cmdline plus '\n' */

  if (offset >= total)
    return 0;
  if (offset + size > total)
    size = total - offset;
  for (uint32_t i = 0; i < size; i++) {
    uint32_t pos = offset + i;
    buffer[i] = (pos < len) ? (uint8_t)cmd[pos] : (uint8_t)'\n';
  }
  return size;
}

// Helpers shared by per-PID readers

// Parse a decimal string; returns 0 if not a pure number.
static uint32_t str_to_pid(const char *s) {
  if (!s || !*s)
    return 0;
  uint32_t v = 0;
  for (const char *p = s; *p; p++) {
    if (*p < '0' || *p > '9')
      return 0;
    v = v * 10 + (uint32_t)(*p - '0');
  }
  return v;
}

// Map thread_state_t to the single-char Linux stat state.
static char thread_state_char(thread_state_t s) {
  switch (s) {
  case THREAD_RUNNING: return 'R';
  case THREAD_READY:   return 'R';
  case THREAD_BLOCKED: return 'S';
  case THREAD_SLEEPING:return 'S';
  case THREAD_DEAD:    return 'Z';
  case THREAD_ZOMBIE:  return 'Z';
  default:             return 'S';
  }
}

// /proc/<pid>/stat
static bool procfs_pid_visible(uint32_t pid) {
  return pid_namespace_visible(sched_get_current(), pid) != 0;
}
static bool procfs_pid_ptrace_access(uint32_t pid) {
  struct thread *current = sched_get_current();
  struct sched_thread_snapshot target;
  if (!current || !procfs_pid_visible(pid) ||
      !sched_get_thread_snapshot(pid, &target)) return false;
  return target.tgid == current->tgid || capability_has(current, CAP_SYS_PTRACE);
}

static uint32_t procfs_pid_stat_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  uint32_t pid = node->impl;
  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  char buf[512];
  uint64_t user_jiffies = t.runtime_user_ms / 10;
  uint64_t system_jiffies = t.runtime_system_ms / 10;
  uint64_t start_ticks = t.start_time_ms / 10;
  if (start_ticks == 0)
    start_ticks = 1;

  // Fields:
  // 1:pid 2:(comm) 3:state 4:ppid 5:pgrp 6:session 7:tty 8:tpgid 9:flags
  // 10..13:flt 14:utime 15:stime 16:cutime 17:cstime 18:pri 19:nice 20:threads 21:itreal 22:starttime
  // 23..38: 16 zeroes
  // 39:processor 40..52: 13 zeroes
  int len = snprintf(buf, sizeof(buf),
      "%u (%s) %c %u %u %u 0 0 0 0 0 0 0 %llu %llu "
      "0 0 20 0 1 0 %llu "
      "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %u "
      "0 0 0 0 0 0 0 0 0 0 0 0 0\n",
      pid_namespace_visible(sched_get_current(), pid),
      t.comm[0] ? t.comm : "unknown",
      thread_state_char(t.state),
      pid_namespace_visible(sched_get_current(), t.parent_tid),
      pid_namespace_visible(sched_get_current(), t.pgid),
      pid_namespace_visible(sched_get_current(), t.pgid),
      (unsigned long long)user_jiffies,
      (unsigned long long)system_jiffies,
      (unsigned long long)start_ticks,
      t.cpu_index);

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

// /proc/<pid>/syscall - last dispatched syscall + args + return value.
// Sampling it twice on a 100% single-threaded spinner names the call:
// a stable number with an instant error return is the spin.
static uint32_t procfs_pid_syscall_read(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_ptrace_access(node->impl)) return 0;
  uint32_t pid = node->impl;
  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  const char *name = syscall_get_name(t.last_syscall_num);
  char *buf = kmalloc(1024);
  if (!buf)
    return 0;
  int len;
  if (t.last_syscall_num == 271 || t.last_syscall_num == 7) {
    /* ppoll/poll: show the requested timeout and fd count so a spin
     * reads as timeout={0,0} (userspace busy-poll) vs a real wait. */
    len = snprintf(buf, 1024,
        "%llu %s ret=%lld nfds=%llu timeout=%lld.%09lld args=0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx\n"
        "pollset: %d(0x%x:%s) %d(0x%x:%s) %d(0x%x:%s) %d(0x%x:%s) %d(0x%x:%s) %d(0x%x:%s) %d(0x%x:%s) %d(0x%x:%s)\n",
        (unsigned long long)t.last_syscall_num,
        name ? name : "unknown",
        (long long)t.last_syscall_ret,
        (unsigned long long)t.last_poll_nfds,
        (long long)t.last_poll_sec, (long long)t.last_poll_nsec,
        (unsigned long long)t.last_syscall_args[0],
        (unsigned long long)t.last_syscall_args[1],
        (unsigned long long)t.last_syscall_args[2],
        (unsigned long long)t.last_syscall_args[3],
        (unsigned long long)t.last_syscall_args[4],
        (unsigned long long)t.last_syscall_args[5],
        t.last_poll_fds[0], t.last_poll_events[0], t.last_poll_paths[0],
        t.last_poll_fds[1], t.last_poll_events[1], t.last_poll_paths[1],
        t.last_poll_fds[2], t.last_poll_events[2], t.last_poll_paths[2],
        t.last_poll_fds[3], t.last_poll_events[3], t.last_poll_paths[3],
        t.last_poll_fds[4], t.last_poll_events[4], t.last_poll_paths[4],
        t.last_poll_fds[5], t.last_poll_events[5], t.last_poll_paths[5],
        t.last_poll_fds[6], t.last_poll_events[6], t.last_poll_paths[6],
        t.last_poll_fds[7], t.last_poll_events[7], t.last_poll_paths[7]);
  } else {
    len = snprintf(buf, 1024,
        "%llu %s ret=%lld args=0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx\n",
      (unsigned long long)t.last_syscall_num,
      name ? name : "unknown",
      (long long)t.last_syscall_ret,
      (unsigned long long)t.last_syscall_args[0],
      (unsigned long long)t.last_syscall_args[1],
      (unsigned long long)t.last_syscall_args[2],
      (unsigned long long)t.last_syscall_args[3],
      (unsigned long long)t.last_syscall_args[4],
      (unsigned long long)t.last_syscall_args[5]);
  }

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

// /proc/<pid>/status
static uint32_t procfs_pid_status_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  uint32_t pid = node->impl;
  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  char *buf = kmalloc(1024);
  if (!buf)
    return 0;

  uint64_t virt_bytes = 0;
  uint64_t res_bytes = 0;
  sched_get_mem_snapshot(pid, &virt_bytes, &res_bytes);

  uint64_t virt_kb = virt_bytes / 1024;
  uint64_t rss_kb  = res_bytes / 1024;

  int len = snprintf(buf, 1024,
      "Name:\t%s\n"
      "State:\t%c\n"
      "Tgid:\t%u\n"
      "Pid:\t%u\n"
      "PPid:\t%u\n"
      "Uid:\t%u\t%u\t%u\t%u\n"
      "Gid:\t%u\t%u\t%u\t%u\n"
      "Threads:\t%u\n"
      "VmSize:\t%llu kB\n"
      "VmRSS:\t%llu kB\n"
      "CapInh:\t%016llx\nCapPrm:\t%016llx\nCapEff:\t%016llx\n"
      "CapBnd:\t%016llx\nCapAmb:\t%016llx\n"
      "NoNewPrivs:\t%u\nSeccomp:\t%u\n",
      t.comm[0] ? t.comm : "unknown",
      thread_state_char(t.state),
      pid_namespace_visible(sched_get_current(), t.tgid ? t.tgid : t.tid),
      pid_namespace_visible(sched_get_current(), t.tid),
      pid_namespace_visible(sched_get_current(), t.parent_tid),
      t.uid, t.euid, t.suid, t.uid,
      t.gid, t.egid, t.sgid, t.gid,
      sched_get_group_threads(t.tgid ? t.tgid : t.tid, 0, NULL),
      (unsigned long long)virt_kb,
      (unsigned long long)rss_kb,
      (unsigned long long)t.cap_inheritable, (unsigned long long)t.cap_permitted,
      (unsigned long long)t.cap_effective, (unsigned long long)t.cap_bounding,
      (unsigned long long)t.cap_ambient, t.no_new_privs ? 1u : 0u, t.seccomp_mode);

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

/*
 * /proc/<pid>/cmdline - the argv blob captured at exec, arguments NUL separated
 * exactly like Linux.  Readers match command lines against this content
 * (pgrep/pkill -f, ps, jps, every "is it already running?" guard in the desktop
 * scripts), so it has to carry the full path the process was invoked with.
 *
 * Falls back to comm only when no argv was recorded - a kernel thread, or the
 * window after a fresh exec's address space was reset but before its stack was
 * built - so readers get a usable name instead of an empty file.
 */
static uint32_t procfs_pid_cmdline_read(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  uint32_t pid = node->impl;

  size_t total = sched_thread_cmdline_size(pid);
  if (total > 0) {
    node->length = (uint32_t)total;
    return (uint32_t)sched_read_thread_cmdline(pid, offset, size, buffer);
  }

  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  const char *cmd = t.comm[0] ? t.comm : "unknown";
  uint32_t len = (uint32_t)strlen(cmd) + 1; // include NUL
  node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, cmd + offset, size);
  return size;
}

/*
 * /proc/<pid>/comm - the thread name, newline terminated.  This is the file
 * pgrep/pkill match against when -f is NOT given, and it is deliberately
 * distinct from cmdline: comm is capped at 15 characters and holds a bare
 * name, never a path.
 */
/* /proc/<pid>/cgroup — hybrid stub: v1 multi-controller line + v2 root line.
 * v1 named hierarchies appear as "name=NAME"; queries may use bare ("elogind")
 * or prefixed ("name=elogind") form depending on call site. List bare cpu/
 * memory/pids/systemd/elogind/openrc plus name= variants so any legacy query
 * matches. All at root "/". */
static uint32_t procfs_pid_cgroup_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  static const char cgroup_line[] = "2:cpu,memory,pids,systemd,elogind,openrc,name=elogind,name=systemd:/\n"
                                    "0::/\n";
  uint32_t len = (uint32_t)(sizeof(cgroup_line) - 1);

  if (node)
    node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, cgroup_line + offset, size);
  return size;
}

static uint32_t procfs_pid_comm_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  uint32_t pid = node->impl;
  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  char line[18]; // comm (15) + '\n' + slack
  uint32_t len = (uint32_t)strlen(t.comm);
  if (len > sizeof(t.comm) - 1)
    len = sizeof(t.comm) - 1;
  memcpy(line, t.comm, len);
  line[len++] = '\n';

  node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, line + offset, size);
  return size;
}

static uint32_t procfs_pid_comm_write(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  (void)offset;
  if (!node || !buffer || size == 0) return 0;
  if (!procfs_pid_visible(node->impl)) return 0;
  uint32_t pid = (uint32_t)node->impl;
  struct thread *t = sched_get_thread_by_tid(pid);
  if (!t) return 0;
  uint32_t len = size;
  if (len > 15) len = 15;
  memcpy(t->comm, buffer, len);
  while (len > 0 && (t->comm[len - 1] == '\n' || t->comm[len - 1] == '\r')) {
    len--;
  }
  t->comm[len] = '\0';
  return size;
}

// /proc/<pid>/statm — memory in pages
static uint32_t procfs_pid_statm_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  uint32_t pid = node->impl;
  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  char buf[64];

  /* Field 2 (resident) is what WTF::memoryFootprint() reads to decide whether
   * to kill a WebKit web process, so it has to be the real count - see
   * vmm_user_resident_bytes(). */
  uint64_t virt_bytes = 0;
  uint64_t res_bytes = 0;
  sched_get_mem_snapshot(pid, &virt_bytes, &res_bytes);

  uint64_t vp = virt_bytes / 4096;
  uint64_t rp = res_bytes / 4096;

  int len = snprintf(buf, sizeof(buf),
      "%llu %llu 0 0 0 0 0\n",
      (unsigned long long)vp,
      (unsigned long long)rp);

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

// /proc/<pid>/io read
// htop 3.x reads this for I/O accounting. Stub with zeros.
static uint32_t procfs_pid_io_read(vfs_node_t *node, uint32_t offset,
                                   uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  (void)node;
  const char *io = "rchar: 0\n"
                   "wchar: 0\n"
                   "syscr: 0\n"
                   "syscw: 0\n"
                   "read_bytes: 0\n"
                   "write_bytes: 0\n"
                   "cancelled_write_bytes: 0\n";
  uint32_t len = (uint32_t)strlen(io);
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, io + offset, size);
  return size;
}

// /proc/<pid>/auxv
static uint32_t procfs_pid_auxv_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_ptrace_access(node->impl)) return 0;
  uint32_t pid = node->impl;
  return (uint32_t)sched_read_thread_auxv(pid, offset, size, buffer);
}

// /proc/<pid>/setgroups (used by bwrap, podman, flatpak, unshare)
static uint32_t procfs_pid_setgroups_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  (void)node;
  const char *val = "allow\n";
  uint32_t len = 6;
  if (offset >= len || !buffer) return 0;
  if (offset + size > len) size = len - offset;
  memcpy(buffer, val + offset, size);
  return size;
}

static uint32_t procfs_pid_setgroups_write(vfs_node_t *node, uint32_t offset,
                                           uint32_t size, uint8_t *buffer) {
  (void)node; (void)offset; (void)buffer;
  return size; // Accept "deny" or "allow" successfully
}

// /proc/<pid>/uid_map and gid_map (used by bwrap, podman, flatpak)
static uint32_t procfs_pid_id_map_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  (void)node;
  const char *val = "         0          0 4294967295\n";
  uint32_t len = 34;
  if (offset >= len || !buffer) return 0;
  if (offset + size > len) size = len - offset;
  memcpy(buffer, val + offset, size);
  return size;
}

static uint32_t procfs_pid_id_map_write(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
  (void)node; (void)offset; (void)buffer;
  return size; // Accept mapping successfully
}

// /proc/<pid>/oom_score_adj
static uint32_t procfs_pid_oom_score_adj_read(vfs_node_t *node, uint32_t offset,
                                              uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  (void)node;
  const char *val = "0\n";
  uint32_t len = 2;
  if (offset >= len || !buffer) return 0;
  if (offset + size > len) size = len - offset;
  memcpy(buffer, val + offset, size);
  return size;
}

static uint32_t procfs_pid_oom_score_adj_write(vfs_node_t *node, uint32_t offset,
                                               uint32_t size, uint8_t *buffer) {
  (void)node; (void)offset; (void)buffer;
  return size;
}

// Helper to recursively collect VMAs in address order into a flat array
static void procfs_vma_collect(struct vma *node, struct vma **arr, int *count, int max_count) {
  if (!node || *count >= max_count)
    return;
  procfs_vma_collect(node->left, arr, count, max_count);
  if (*count < max_count) {
    arr[*count] = node;
    (*count)++;
  }
  procfs_vma_collect(node->right, arr, count, max_count);
}

// /proc/<pid>/maps
static uint32_t procfs_pid_maps_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_ptrace_access(node->impl)) return 0;
  uint32_t pid = node->impl;
  struct thread *t = sched_get_thread_by_tid(pid);
  if (!t || !t->mm)
    return 0;

  spinlock_acquire(&t->mm->lock);

  int vma_count = t->mm->vmas.count;
  if (vma_count <= 0) {
    spinlock_release(&t->mm->lock);
    return 0;
  }

  // Allocate an array of vma pointers to snapshot the tree under lock
  int max_vmas = vma_count + 16;
  struct vma **vma_ptrs = kmalloc(sizeof(struct vma *) * max_vmas);
  if (!vma_ptrs) {
    spinlock_release(&t->mm->lock);
    return 0;
  }

  int collected = 0;
  procfs_vma_collect(t->mm->vmas.root, vma_ptrs, &collected, max_vmas);

  // Allocate snapshot entries so we can release t->mm->lock quickly
  struct vma_snap {
    uint64_t start;
    uint64_t end;
    uint64_t prot;
    uint64_t flags;
    uint64_t offset;
    int fd;
    void *file_node;
    uint32_t inode;
  };

  struct vma_snap *snaps = kmalloc(sizeof(struct vma_snap) * collected);
  if (!snaps) {
    kfree(vma_ptrs);
    spinlock_release(&t->mm->lock);
    return 0;
  }

  for (int i = 0; i < collected; i++) {
    struct vma *v = vma_ptrs[i];
    snaps[i].start = v->start;
    snaps[i].end = v->end;
    snaps[i].prot = v->prot;
    snaps[i].flags = v->flags;
    snaps[i].offset = v->offset;
    snaps[i].fd = v->fd;
    snaps[i].file_node = v->file_node;
    if (v->file_node && vfs_node_is_alive((vfs_node_t *)v->file_node)) {
      snaps[i].inode = ((vfs_node_t *)v->file_node)->inode;
    } else {
      snaps[i].inode = 0;
    }
  }

  uint64_t brk_base = t->mm->brk_base;
  uint64_t brk_current = t->mm->brk_current;

  kfree(vma_ptrs);
  spinlock_release(&t->mm->lock);

  // Now format each line into a dynamically sized buffer
  // Typical line length is ~80-120 bytes. Let's allocate 256 bytes per VMA.
  size_t buf_cap = (size_t)collected * 256 + 256;
  char *buf = kmalloc(buf_cap);
  if (!buf) {
    kfree(snaps);
    return 0;
  }

  size_t pos = 0;
  for (int i = 0; i < collected; i++) {
    struct vma_snap *s = &snaps[i];

    char perms[5];
    perms[0] = (s->prot & 0x1) ? 'r' : '-';
    perms[1] = (s->prot & 0x2) ? 'w' : '-';
    perms[2] = (s->prot & 0x4) ? 'x' : '-';
    perms[3] = (s->flags & MAP_SHARED) ? 's' : 'p';
    perms[4] = '\0';

    char path_buf[256];
    path_buf[0] = '\0';

    if (s->flags & MAP_GROWSDOWN) {
      strcpy(path_buf, "[stack]");
    } else if (brk_base && s->start >= brk_base && s->end <= brk_current) {
      strcpy(path_buf, "[heap]");
    } else if (s->start == VDSO_USER_BASE) {
      strcpy(path_buf, "[vdso]");
    } else if (s->flags & MAP_SYSV_SHM) {
      snprintf(path_buf, sizeof(path_buf), "/SYSV%08x (deleted)", (unsigned)s->offset);
    } else if (s->fd >= 0) {
      if (!sched_get_fd_path_snapshot(pid, (uint32_t)s->fd, path_buf, sizeof(path_buf))) {
        path_buf[0] = '\0';
      }
    } else if (s->file_node && vfs_node_is_alive((vfs_node_t *)s->file_node)) {
      vfs_node_t *vn = (vfs_node_t *)s->file_node;
      if (vn->name[0]) {
        strncpy(path_buf, vn->name, sizeof(path_buf) - 1);
        path_buf[sizeof(path_buf) - 1] = '\0';
      }
    }

    // Format line: address perms offset dev inode pathname\n
    // e.g., 00400000-00452000 r-xp 00000000 08:02 173521 /usr/bin/foo
    int written;
    if (path_buf[0]) {
      written = snprintf(buf + pos, buf_cap - pos,
                         "%08lx-%08lx %s %08lx 00:00 %u %s\n",
                         (unsigned long)s->start, (unsigned long)s->end,
                         perms, (unsigned long)s->offset,
                         s->inode, path_buf);
    } else {
      written = snprintf(buf + pos, buf_cap - pos,
                         "%08lx-%08lx %s %08lx 00:00 %u\n",
                         (unsigned long)s->start, (unsigned long)s->end,
                         perms, (unsigned long)s->offset,
                         s->inode);
    }

    if (written > 0 && pos + (size_t)written < buf_cap) {
      pos += (size_t)written;
    } else {
      break;
    }
  }

  kfree(snaps);

  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }

  uint32_t to_copy = (uint32_t)pos - offset;
  if (to_copy > size)
    to_copy = size;

  memcpy(buffer, buf + offset, to_copy);
  kfree(buf);
  return to_copy;
}

// /proc/<pid>/numa_maps
static uint32_t procfs_pid_numa_maps_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_ptrace_access(node->impl)) return 0;
  uint32_t pid = node->impl;
  struct thread *t = sched_get_thread_by_tid(pid);
  if (!t || !t->mm)
    return 0;

  spinlock_acquire(&t->mm->lock);
  int vma_count = t->mm->vmas.count;
  if (vma_count <= 0) {
    spinlock_release(&t->mm->lock);
    return 0;
  }

  int max_vmas = vma_count + 16;
  struct vma **vma_ptrs = kmalloc(sizeof(struct vma *) * max_vmas);
  if (!vma_ptrs) {
    spinlock_release(&t->mm->lock);
    return 0;
  }

  int collected = 0;
  procfs_vma_collect(t->mm->vmas.root, vma_ptrs, &collected, max_vmas);

  struct numa_vma_snap {
    uint64_t start;
    uint64_t end;
    int policy;
    uint64_t nodemask;
    bool is_anon;
  };

  struct numa_vma_snap *snaps = kmalloc(sizeof(struct numa_vma_snap) * collected);
  if (!snaps) {
    kfree(vma_ptrs);
    spinlock_release(&t->mm->lock);
    return 0;
  }

  for (int i = 0; i < collected; i++) {
    struct vma *v = vma_ptrs[i];
    snaps[i].start = v->start;
    snaps[i].end = v->end;
    snaps[i].policy = v->numa_policy;
    snaps[i].nodemask = v->numa_nodemask;
    snaps[i].is_anon = (v->file_node == NULL);
  }
  uint64_t cr3 = (t->cr3 != 0) ? t->cr3 : (uint64_t)vmm_get_active_pml4();
  kfree(vma_ptrs);
  spinlock_release(&t->mm->lock);

  size_t buf_cap = 65536;
  char *buf = kmalloc(buf_cap);
  if (!buf) {
    kfree(snaps);
    return 0;
  }

  size_t pos = 0;
  uint32_t ncount = numa_node_count();

  for (int i = 0; i < collected && pos + 256 < buf_cap; i++) {
    uint32_t node_pages[MAX_NUMA_NODES] = {0};
    uint32_t anon_pages = 0;

    for (uint64_t va = snaps[i].start; va < snaps[i].end; va += PAGE_SIZE) {
      uint64_t *pte_ptr = vmm_get_pte_ptr((uint64_t *)cr3, va);
      if (pte_ptr && (*pte_ptr & PAGE_FLAG_PRESENT)) {
        uint64_t phys = *pte_ptr & PAGE_MASK;
        int nd = numa_phys_to_node(phys);
        if (nd >= 0 && nd < MAX_NUMA_NODES) {
          node_pages[nd]++;
        }
        if (snaps[i].is_anon)
          anon_pages++;
      }
    }

    const char *pstr = "default";
    if (snaps[i].policy == MPOL_BIND) pstr = "bind";
    else if (snaps[i].policy == MPOL_PREFERRED) pstr = "prefer";
    else if (snaps[i].policy == MPOL_INTERLEAVE) pstr = "interleave";

    int w = snprintf(buf + pos, buf_cap - pos, "%lx %s", (unsigned long)snaps[i].start, pstr);
    if (w > 0) pos += (size_t)w;

    if (snaps[i].is_anon) {
      w = snprintf(buf + pos, buf_cap - pos, " anon=%u dirty=%u", anon_pages, anon_pages);
      if (w > 0) pos += (size_t)w;
    }

    for (uint32_t n = 0; n < ncount && n < MAX_NUMA_NODES; n++) {
      w = snprintf(buf + pos, buf_cap - pos, " N%u=%u", n, node_pages[n]);
      if (w > 0) pos += (size_t)w;
    }

    w = snprintf(buf + pos, buf_cap - pos, " kernelpagesize_kB=4\n");
    if (w > 0) pos += (size_t)w;
  }

  kfree(snaps);

  node->length = (uint32_t)pos;
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }

  uint32_t to_copy = (uint32_t)pos - offset;
  if (to_copy > size)
    to_copy = size;

  memcpy(buffer, buf + offset, to_copy);
  kfree(buf);
  return to_copy;
}

// /proc/<pid>/fd/ support

static int procfs_pid_fd_link_readlink(vfs_node_t *node, char *buf,
                                       uint32_t size) {
  if (!procfs_pid_ptrace_access((uint32_t)node->inode)) return -13;
  uint32_t pid = (uint32_t)node->inode;
  uint32_t fd = node->impl;

  char path[256];
  if (!sched_get_fd_path_snapshot(pid, fd, path, sizeof(path)))
    return -2; // ENOENT

  uint32_t len = (uint32_t)strlen(path);
  if (len > size)
    len = size;
  memcpy(buf, path, len);
  return (int)len;
}

static vfs_node_t *procfs_pid_fd_link_get_link(vfs_node_t *node) {
  return sched_get_fd_node_snapshot((uint32_t)node->inode, node->impl);
}

static vfs_node_t *procfs_pid_fd_finddir(vfs_node_t *node, char *name) {
  if (!procfs_pid_ptrace_access(node->impl)) return NULL;
  uint32_t pid = node->impl;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    vfs_open(node);
    return node;
  }

  uint32_t fd = str_to_pid(name);
  if (fd == 0 && name[0] != '0')
    return NULL;

  char ignored[1];
  if (!sched_get_fd_path_snapshot(pid, fd, ignored, sizeof(ignored)))
    return NULL;

  vfs_node_t *link = kmalloc(sizeof(vfs_node_t));
  if (!link)
    return NULL;
  procfs_node_init(link);
  strcpy(link->name, name);
  link->flags = FS_SYMLINK;
  link->mask = 0777;
  link->inode = pid;
  link->impl = fd;
  link->readlink = procfs_pid_fd_link_readlink;
  link->get_link = procfs_pid_fd_link_get_link;
  return link;
}

static struct dirent *procfs_pid_fd_readdir(vfs_node_t *node, uint64_t index) {
  if (!procfs_pid_ptrace_access(node->impl)) return NULL;
  uint32_t pid = node->impl;

  struct dirent *d = vfs_readdir_buffer();
  memset(d, 0, sizeof(*d));

  if (index == 0) {
    strcpy(d->name, ".");
    d->ino = node->inode;
    d->d_type = DT_DIR;
    return d;
  }
  if (index == 1) {
    strcpy(d->name, "..");
    d->ino = node->inode;
    d->d_type = DT_DIR;
    return d;
  }

  uint32_t fd;
  if (!sched_get_nth_open_fd(pid, index - 2, &fd))
    return NULL;
  snprintf(d->name, sizeof(d->name), "%u", fd);
  d->ino = (pid << 16) | fd;
  d->d_type = DT_LNK; // /proc/<pid>/fd/N entries are symlinks
  return d;
}

// /proc/<pid>/fdinfo/ support
// Polkit/pkexec and systemd inspect /proc/<pid>/fdinfo/<fd> to resolve
// process metadata (specifically "Pid:\t<pid>\n" for pidfds).

static uint32_t procfs_pid_fdinfo_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_ptrace_access((uint32_t)node->inode)) return 0;
  uint32_t pid = (uint32_t)node->inode;
  uint32_t fd = node->impl;

  uint64_t file_offset = 0;
  uint32_t flags = 0;
  uint32_t ino = fd;
  bool is_pidfd = false;
  uint32_t target_pid = 0;

  if (!sched_get_fd_info_snapshot(pid, fd, &file_offset, &flags, &ino,
                                  &is_pidfd, &target_pid))
    return 0;

  char out[256];
  int pos = 0;
  if (is_pidfd) {
    pos = snprintf(out, sizeof(out),
                   "pos:\t%llu\nflags:\t0%o\nmnt_id:\t9\nino:\t%u\nPid:\t%u\nNSpid:\t%u\n",
                   (unsigned long long)file_offset, flags, ino, pid_namespace_visible(sched_get_current(), target_pid), pid_namespace_visible(sched_get_current(), target_pid));
  } else {
    pos = snprintf(out, sizeof(out),
                   "pos:\t%llu\nflags:\t0%o\nmnt_id:\t9\nino:\t%u\n",
                   (unsigned long long)file_offset, flags, ino);
  }

  if (pos < 0) return 0;
  uint32_t len = (uint32_t)pos;
  if (offset >= len) return 0;
  if (offset + size > len) size = len - offset;
  memcpy(buffer, out + offset, size);
  return size;
}

static vfs_node_t *procfs_pid_fdinfo_finddir(vfs_node_t *node, char *name) {
  if (!procfs_pid_ptrace_access(node->impl)) return NULL;
  uint32_t pid = node->impl;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    vfs_open(node);
    return node;
  }

  uint32_t fd = str_to_pid(name);
  if (fd == 0 && name[0] != '0')
    return NULL;

  uint64_t ignored_pos = 0;
  if (!sched_get_fd_info_snapshot(pid, fd, &ignored_pos, NULL, NULL, NULL, NULL))
    return NULL;

  vfs_node_t *finfo = kmalloc(sizeof(vfs_node_t));
  if (!finfo)
    return NULL;
  procfs_node_init(finfo);
  strcpy(finfo->name, name);
  finfo->flags = FS_FILE;
  finfo->mask = 0444;
  finfo->inode = pid;
  finfo->impl = fd;
  finfo->length = 128;
  finfo->read = procfs_pid_fdinfo_read;
  return finfo;
}

static struct dirent *procfs_pid_fdinfo_readdir(vfs_node_t *node, uint64_t index) {
  if (!procfs_pid_ptrace_access(node->impl)) return NULL;
  uint32_t pid = node->impl;

  struct dirent *d = vfs_readdir_buffer();
  memset(d, 0, sizeof(*d));

  if (index == 0) {
    strcpy(d->name, ".");
    d->ino = node->inode;
    d->d_type = DT_DIR;
    return d;
  }
  if (index == 1) {
    strcpy(d->name, "..");
    d->ino = node->inode;
    d->d_type = DT_DIR;
    return d;
  }

  uint32_t fd;
  if (!sched_get_nth_open_fd(pid, index - 2, &fd))
    return NULL;
  snprintf(d->name, sizeof(d->name), "%u", fd);
  d->ino = (pid << 16) | fd;
  d->d_type = DT_REG;
  return d;
}

// /proc/<pid>/ns/ support
//
// bwrap (and with it WebKitGTK's sandbox setup) opens /proc/<pid>/ns as a
// directory and fstatat()s the entries to read namespace IDs before it starts
// a sandbox; container runtimes do the same. UTS identity follows the task;
// other namespaces still have a single initial instance and cannot be cloned.
// The files have to exist and carry a stable inode number,
// because that number is how namespace identity is compared.  The entries are
// regular files rather than Linux's magic symlinks: the stat must succeed, and
// there is no meaningful target path to follow.
static const struct {
  const char *name;
  uint32_t inode;
} procfs_ns_entries[] = {
    {"cgroup", 0x4E530001}, {"ipc", 0x4E530002}, {"mnt", 0x4E530003},
    {"net", 0x4E530004},    {"pid", 0x4E530005}, {"time", 0x4E530006},
    {"user", 0x4E530007},   {"uts", 0x4E530008},
};

#define PROCFS_NS_ENTRY_COUNT \
  (sizeof(procfs_ns_entries) / sizeof(procfs_ns_entries[0]))

static uint32_t procfs_pid_ns_read(vfs_node_t *node, uint32_t offset,
                                   uint32_t size, uint8_t *buffer) {
  uint32_t idx = node->impl;
  if (idx >= PROCFS_NS_ENTRY_COUNT || !buffer)
    return 0;

  char text[48];
  int len = snprintf(text, sizeof(text), "%s:[%u]",
                     procfs_ns_entries[idx].name, node->inode);
  if (len <= 0)
    return 0;

  uint32_t total = (uint32_t)len;
  if (offset >= total)
    return 0;
  uint32_t n = total - offset;
  if (n > size)
    n = size;
  memcpy(buffer, text + offset, n);
  return n;
}

static vfs_node_t *procfs_pid_ns_finddir(vfs_node_t *node, char *name) {
  if (!procfs_pid_visible(node->impl)) return NULL;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    vfs_open(node);
    return node;
  }

  for (uint32_t i = 0; i < PROCFS_NS_ENTRY_COUNT; i++) {
    if (strcmp(name, procfs_ns_entries[i].name) != 0)
      continue;

    vfs_node_t *entry = kmalloc(sizeof(vfs_node_t));
    if (!entry)
      return NULL;
    procfs_node_init(entry);
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->flags = FS_FILE;
    entry->mask = 0444;
    entry->inode = procfs_ns_entries[i].inode;
    if (strcmp(name, "uts") == 0 || strcmp(name, "ipc") == 0 || strcmp(name, "pid") == 0 || strcmp(name, "net") == 0 || strcmp(name, "mnt") == 0) {
      struct sched_thread_snapshot snapshot;
      if (!sched_get_thread_snapshot(node->impl, &snapshot)) {
        vfs_close(entry);
        return NULL;
      }
      entry->inode = strcmp(name, "uts") == 0 ? snapshot.uts_namespace_id :
                     strcmp(name, "ipc") == 0 ? snapshot.ipc_namespace_id : strcmp(name, "pid") == 0 ? snapshot.pid_namespace_id : strcmp(name, "net") == 0 ? snapshot.net_namespace_id : snapshot.mount_namespace_id;
    }
    entry->impl = i;
    entry->length = 32;
    entry->read = procfs_pid_ns_read;
    return entry;
  }
  return NULL;
}

static struct dirent *procfs_pid_ns_readdir(vfs_node_t *node, uint64_t index) {
  if (!procfs_pid_visible(node->impl)) return NULL;
  struct dirent *d = vfs_readdir_buffer();
  memset(d, 0, sizeof(*d));

  if (index == 0) {
    strcpy(d->name, ".");
    d->ino = node->inode;
    d->d_type = DT_DIR;
    return d;
  }
  if (index == 1) {
    strcpy(d->name, "..");
    d->ino = node->inode;
    d->d_type = DT_DIR;
    return d;
  }

  uint32_t i = index - 2;
  if (i >= PROCFS_NS_ENTRY_COUNT)
    return NULL;
  strncpy(d->name, procfs_ns_entries[i].name, sizeof(d->name) - 1);
  d->ino = procfs_ns_entries[i].inode;
  if (strcmp(procfs_ns_entries[i].name, "uts") == 0 ||
      strcmp(procfs_ns_entries[i].name, "ipc") == 0 || strcmp(procfs_ns_entries[i].name, "pid") == 0 || strcmp(procfs_ns_entries[i].name, "net") == 0 || strcmp(procfs_ns_entries[i].name, "mnt") == 0) {
    struct sched_thread_snapshot snapshot;
    if (!sched_get_thread_snapshot(node->impl, &snapshot)) return NULL;
    d->ino = strcmp(procfs_ns_entries[i].name, "uts") == 0 ? snapshot.uts_namespace_id :
             strcmp(procfs_ns_entries[i].name, "ipc") == 0 ? snapshot.ipc_namespace_id : strcmp(procfs_ns_entries[i].name, "pid") == 0 ? snapshot.pid_namespace_id : strcmp(procfs_ns_entries[i].name, "net") == 0 ? snapshot.net_namespace_id : snapshot.mount_namespace_id;
  }
  d->d_type = DT_REG;
  return d;
}

// Synthesise a /proc/<pid>/ directory node on demand

typedef struct procfs_pid_cache_entry {
  uint32_t pid;
  vfs_node_t *dir;
  struct procfs_pid_cache_entry *next;
} procfs_pid_cache_entry_t;

static procfs_pid_cache_entry_t *procfs_pid_cache;
static spinlock_t procfs_pid_cache_lock = SPINLOCK_INIT;

/* Dynamic PID trees own one reference to every contained node. Unlike the
 * normal persistent ramfs nodes, lookups need a separate caller reference. */
static vfs_node_t *procfs_pid_ramfs_finddir(vfs_node_t *node, char *name) {
  if (!node || !procfs_pid_visible(node->impl)) return NULL;
  if (!node || !node->device)
    return NULL;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    vfs_open(node);
    return node;
  }

  ramfs_dir_t *dir = (ramfs_dir_t *)node->device;
  for (child_node_t *child = dir->children; child; child = child->next) {
    if (strcmp(child->node->name, name) == 0) {
      vfs_open(child->node);
      return child->node;
    }
  }
  return NULL;
}

static void procfs_pid_dir_close(vfs_node_t *node) {
  ramfs_dir_t *dir = (ramfs_dir_t *)node->device;
  if (!dir)
    return;

  node->device = NULL;
  child_node_t *child = dir->children;
  while (child) {
    child_node_t *next = child->next;
    vfs_close(child->node);
    kfree(child);
    child = next;
  }
  kfree(dir);
}

static int procfs_pid_exe_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  if (!procfs_pid_visible(node->impl)) return -2;
  uint32_t pid = node->impl;
  struct sched_thread_snapshot snapshot;
  if (!sched_get_thread_snapshot(pid, &snapshot))
    return -2; // ENOENT
  const char *target = snapshot.exe_path[0] ? snapshot.exe_path : "/init";
  uint32_t len = (uint32_t)strlen(target);
  if (len > size)
    len = size;
  memcpy(buf, target, len);
  return (int)len;
}

static int procfs_pid_cwd_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  if (!procfs_pid_visible(node->impl)) return -2;
  uint32_t pid = node->impl;
  struct sched_thread_snapshot snapshot;
  if (!sched_get_thread_snapshot(pid, &snapshot))
    return -2; // ENOENT
  const char *target = snapshot.cwd_path[0] ? snapshot.cwd_path : "/";
  uint32_t len = (uint32_t)strlen(target);
  if (len > size)
    len = size;
  memcpy(buf, target, len);
  return (int)len;
}

// root symlink — /proc/<pid>/root. AvoryOS has a single global root
// (chroot confinement is unimplemented and returns EPERM), so the target
// is always "/". Needed by pipewire (peer-root checks via /proc/<pid>/root),
// containers, and anything resolving fds across processes.
static int procfs_pid_root_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  if (!procfs_pid_visible(node->impl)) return -2;
  uint32_t pid = node->impl;
  struct sched_thread_snapshot snapshot;
  if (!sched_get_thread_snapshot(pid, &snapshot))
    return -2; // ENOENT
  if (size < 1)
    return 0;
  buf[0] = '/';
  return 1;
}

/* /proc/<pid>/mountinfo — libmount's kernel monitor opens _PATH_PROC_MOUNTINFO
 * (/proc/self/mountinfo). Without it open() fails with ENOENT and every KDE
 * Solid / systemsettings mount query takes untested error paths. Emit minimal
 * mountinfo-format lines from the same table as /proc/mounts. */
static uint32_t procfs_pid_mountinfo_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  (void)node;
  const size_t cap = PROCFS_MOUNTINFO_BUFSZ;
  char *buf = kmalloc(cap);
  if (!buf)
    return 0;

  /* Heap-allocate the namespace mount table to keep it off the stack. */
  vfs_mount_info_t *mounts =
      kmalloc(sizeof(vfs_mount_info_t) * PROCFS_MAX_MOUNTS);
  if (!mounts) {
    kfree(buf);
    return 0;
  }
  int count = sched_get_mount_snapshot(node->impl, mounts, PROCFS_MAX_MOUNTS);
  size_t pos = 0;

  for (int i = 0; i < count && pos < cap - 256; i++) {
    const char *mnt = mounts[i].mountpoint[0] ? mounts[i].mountpoint : "/";
    const char *rawdev = mounts[i].dev_name[0] ? mounts[i].dev_name : "none";
    const char *fst = mounts[i].fs_type[0] ? mounts[i].fs_type : "auto";
    const char *root = "/";
    /* Source must be the /dev/ path UDisks advertises ("/dev/nvme0n1p1"),
     * not the bare kernel name, or Solid's libmount string match
     * (mnt_fs_get_srcpath == Device) fails and the volume gets no
     * MountPoints. Devno must be the real block major:minor for the same
     * reason; pseudo filesystems keep 0:N like Linux. */
    char srcbuf[80];
    const char *dev = procfs_mount_src(rawdev, srcbuf, sizeof(srcbuf));
    uint32_t major = 0, minor = (uint32_t)(i + 1);
    struct block_device *bd = NULL;
    if (rawdev[0] != '/' && strcmp(rawdev, "none") != 0)
      bd = block_get_by_name(rawdev);
    if (bd) {
      /* procfs_mount_src already rewrote dev to /dev/<name> when bd exists. */
      major = bd->linux_major;
      minor = bd->linux_minor;
    }
    /* id parent major:minor root mountpoint opts optional - fstype src super */
    char options[80]; procfs_mount_options(mounts[i].flags, options);
    int w = snprintf(buf + pos, cap - pos,
        "%llu %llu %u:%u %s %s %s - %s %s %s\n",
        (unsigned long long)mounts[i].mount_id,
        (unsigned long long)mounts[i].parent_id, major, minor, root, mnt, options, fst, dev,
        mounts[i].flags & 1 ? "ro" : "rw");
    if (w < 0 || (size_t)w >= cap - pos)
      break;
    pos += (size_t)w;
  }

  if (count == 0 && pos < cap - 128) {
    int w = snprintf(buf + pos, cap - pos,
                     "1 1 0:1 / / rw,relatime - ext4 none rw\n");
    if (w > 0 && (size_t)w < cap - pos)
      pos += (size_t)w;
  }

  kfree(mounts);
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)pos)
    size = (uint32_t)pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

/* /proc/<pid>/mountstats — Linux exposes this alongside mountinfo; tools
 * that poll per-mount I/O (nfsiostat-style monitors, some Qt storage
 * probes) open it unconditionally and fall into error paths on ENOENT.
 * Emit the standard "device ... mounted on ... with fstype ..." line per
 * mount so parsers see a Linux-shaped file. Usage numbers come from
 * statfs; this file stays strictly compatible. */
static uint32_t procfs_pid_mountstats_read(vfs_node_t *node, uint32_t offset,
                                           uint32_t size, uint8_t *buffer) {
  if (!procfs_pid_visible(node->impl)) return 0;
  (void)node;
  const size_t cap = PROCFS_MOUNTINFO_BUFSZ;
  char *buf = kmalloc(cap);
  if (!buf)
    return 0;

  /* Heap-allocate the namespace mount table to keep it off the stack. */
  vfs_mount_info_t *mounts =
      kmalloc(sizeof(vfs_mount_info_t) * PROCFS_MAX_MOUNTS);
  if (!mounts) {
    kfree(buf);
    return 0;
  }
  int count = sched_get_mount_snapshot(node->impl, mounts, PROCFS_MAX_MOUNTS);
  size_t pos = 0;

  for (int i = 0; i < count && pos < cap - 256; i++) {
    const char *mnt = mounts[i].mountpoint[0] ? mounts[i].mountpoint : "/";
    const char *fst = mounts[i].fs_type[0] ? mounts[i].fs_type : "auto";
    const char *rawsrc =
        mounts[i].dev_name[0] ? mounts[i].dev_name : "none";
    char srcbuf[80];
    const char *src = procfs_mount_src(rawsrc, srcbuf, sizeof(srcbuf));
    int w = snprintf(buf + pos, cap - pos,
        "device %s mounted on %s with fstype %s\n",
        src, mnt, fst);
    if (w < 0 || (size_t)w >= cap - pos)
      break;
    pos += (size_t)w;
  }

  if (count == 0 && pos < cap - 64) {
    int w = snprintf(buf + pos, cap - pos,
                     "device none mounted on / with fstype ext4\n");
    if (w > 0 && (size_t)w < cap - pos)
      pos += (size_t)w;
  }

  kfree(mounts);
  if (offset >= (uint32_t)pos) {
    kfree(buf);
    return 0;
  }
  if (offset + size > (uint32_t)pos)
    size = (uint32_t)pos - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

static int procfs_thread_self_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  (void)node;
  struct thread *t = sched_get_current();
  if (!t)
    return -1;
  char link_str[32];
  uint32_t tgid = pid_namespace_visible(t, t->tgid ? t->tgid : t->tid);
  snprintf(link_str, sizeof(link_str), "%u/task/%u", tgid, pid_namespace_tid(t, t));
  uint32_t len = (uint32_t)strlen(link_str);
  if (len > size)
    len = size;
  memcpy(buf, link_str, len);
  return (int)len;
}

static void procfs_make_pid_ramfs_ephemeral(vfs_node_t *dir) {
  /* ramfs_mount_on() defaults to persistent nodes. The proc cache owns these
   * instead, while open paths/FDs may keep them alive past cache removal. */
  dir->flags &= ~FS_PERSISTENT;
  dir->finddir = procfs_pid_ramfs_finddir;
  dir->close = procfs_pid_dir_close;
}

/* Linux's task directory has two base links plus one per live thread.
 * Chromium uses this count to check that sandbox setup is single-threaded.
 * Both lookup and readdir must stay live even through an already-open fd. */
static uint32_t procfs_task_nlink(vfs_node_t *node) {
  return 2 + sched_get_group_threads(node->impl, 0, NULL);
}

static struct dirent *procfs_task_readdir(vfs_node_t *node, uint64_t index) {
  struct dirent *dent = vfs_readdir_buffer();
  memset(dent, 0, sizeof(*dent));
  dent->d_type = DT_DIR;
  if (index < 2) {
    strcpy(dent->name, index ? ".." : ".");
    dent->ino = node->inode;
  } else {
    uint32_t tid;
    sched_get_group_threads(node->impl, index - 2, &tid);
    if (!tid) return NULL;
    snprintf(dent->name, sizeof(dent->name), "%u", pid_namespace_visible(sched_get_current(), tid));
    dent->ino = 0x10000 + tid;
  }
  return dent;
}

static vfs_node_t *procfs_task_finddir(vfs_node_t *node, char *name) {
  uint32_t tid = pid_namespace_global(sched_get_current(), str_to_pid(name));
  struct sched_thread_snapshot snap;
  if (!tid || !sched_get_thread_snapshot(tid, &snap) ||
      (snap.tgid ? snap.tgid : snap.tid) != node->impl ||
      snap.state == THREAD_DEAD || snap.state == THREAD_ZOMBIE)
    return NULL;

  vfs_node_t *dir = kmalloc(sizeof(*dir));
  if (!dir) return NULL;
  procfs_node_init(dir);
  snprintf(dir->name, sizeof(dir->name), "%u", tid);
  dir->flags = FS_DIRECTORY;
  dir->mask = 0555;
  ramfs_mount_on(dir);
  dir->impl = tid;
  procfs_make_pid_ramfs_ephemeral(dir);
  dir->flags |= FS_DENTRY_NOCACHE;
  vfs_node_t *stat = kmalloc(sizeof(*stat));
  if (stat) {
    procfs_node_init(stat);
    strcpy(stat->name, "stat");
    stat->flags = FS_FILE;
    stat->mask = 0444;
    stat->impl = tid;
    stat->length = 128;
    stat->read = procfs_pid_stat_read;
    ramfs_mount_node(dir, stat);
  }
  vfs_node_t *status = kmalloc(sizeof(*status));
  if (status) {
    procfs_node_init(status);
    strcpy(status->name, "status");
    status->flags = FS_FILE;
    status->mask = 0444;
    status->impl = tid;
    status->length = 768;
    status->read = procfs_pid_status_read;
    ramfs_mount_node(dir, status);
  }
  return dir;
}

static vfs_node_t *make_pid_dir(uint32_t pid) {
  vfs_node_t *dir = kmalloc(sizeof(vfs_node_t));
  if (!dir)
    return NULL;
  procfs_node_init(dir);
  snprintf(dir->name, sizeof(dir->name), "%u", pid);
  dir->flags = FS_DIRECTORY; // not FS_PERSISTENT — ephemeral
  dir->mask = 0555;
  dir->inode = 0x10000 + pid;
  ramfs_mount_on(dir);
  dir->impl = pid;
  procfs_make_pid_ramfs_ephemeral(dir);
  dir->flags |= FS_DENTRY_NOCACHE;

  // stat
  vfs_node_t *stat_node = kmalloc(sizeof(vfs_node_t));
  if (stat_node) {
    procfs_node_init(stat_node);
    strcpy(stat_node->name, "stat");
    stat_node->flags = FS_FILE;
    stat_node->mask = 0444;
    stat_node->impl = pid; // stash PID for the read callback
    stat_node->length = 128;
    stat_node->read = procfs_pid_stat_read;
    ramfs_mount_node(dir, stat_node);
  }

  // status
  vfs_node_t *status_node = kmalloc(sizeof(vfs_node_t));
  if (status_node) {
    procfs_node_init(status_node);
    strcpy(status_node->name, "status");
    status_node->flags = FS_FILE;
    status_node->mask = 0444;
    status_node->impl = pid;
    status_node->length = 256;
    status_node->read = procfs_pid_status_read;
    ramfs_mount_node(dir, status_node);
  }

  // syscall - last syscall number/args/return for spin diagnosis.
  // Sample twice: a stable nr with an instant-return error names the
  // spinning call without needing a SYSCALL_LOG rebuild.
  vfs_node_t *syscall_node = kmalloc(sizeof(vfs_node_t));
  if (syscall_node) {
    procfs_node_init(syscall_node);
    strcpy(syscall_node->name, "syscall");
    syscall_node->flags = FS_FILE;
    syscall_node->mask = 0444;
    syscall_node->impl = pid;
    syscall_node->length = 256;
    syscall_node->read = procfs_pid_syscall_read;
    ramfs_mount_node(dir, syscall_node);
  }

  // cmdline
  vfs_node_t *cmdline_node = kmalloc(sizeof(vfs_node_t));
  if (cmdline_node) {
    procfs_node_init(cmdline_node);
    strcpy(cmdline_node->name, "cmdline");
    cmdline_node->flags = FS_FILE;
    cmdline_node->mask = 0444;
    cmdline_node->impl = pid;
    cmdline_node->length = 256;
    cmdline_node->read = procfs_pid_cmdline_read;
    ramfs_mount_node(dir, cmdline_node);
  }

  // comm — what pgrep/pkill match without -f (distinct from cmdline)
  vfs_node_t *comm_node = kmalloc(sizeof(vfs_node_t));
  if (comm_node) {
    procfs_node_init(comm_node);
    strcpy(comm_node->name, "comm");
    comm_node->flags = FS_FILE;
    comm_node->mask = 0644;
    comm_node->impl = pid;
    comm_node->length = 16;
    comm_node->read = procfs_pid_comm_read;
    comm_node->write = procfs_pid_comm_write;
    ramfs_mount_node(dir, comm_node);
  }

  // cgroup — hybrid v1+v2 lines (elogind daemon + sessions)
  vfs_node_t *cgroup_node = kmalloc(sizeof(vfs_node_t));
  if (cgroup_node) {
    procfs_node_init(cgroup_node);
    strcpy(cgroup_node->name, "cgroup");
    cgroup_node->flags = FS_FILE;
    cgroup_node->mask = 0444;
    cgroup_node->impl = pid;
    cgroup_node->length = 74;
    cgroup_node->read = procfs_pid_cgroup_read;
    ramfs_mount_node(dir, cgroup_node);
  }

  // statm — memory usage in pages (VIRT/RES for htop)
  vfs_node_t *statm_node = kmalloc(sizeof(vfs_node_t));
  if (statm_node) {
    procfs_node_init(statm_node);
    strcpy(statm_node->name, "statm");
    statm_node->flags = FS_FILE;
    statm_node->mask = 0444;
    statm_node->impl = pid;
    statm_node->length = 64;
    statm_node->read = procfs_pid_statm_read;
    ramfs_mount_node(dir, statm_node);
  }

  // io — I/O stats stub (htop 3.x tries to open this)
  vfs_node_t *io_node = kmalloc(sizeof(vfs_node_t));
  if (io_node) {
    procfs_node_init(io_node);
    strcpy(io_node->name, "io");
    io_node->flags = FS_FILE;
    io_node->mask = 0444;
    io_node->impl = pid;
    io_node->length = 64;
    io_node->read = procfs_pid_io_read;
    ramfs_mount_node(dir, io_node);
  }

  // setgroups — used by bwrap/flatpak/containers before mapping IDs
  vfs_node_t *setgroups_node = kmalloc(sizeof(vfs_node_t));
  if (setgroups_node) {
    procfs_node_init(setgroups_node);
    strcpy(setgroups_node->name, "setgroups");
    setgroups_node->flags = FS_FILE;
    setgroups_node->mask = 0644;
    setgroups_node->impl = pid;
    setgroups_node->length = 6;
    setgroups_node->read = procfs_pid_setgroups_read;
    setgroups_node->write = procfs_pid_setgroups_write;
    ramfs_mount_node(dir, setgroups_node);
  }

  // uid_map — UID mapping for user namespaces
  vfs_node_t *uid_map_node = kmalloc(sizeof(vfs_node_t));
  if (uid_map_node) {
    procfs_node_init(uid_map_node);
    strcpy(uid_map_node->name, "uid_map");
    uid_map_node->flags = FS_FILE;
    uid_map_node->mask = 0644;
    uid_map_node->impl = pid;
    uid_map_node->length = 34;
    uid_map_node->read = procfs_pid_id_map_read;
    uid_map_node->write = procfs_pid_id_map_write;
    ramfs_mount_node(dir, uid_map_node);
  }

  // gid_map — GID mapping for user namespaces
  vfs_node_t *gid_map_node = kmalloc(sizeof(vfs_node_t));
  if (gid_map_node) {
    procfs_node_init(gid_map_node);
    strcpy(gid_map_node->name, "gid_map");
    gid_map_node->flags = FS_FILE;
    gid_map_node->mask = 0644;
    gid_map_node->impl = pid;
    gid_map_node->length = 34;
    gid_map_node->read = procfs_pid_id_map_read;
    gid_map_node->write = procfs_pid_id_map_write;
    ramfs_mount_node(dir, gid_map_node);
  }

  /* No OOM victim-selection policy is implemented yet. Expose the neutral
   * score so memory monitors can probe it without a missing-file loop. */
  vfs_node_t *score_node = kmalloc(sizeof(vfs_node_t));
  if (score_node) {
    procfs_node_init(score_node);
    strcpy(score_node->name, "oom_score");
    score_node->flags = FS_FILE;
    score_node->mask = 0444;
    score_node->impl = pid;
    score_node->length = 2;
    score_node->read = procfs_pid_oom_score_adj_read;
    ramfs_mount_node(dir, score_node);
  }

  // oom_score_adj — process OOM killer score adjustment
  vfs_node_t *oom_node = kmalloc(sizeof(vfs_node_t));
  if (oom_node) {
    procfs_node_init(oom_node);
    strcpy(oom_node->name, "oom_score_adj");
    oom_node->flags = FS_FILE;
    oom_node->mask = 0644;
    oom_node->impl = pid;
    oom_node->length = 2;
    oom_node->read = procfs_pid_oom_score_adj_read;
    oom_node->write = procfs_pid_oom_score_adj_write;
    ramfs_mount_node(dir, oom_node);
  }

  // auxv — ELF auxiliary vector
  vfs_node_t *auxv_node = kmalloc(sizeof(vfs_node_t));
  if (auxv_node) {
    procfs_node_init(auxv_node);
    strcpy(auxv_node->name, "auxv");
    auxv_node->flags = FS_FILE;
    auxv_node->mask = 0444;
    auxv_node->impl = pid;
    auxv_node->length = 512;
    auxv_node->read = procfs_pid_auxv_read;
    ramfs_mount_node(dir, auxv_node);
  }

  // maps — memory regions / VMAs
  vfs_node_t *maps_node = kmalloc(sizeof(vfs_node_t));
  if (maps_node) {
    procfs_node_init(maps_node);
    strcpy(maps_node->name, "maps");
    maps_node->flags = FS_FILE;
    maps_node->mask = 0444;
    maps_node->impl = pid;
    maps_node->length = 4096;
    maps_node->read = procfs_pid_maps_read;
    ramfs_mount_node(dir, maps_node);
  }

  // numa_maps — per-node NUMA page distribution
  vfs_node_t *numa_maps_node = kmalloc(sizeof(vfs_node_t));
  if (numa_maps_node) {
    procfs_node_init(numa_maps_node);
    strcpy(numa_maps_node->name, "numa_maps");
    numa_maps_node->flags = FS_FILE;
    numa_maps_node->mask = 0444;
    numa_maps_node->impl = pid;
    numa_maps_node->length = 4096;
    numa_maps_node->read = procfs_pid_numa_maps_read;
    ramfs_mount_node(dir, numa_maps_node);
  }

  // mountinfo — libmount kernel monitor needs /proc/self/mountinfo.
  // /proc/self is a symlink to /proc/<pid>, so this serves both.
  vfs_node_t *mountinfo_node = kmalloc(sizeof(vfs_node_t));
  if (mountinfo_node) {
    procfs_node_init(mountinfo_node);
    strcpy(mountinfo_node->name, "mountinfo");
    mountinfo_node->flags = FS_FILE;
    mountinfo_node->mask = 0444;
    mountinfo_node->impl = pid;
    mountinfo_node->length = 65536;
    mountinfo_node->read = procfs_pid_mountinfo_read;
    ramfs_mount_node(dir, mountinfo_node);
  }

  // mountstats — Linux exposes /proc/self/mountstats alongside mountinfo.
  // Per-mount I/O monitors open it unconditionally; without it they take
  // ENOENT error paths and report no disks.
  vfs_node_t *mountstats_node = kmalloc(sizeof(vfs_node_t));
  if (mountstats_node) {
    procfs_node_init(mountstats_node);
    strcpy(mountstats_node->name, "mountstats");
    mountstats_node->flags = FS_FILE;
    mountstats_node->mask = 0444;
    mountstats_node->impl = pid;
    mountstats_node->length = 65536;
    mountstats_node->read = procfs_pid_mountstats_read;
    ramfs_mount_node(dir, mountstats_node);
  }

  // mounts — same table as global /proc/mounts, per-pid alias
  vfs_node_t *pid_mounts_node = kmalloc(sizeof(vfs_node_t));
  if (pid_mounts_node) {
    procfs_node_init(pid_mounts_node);
    strcpy(pid_mounts_node->name, "mounts");
    pid_mounts_node->flags = FS_FILE;
    pid_mounts_node->mask = 0444;
    pid_mounts_node->impl = pid;
    pid_mounts_node->length = 65536;
    pid_mounts_node->read = procfs_mounts_read;
    ramfs_mount_node(dir, pid_mounts_node);
  }

  // fd directory
  vfs_node_t *fd_dir = kmalloc(sizeof(vfs_node_t));
  if (fd_dir) {
    procfs_node_init(fd_dir);
    strcpy(fd_dir->name, "fd");
    fd_dir->flags = FS_DIRECTORY | FS_DENTRY_NOCACHE;
    fd_dir->mask = 0555;
    fd_dir->impl = pid;
    fd_dir->readdir = procfs_pid_fd_readdir;
    fd_dir->finddir = procfs_pid_fd_finddir;
    ramfs_mount_node(dir, fd_dir);
  }

  // fdinfo directory
  vfs_node_t *fdinfo_dir = kmalloc(sizeof(vfs_node_t));
  if (fdinfo_dir) {
    procfs_node_init(fdinfo_dir);
    strcpy(fdinfo_dir->name, "fdinfo");
    fdinfo_dir->flags = FS_DIRECTORY | FS_DENTRY_NOCACHE;
    fdinfo_dir->mask = 0555;
    fdinfo_dir->impl = pid;
    fdinfo_dir->readdir = procfs_pid_fdinfo_readdir;
    fdinfo_dir->finddir = procfs_pid_fdinfo_finddir;
    ramfs_mount_node(dir, fdinfo_dir);
  }

  // ns directory — namespace identity files.  bwrap opens this directory and
  // fstatat()s the entries before starting a sandbox; without it the sandbox
  // launcher dies with "open /proc/<pid>/ns failed" before it can fall back.
  vfs_node_t *ns_dir = kmalloc(sizeof(vfs_node_t));
  if (ns_dir) {
    procfs_node_init(ns_dir);
    strcpy(ns_dir->name, "ns");
    ns_dir->flags = FS_DIRECTORY | FS_DENTRY_NOCACHE;
    ns_dir->mask = 0555;
    ns_dir->impl = pid;
    ns_dir->readdir = procfs_pid_ns_readdir;
    ns_dir->finddir = procfs_pid_ns_finddir;
    ramfs_mount_node(dir, ns_dir);
  }

  // task/<tid>/ entries follow live thread-group membership.
  vfs_node_t *task_dir = kmalloc(sizeof(vfs_node_t));
  if (task_dir) {
    procfs_node_init(task_dir);
    strcpy(task_dir->name, "task");
    task_dir->flags = FS_DIRECTORY;
    task_dir->mask = 0555;
    ramfs_mount_on(task_dir);
    procfs_make_pid_ramfs_ephemeral(task_dir);
    task_dir->flags |= FS_DENTRY_NOCACHE;

    struct sched_thread_snapshot snap;
    task_dir->impl = sched_get_thread_snapshot(pid, &snap) && snap.tgid
                         ? snap.tgid : pid;
    task_dir->get_nlink = procfs_task_nlink;
    task_dir->readdir = procfs_task_readdir;
    task_dir->finddir = procfs_task_finddir;

    ramfs_mount_node(dir, task_dir);
  }

  // exe symlink — /proc/<pid>/exe
  vfs_node_t *exe_link = kmalloc(sizeof(vfs_node_t));
  if (exe_link) {
    procfs_node_init(exe_link);
    strcpy(exe_link->name, "exe");
    exe_link->flags = FS_SYMLINK;
    exe_link->mask = 0777;
    exe_link->impl = pid;
    exe_link->readlink = procfs_pid_exe_readlink;
    ramfs_mount_node(dir, exe_link);
  }

  // cwd symlink — /proc/<pid>/cwd
  vfs_node_t *cwd_link = kmalloc(sizeof(vfs_node_t));
  if (cwd_link) {
    procfs_node_init(cwd_link);
    strcpy(cwd_link->name, "cwd");
    cwd_link->flags = FS_SYMLINK;
    cwd_link->mask = 0777;
    cwd_link->impl = pid;
    cwd_link->readlink = procfs_pid_cwd_readlink;
    ramfs_mount_node(dir, cwd_link);
  }

  // root symlink — /proc/<pid>/root
  vfs_node_t *root_link = kmalloc(sizeof(vfs_node_t));
  if (root_link) {
    procfs_node_init(root_link);
    strcpy(root_link->name, "root");
    root_link->flags = FS_SYMLINK;
    root_link->mask = 0777;
    root_link->impl = pid;
    root_link->readlink = procfs_pid_root_readlink;
    ramfs_mount_node(dir, root_link);
  }

  return dir;
}

static vfs_node_t *procfs_get_pid_dir(uint32_t pid) {
  spinlock_acquire(&procfs_pid_cache_lock);
  for (procfs_pid_cache_entry_t *e = procfs_pid_cache; e; e = e->next) {
    if (e->pid == pid) {
      vfs_node_t *dir = e->dir;
      vfs_open(dir);
      spinlock_release(&procfs_pid_cache_lock);
      return dir;
    }
  }
  spinlock_release(&procfs_pid_cache_lock);

  vfs_node_t *new_dir = make_pid_dir(pid);
  if (!new_dir)
    return NULL;
  procfs_pid_cache_entry_t *new_entry =
      kmalloc(sizeof(procfs_pid_cache_entry_t));
  if (!new_entry) {
    vfs_close(new_dir);
    return NULL;
  }

  spinlock_acquire(&procfs_pid_cache_lock);
  /* Another CPU may have populated this PID while allocations were made. */
  for (procfs_pid_cache_entry_t *e = procfs_pid_cache; e; e = e->next) {
    if (e->pid == pid) {
      vfs_node_t *dir = e->dir;
      vfs_open(dir);
      spinlock_release(&procfs_pid_cache_lock);
      vfs_close(new_dir);
      kfree(new_entry);
      return dir;
    }
  }
  new_entry->pid = pid;
  new_entry->dir = new_dir;
  new_entry->next = procfs_pid_cache;
  procfs_pid_cache = new_entry;
  vfs_open(new_dir); /* reference returned to the path walker */
  spinlock_release(&procfs_pid_cache_lock);
  return new_dir;
}

void procfs_release_pid_dir(uint32_t pid) {
  procfs_pid_cache_entry_t *victim = NULL;

  spinlock_acquire(&procfs_pid_cache_lock);
  procfs_pid_cache_entry_t **link = &procfs_pid_cache;
  while (*link) {
    if ((*link)->pid == pid) {
      victim = *link;
      *link = victim->next;
      break;
    }
    link = &(*link)->next;
  }
  spinlock_release(&procfs_pid_cache_lock);

  if (victim) {
    /* Drop the cache reference. Open paths and descriptors keep the tree
     * alive until their final vfs_close(). */
    vfs_close(victim->dir);
    kfree(victim);
  }
}

static int procfs_self_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  (void)node;
  struct thread *t = sched_get_current();
  if (!t)
    return -1;
  char pid_str[16];
  uint32_t pid = pid_namespace_visible(t, t->tgid ? t->tgid : t->tid);
  snprintf(pid_str, sizeof(pid_str), "%u", pid);
  uint32_t len = (uint32_t)strlen(pid_str);
  if (len > size)
    len = size;
  memcpy(buf, pid_str, len);
  return (int)len;
}

// Custom readdir for /proc
//
// Index layout:
//   0        → "."
//   1        → ".."
//   2..N+1   → static ramfs children (all nodes mounted in procfs_root)
//   N+2 ..   → live PID entries (one per thread in global_thread_list)


static struct dirent *procfs_root_readdir(vfs_node_t *node, uint64_t index) {
  struct dirent *procfs_dent = vfs_readdir_buffer();
  memset(procfs_dent, 0, sizeof(*procfs_dent));

  if (index == 0) {
    strcpy(procfs_dent->name, ".");
    procfs_dent->ino = node->inode;
    procfs_dent->d_type = DT_DIR;
    return procfs_dent;
  }
  if (index == 1) {
    strcpy(procfs_dent->name, "..");
    procfs_dent->ino = node->inode;
    procfs_dent->d_type = DT_DIR;
    return procfs_dent;
  }

  // Walk the ramfs child list
  typedef struct child_node_s {
    vfs_node_t *node;
    struct child_node_s *next;
  } child_node_t;
  typedef struct {
    child_node_t *children;
  } ramfs_dir_t;
  ramfs_dir_t *rdir = (ramfs_dir_t *)node->device;
  if (!rdir)
    return NULL;

  uint32_t static_idx = index - 2;
  child_node_t *curr = rdir->children;
  for (uint32_t i = 0; i < static_idx && curr; i++)
    curr = curr->next;
  if (curr) {
    strcpy(procfs_dent->name, curr->node->name);
    procfs_dent->ino = curr->node->inode;
    procfs_dent->d_type = vfs_dtype(curr->node->flags);
    return procfs_dent;
  }

  // Count total static children
  uint32_t num_static = 0;
  for (child_node_t *c = rdir->children; c; c = c->next)
    num_static++;

  // PID entries
  uint32_t pid;
  if (!pid_namespace_nth(sched_get_current(), static_idx - num_static, &pid))
    return NULL;
  snprintf(procfs_dent->name, sizeof(procfs_dent->name), "%u", pid_namespace_visible(sched_get_current(), pid));
  procfs_dent->ino = 0x10000 + pid;
  procfs_dent->d_type = DT_DIR;
  return procfs_dent;
}

// Custom finddir for /proc

static vfs_node_t *procfs_root_finddir(vfs_node_t *node, char *name) {
  if (strcmp(name, "self") == 0) {
    vfs_node_t *self_link = kmalloc(sizeof(vfs_node_t));
    if (!self_link)
      return NULL;
    procfs_node_init(self_link);
    strcpy(self_link->name, "self");
    self_link->flags = FS_SYMLINK;
    self_link->mask = 0777;
    self_link->readlink = procfs_self_readlink;
    return self_link;
  }
  if (strcmp(name, "thread-self") == 0) {
    vfs_node_t *tself_link = kmalloc(sizeof(vfs_node_t));
    if (!tself_link)
      return NULL;
    procfs_node_init(tself_link);
    strcpy(tself_link->name, "thread-self");
    tself_link->flags = FS_SYMLINK;
    tself_link->mask = 0777;
    tself_link->readlink = procfs_thread_self_readlink;
    return tself_link;
  }

  // First try the static ramfs children
  typedef struct child_node_s {
    vfs_node_t *node;
    struct child_node_s *next;
  } child_node_t;
  typedef struct {
    child_node_t *children;
  } ramfs_dir_t;
  ramfs_dir_t *rdir = (ramfs_dir_t *)node->device;
  if (rdir) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      vfs_open(node);
      return node;
    }
    child_node_t *curr = rdir->children;
    while (curr) {
      if (strcmp(curr->node->name, name) == 0) {
        vfs_open(curr->node);
        return curr->node;
      }
      curr = curr->next;
    }
  }

  // Check if it's a numeric PID
  uint32_t pid = pid_namespace_global(sched_get_current(), str_to_pid(name));
  if (pid == 0)
    return NULL;

  // Verify the thread actually exists
  struct sched_thread_snapshot snapshot;
  if (!sched_get_thread_snapshot(pid, &snapshot))
    return NULL;

  // Reuse one generated tree for the lifetime of this task. htop polls these
  // paths every refresh; rebuilding the tree here leaked kernel heap steadily.
  return procfs_get_pid_dir(pid);
}

void procfs_init(void) {
  if (!fs_root)
    return;

  // Try to find /proc, create if it doesn't exist (assuming ext2/3 supports
  // root->mkdir)
  vfs_node_t *proc_dir = vfs_finddir(fs_root, "proc");
  if (!proc_dir && fs_root->mkdir) {
    fs_root->mkdir(fs_root, "proc", 0755);
    proc_dir = vfs_finddir(fs_root, "proc");
  }

  if (proc_dir) {
    // Stage a new virtual root for procfs
    vfs_node_t *procfs_root = kmalloc(sizeof(vfs_node_t));
    procfs_node_init(procfs_root);
    strcpy(procfs_root->name, "proc");
    procfs_root->flags = FS_DIRECTORY;
    procfs_root->mask = 0555;
    ramfs_mount_on(procfs_root);
    procfs_root->statfs = procfs_statfs;

    extern const char *kernel_boot_cmdline;
    const char *diag_option = kernel_boot_cmdline;
    while (diag_option && *diag_option) {
      if ((diag_option == kernel_boot_cmdline || diag_option[-1] == ' ') &&
          !strncmp(diag_option, "gpu_diag=0", 10) &&
          (!diag_option[10] || diag_option[10] == ' ')) {
        gpu_diag_control(1, (const unsigned char *)"0");
        break;
      }
      diag_option++;
    }
    vfs_open(procfs_root); // Permanent reference for the mount entry

    // Apply the mount: anyone looking up 'proc' will now get our virtual root
    vfs_mount_ex(proc_dir, procfs_root, "proc", "proc", "/proc");
    vfs_close(proc_dir);

    // Add /proc/meminfo
    vfs_node_t *gpu_debug_node=kmalloc(sizeof(vfs_node_t));
    if (gpu_debug_node) {
      procfs_node_init(gpu_debug_node);
      strncpy(gpu_debug_node->name,"gpu_debug",127);
      gpu_debug_node->flags=FS_FILE|FS_PERSISTENT;
      gpu_debug_node->mask=0600;
      gpu_debug_node->read=procfs_gpu_debug_read;
      gpu_debug_node->write=procfs_gpu_debug_write;
      gpu_debug_node->length=512*1024;
      ramfs_mount_node(procfs_root,gpu_debug_node);
      klog_puts("[GPUDBG] native recorder active; /proc/gpu_debug (1 metadata, 2 pixel checks, dump freezes to serial)\n");
    }

    // Add /proc/meminfo
    vfs_node_t *meminfo_node = kmalloc(sizeof(vfs_node_t));
    if (meminfo_node) {
      procfs_node_init(meminfo_node);
      strncpy(meminfo_node->name, "meminfo", 127);
      meminfo_node->flags = FS_FILE | FS_PERSISTENT;
      meminfo_node->mask = 0444; // Read-only
      meminfo_node->read = procfs_meminfo_read;
      meminfo_node->length = 1024; // Dummy size, redefined on read

      ramfs_mount_node(procfs_root, meminfo_node);
    }

    // Add /proc/zoneinfo — PMM zone watermarks/free pages.  WebKit's
    // memory-pressure monitor opens it unconditionally; htop/btop read it.
    vfs_node_t *zoneinfo_node = kmalloc(sizeof(vfs_node_t));
    if (zoneinfo_node) {
      procfs_node_init(zoneinfo_node);
      strncpy(zoneinfo_node->name, "zoneinfo", 127);
      zoneinfo_node->flags = FS_FILE | FS_PERSISTENT;
      zoneinfo_node->mask = 0444;
      zoneinfo_node->read = procfs_zoneinfo_read;
      zoneinfo_node->length = 256; // Dummy size, redefined on read

      ramfs_mount_node(procfs_root, zoneinfo_node);
    }

    // Add /proc/numainfo — NUMA topology and per-node memory/CPU statistics
    vfs_node_t *numainfo_node = kmalloc(sizeof(vfs_node_t));
    if (numainfo_node) {
      procfs_node_init(numainfo_node);
      strncpy(numainfo_node->name, "numainfo", 127);
      numainfo_node->flags = FS_FILE | FS_PERSISTENT;
      numainfo_node->mask = 0444;
      numainfo_node->read = procfs_numainfo_read;
      numainfo_node->length = 512;

      ramfs_mount_node(procfs_root, numainfo_node);
    }

    // Add /proc/klog: the persistent boot-log ring (console/klog.c).  Serial
    // capture is not always available on real hardware; this makes the boot
    // log readable after the machine is up.
    vfs_node_t *klog_node = kmalloc(sizeof(vfs_node_t));
    if (klog_node) {
      procfs_node_init(klog_node);
      strncpy(klog_node->name, "klog", 127);
      klog_node->flags = FS_FILE | FS_PERSISTENT;
      klog_node->mask = 0444;
      klog_node->read = procfs_klog_read;
      klog_node->length = 65536;
      ramfs_mount_node(procfs_root, klog_node);
    }

    // Add /proc/bootlog: frozen snapshot of the boot log up to the first
    // userspace exec, so driver bring-up lines survive userland log spam.
    vfs_node_t *bootlog_node = kmalloc(sizeof(vfs_node_t));
    if (bootlog_node) {
      procfs_node_init(bootlog_node);
      strncpy(bootlog_node->name, "bootlog", 127);
      bootlog_node->flags = FS_FILE | FS_PERSISTENT;
      bootlog_node->mask = 0444;
      bootlog_node->read = procfs_bootlog_read;
      bootlog_node->length = 262144;
      ramfs_mount_node(procfs_root, bootlog_node);
    }

    // Add /proc/usb: USB host-controller, device and HID-driver state.
    // The driver's probe decisions survive boot here, so a machine without a
    // serial capture can still show why a mouse was not claimed.
    vfs_node_t *usb_node = kmalloc(sizeof(vfs_node_t));
    if (usb_node) {
      procfs_node_init(usb_node);
      strncpy(usb_node->name, "usb", 127);
      usb_node->flags = FS_FILE | FS_PERSISTENT;
      usb_node->mask = 0444;
      usb_node->read = procfs_usb_read;
      usb_node->length = 16384;
      ramfs_mount_node(procfs_root, usb_node);
    }

    // Add /proc/usb_mouse and /proc/usb_xhci: short focused views of the same
    // state, so one `cat` on a screen without scrollback is enough.
    vfs_node_t *usb_mouse_node = kmalloc(sizeof(vfs_node_t));
    if (usb_mouse_node) {
      procfs_node_init(usb_mouse_node);
      strncpy(usb_mouse_node->name, "usb_mouse", 127);
      usb_mouse_node->flags = FS_FILE | FS_PERSISTENT;
      usb_mouse_node->mask = 0444;
      usb_mouse_node->read = procfs_usb_mouse_read;
      usb_mouse_node->length = 16384;
      ramfs_mount_node(procfs_root, usb_mouse_node);
    }

    vfs_node_t *usb_xhci_node = kmalloc(sizeof(vfs_node_t));
    if (usb_xhci_node) {
      procfs_node_init(usb_xhci_node);
      strncpy(usb_xhci_node->name, "usb_xhci", 127);
      usb_xhci_node->flags = FS_FILE | FS_PERSISTENT;
      usb_xhci_node->mask = 0444;
      usb_xhci_node->read = procfs_usb_xhci_read;
      usb_xhci_node->length = 32768;
      ramfs_mount_node(procfs_root, usb_xhci_node);
    }

    // Add /proc/usb_debug: runtime switch for the verbose USB transfer trace.
    vfs_node_t *usb_debug_node = kmalloc(sizeof(vfs_node_t));
    if (usb_debug_node) {
      procfs_node_init(usb_debug_node);
      strncpy(usb_debug_node->name, "usb_debug", 127);
      usb_debug_node->flags = FS_FILE | FS_PERSISTENT;
      usb_debug_node->mask = 0644;
      usb_debug_node->read = procfs_usb_debug_read;
      usb_debug_node->write = procfs_usb_debug_write;
      usb_debug_node->length = 2;
      ramfs_mount_node(procfs_root, usb_debug_node);
    }

    // Add /proc/drmstats
    vfs_node_t *drmstats_node = kmalloc(sizeof(vfs_node_t));
    if (drmstats_node) {
      procfs_node_init(drmstats_node);
      strncpy(drmstats_node->name, "drmstats", 127);
      drmstats_node->flags = FS_FILE | FS_PERSISTENT;
      drmstats_node->mask = 0444;
      drmstats_node->read = procfs_drmstats_read;
      drmstats_node->length = 4096;
      ramfs_mount_node(procfs_root, drmstats_node);
    }

    // Add /proc/tlbstats: what TLB invalidation costs (mm/tlb_shootdown.c)
    vfs_node_t *tlbstats_node = kmalloc(sizeof(vfs_node_t));
    if (tlbstats_node) {
      procfs_node_init(tlbstats_node);
      strncpy(tlbstats_node->name, "tlbstats", 127);
      tlbstats_node->flags = FS_FILE | FS_PERSISTENT;
      tlbstats_node->mask = 0444;
      tlbstats_node->read = procfs_tlbstats_read;
      tlbstats_node->length = 512;
      ramfs_mount_node(procfs_root, tlbstats_node);
    }

    // Add /proc/cpuinfo
    vfs_node_t *cpuinfo_node = kmalloc(sizeof(vfs_node_t));
    if (cpuinfo_node) {
      procfs_node_init(cpuinfo_node);
      strncpy(cpuinfo_node->name, "cpuinfo", 127);
      cpuinfo_node->flags = FS_FILE | FS_PERSISTENT;
      cpuinfo_node->mask = 0444; // Read-only
      cpuinfo_node->read = procfs_cpuinfo_read;
      cpuinfo_node->length = 2048; // Dummy size

      ramfs_mount_node(procfs_root, cpuinfo_node);
    }

    // Add /proc/interrupts: per-CPU interrupt-vector delivery counts.
    vfs_node_t *interrupts_node = kmalloc(sizeof(vfs_node_t));
    if (interrupts_node) {
      procfs_node_init(interrupts_node);
      strncpy(interrupts_node->name, "interrupts", 127);
      interrupts_node->flags = FS_FILE | FS_PERSISTENT;
      interrupts_node->mask = 0444;
      interrupts_node->read = procfs_interrupts_read;
      interrupts_node->length = 1024; // Recomputed on read
      ramfs_mount_node(procfs_root, interrupts_node);
    }

    // Add /proc/partitions
    vfs_node_t *part_node = kmalloc(sizeof(vfs_node_t));
    if (part_node) {
      procfs_node_init(part_node);
      strncpy(part_node->name, "partitions", 127);
      part_node->flags = FS_FILE | FS_PERSISTENT;
      part_node->mask = 0444; // Read-only
      part_node->read = procfs_partitions_read;
      part_node->length = 1024; // Dummy size

      ramfs_mount_node(procfs_root, part_node);
    }

    vfs_node_t *storage_perf_node = kmalloc(sizeof(vfs_node_t));
    if (storage_perf_node) {
      procfs_node_init(storage_perf_node);
      strncpy(storage_perf_node->name, "storage_perf", 127);
      storage_perf_node->flags = FS_FILE | FS_PERSISTENT;
      storage_perf_node->mask = 0444;
      storage_perf_node->read = procfs_storage_perf_read;
      storage_perf_node->length = 32768;
      ramfs_mount_node(procfs_root, storage_perf_node);
    }

    // Add /proc/diskstats (see procfs_diskstats_read: real names, zero
    // counters until the block layer tracks I/O stats)
    vfs_node_t *diskstats_node = kmalloc(sizeof(vfs_node_t));
    if (diskstats_node) {
      procfs_node_init(diskstats_node);
      strncpy(diskstats_node->name, "diskstats", 127);
      diskstats_node->flags = FS_FILE | FS_PERSISTENT;
      diskstats_node->mask = 0444;
      diskstats_node->read = procfs_diskstats_read;
      diskstats_node->length = 1024;

      ramfs_mount_node(procfs_root, diskstats_node);
    }

    // Add /proc/mounts
    vfs_node_t *mounts_node = kmalloc(sizeof(vfs_node_t));
    if (mounts_node) {
      procfs_node_init(mounts_node);
      strncpy(mounts_node->name, "mounts", 127);
      mounts_node->flags = FS_FILE | FS_PERSISTENT;
      mounts_node->mask = 0444;
      mounts_node->read = procfs_mounts_read;
      mounts_node->length = 65536;
      ramfs_mount_node(procfs_root, mounts_node);
    }

    // Add /proc/uptime
    vfs_node_t *uptime_node = kmalloc(sizeof(vfs_node_t));
    if (uptime_node) {
      procfs_node_init(uptime_node);
      strncpy(uptime_node->name, "uptime", 127);
      uptime_node->flags = FS_FILE | FS_PERSISTENT;
      uptime_node->mask = 0444;
      uptime_node->read = procfs_uptime_read;
      ramfs_mount_node(procfs_root, uptime_node);
    }

    // Add /proc/stat
    vfs_node_t *static_node = kmalloc(sizeof(vfs_node_t));
    if (static_node) {
      procfs_node_init(static_node);
      strncpy(static_node->name, "stat", 127);
      static_node->flags = FS_FILE | FS_PERSISTENT;
      static_node->mask = 0444;
      static_node->read = procfs_stat_read;
      ramfs_mount_node(procfs_root, static_node);
    }

    // Add /proc/sched_debug
    vfs_node_t *sched_node = kmalloc(sizeof(vfs_node_t));
    if (sched_node) {
      procfs_node_init(sched_node);
      strncpy(sched_node->name, "sched_debug", 127);
      sched_node->flags = FS_FILE | FS_PERSISTENT;
      sched_node->mask = 0444;
      sched_node->read = procfs_sched_debug_read;
      ramfs_mount_node(procfs_root, sched_node);
    }

    // Add /proc/sysdiag
    vfs_node_t *sysdiag_node = kmalloc(sizeof(vfs_node_t));
    if (sysdiag_node) {
      procfs_node_init(sysdiag_node);
      strncpy(sysdiag_node->name, "sysdiag", 127);
      sysdiag_node->flags = FS_FILE | FS_PERSISTENT;
      sysdiag_node->mask = 0444;
      sysdiag_node->read = procfs_sysdiag_read;
      ramfs_mount_node(procfs_root, sysdiag_node);
    }

    // Add /proc/loadavg
    vfs_node_t *loadavg_node = kmalloc(sizeof(vfs_node_t));
    if (loadavg_node) {
      procfs_node_init(loadavg_node);
      strncpy(loadavg_node->name, "loadavg", 127);
      loadavg_node->flags = FS_FILE | FS_PERSISTENT;
      loadavg_node->mask = 0444;
      loadavg_node->read = procfs_loadavg_read;
      ramfs_mount_node(procfs_root, loadavg_node);
    }

    const char *zram_names[] = {"zram", "swaps"};
    for (unsigned i = 0; i < 2; i++) {
      vfs_node_t *zn = kmalloc(sizeof(*zn));
      if (zn) {
        procfs_node_init(zn); strcpy(zn->name, zram_names[i]);
        zn->flags = FS_FILE | FS_PERSISTENT; zn->mask = 0444;
        zn->read = procfs_zram_read; ramfs_mount_node(procfs_root, zn);
      }
    }

    // Add /proc/heapinfo
    vfs_node_t *heapinfo_node = kmalloc(sizeof(vfs_node_t));
    if (heapinfo_node) {
      procfs_node_init(heapinfo_node);
      strncpy(heapinfo_node->name, "heapinfo", 127);
      heapinfo_node->flags = FS_FILE | FS_PERSISTENT;
      heapinfo_node->mask = 0444;
      heapinfo_node->read = procfs_heapinfo_read;
      ramfs_mount_node(procfs_root, heapinfo_node);
    }

    // Add /proc/cmdline
    vfs_node_t *cmdline_node = kmalloc(sizeof(vfs_node_t));
    if (cmdline_node) {
      procfs_node_init(cmdline_node);
      strncpy(cmdline_node->name, "cmdline", 127);
      cmdline_node->flags = FS_FILE | FS_PERSISTENT;
      cmdline_node->mask = 0444;
      cmdline_node->read = procfs_cmdline_read;
      ramfs_mount_node(procfs_root, cmdline_node);
    }

    // Add /proc/version
    vfs_node_t *version_node = kmalloc(sizeof(vfs_node_t));
    if (version_node) {
      procfs_node_init(version_node);
      strncpy(version_node->name, "version", 127);
      version_node->flags = FS_FILE | FS_PERSISTENT;
      version_node->mask = 0444;
      version_node->read = procfs_version_read;
      version_node->length = 256;
      ramfs_mount_node(procfs_root, version_node);
    }

    // Add /proc/filesystems
    vfs_node_t *filesystems_node = kmalloc(sizeof(vfs_node_t));
    if (filesystems_node) {
      procfs_node_init(filesystems_node);
      strncpy(filesystems_node->name, "filesystems", 127);
      filesystems_node->flags = FS_FILE | FS_PERSISTENT;
      filesystems_node->mask = 0444;
      filesystems_node->read = procfs_filesystems_read;
      filesystems_node->length = 128;
      ramfs_mount_node(procfs_root, filesystems_node);
    }

    // /proc/cgroups: v1 hierarchy IDs matching pid/cgroup v1 lines.
    // elogind legacy lookup needs the elogind controller entry; missing file
    // pushes callers into unified-only assumptions that break hybrid setup.
    extern uint32_t procfs_cgroups_read(vfs_node_t *, uint32_t, uint32_t,
                                        uint8_t *);
    vfs_node_t *cgroups_node = kmalloc(sizeof(vfs_node_t));
    if (cgroups_node) {
      procfs_node_init(cgroups_node);
      strncpy(cgroups_node->name, "cgroups", 127);
      cgroups_node->flags = FS_FILE | FS_PERSISTENT;
      cgroups_node->mask = 0444;
      cgroups_node->read = procfs_cgroups_read;
      cgroups_node->length = 128;
      ramfs_mount_node(procfs_root, cgroups_node);
    }

    // Add /proc/net/ subdirectory
    procfs_net_init(procfs_root);

    // Add /proc/sys/ subdirectory
    procfs_sys_init(procfs_root);

    // Install dynamic PID hooks on top of the ramfs root.
    // These wrap the ramfs readdir/finddir to also expose live per-PID dirs.
    procfs_root->flags |= FS_DENTRY_NOCACHE;
    procfs_root->readdir = procfs_root_readdir;
    procfs_root->finddir = procfs_root_finddir;
  }
}

// =============================================================================
// /proc/net/
// =============================================================================

#include "net/core.h"
#include "net/ipv4.h"
#include "net/tcp.h"
#include "net/udp.h"

// Helper: format a big-endian IPv4 address as the 8-char hex string Linux uses
// in /proc/net/tcp and /proc/net/udp  (host byte-order, little-endian on x86).
static void fmt_ipv4_hex(char *out, uint32_t ip) {
    // Linux stores the address in native 32-bit little-endian order.
    // On x86 that means byte0=LSB, so we just print the uint32 as hex.
    const char *hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        out[i] = hex[ip & 0xF];
        ip >>= 4;
    }
    out[8] = '\0';
}

// Helper: format port as 4-char uppercase hex.
static void fmt_port_hex(char *out, uint16_t port) {
    const char *hex = "0123456789ABCDEF";
    out[0] = hex[(port >> 12) & 0xF];
    out[1] = hex[(port >>  8) & 0xF];
    out[2] = hex[(port >>  4) & 0xF];
    out[3] = hex[(port      ) & 0xF];
    out[4] = '\0';
}

// Map enum tcp_state → Linux /proc/net/tcp state byte.
static uint8_t tcp_state_to_linux(uint8_t s) {
    // Linux numbering (1-based):
    //  01 ESTABLISHED  02 SYN_SENT  03 SYN_RECV  04 FIN_WAIT1
    //  05 FIN_WAIT2    06 TIME_WAIT 07 CLOSE      08 CLOSE_WAIT
    //  09 LAST_ACK     0A LISTEN    0B CLOSING
    switch (s) {
    case 0:  return 0x07; // TCP_CLOSED     → CLOSE
    case 1:  return 0x0A; // TCP_LISTEN     → LISTEN
    case 2:  return 0x02; // TCP_SYN_SENT
    case 3:  return 0x03; // TCP_SYN_RECEIVED
    case 4:  return 0x01; // TCP_ESTABLISHED
    case 5:  return 0x04; // TCP_FIN_WAIT_1
    case 6:  return 0x05; // TCP_FIN_WAIT_2
    case 7:  return 0x08; // TCP_CLOSE_WAIT
    case 8:  return 0x09; // TCP_LAST_ACK
    case 9:  return 0x06; // TCP_TIME_WAIT
    default: return 0x07;
    }
}

// ---- /proc/net/dev ----------------------------------------------------------
// Format:
//   Inter-|   Receive                                                ...
//    face |bytes    packets errs drop ...
//      lo:      0       0    0    0 ...
//    eth0:   1234      10    0    0 ...

static uint32_t procfs_private_net_read(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer,
                                         const char *data) {
  uint32_t length = (uint32_t)strlen(data);
  node->length = length;
  if (offset >= length) return 0;
  if (size > length - offset) size = length - offset;
  memcpy(buffer, data + offset, size); return size;
}

static uint32_t procfs_net_dev_read(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
    if (sched_get_current() && sched_get_current()->net_ns)
      return procfs_private_net_read(node, offset, size, buffer, "Inter-|   Receive                                                |  Transmit\n face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n    lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    char *buf = kmalloc(2048);
    if (!buf) return 0;

    int pos = snprintf(buf, 2048,
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|"
        "bytes    packets errs drop fifo colls carrier compressed\n");

    // Loopback stub
    pos += snprintf(buf + pos, 2048 - pos,
        "    lo:       0       0    0    0    0     0          0         0"
        "        0       0    0    0    0     0       0          0\n");

    struct net_device *dev = net_device_default();
    if (dev) {
        pos += snprintf(buf + pos, 2048 - pos,
            "%6s: %llu %llu %llu %llu    0     0          0         0"
            " %llu %llu %llu %llu    0     0       0          0\n",
            dev->name,
            (unsigned long long)dev->stats.rx_bytes,
            (unsigned long long)dev->stats.rx_packets,
            (unsigned long long)(dev->stats.rx_errors + dev->stats.rx_overflows),
            (unsigned long long)dev->stats.rx_dropped,
            (unsigned long long)dev->stats.tx_bytes,
            (unsigned long long)dev->stats.tx_packets,
            (unsigned long long)dev->stats.tx_errors,
            (unsigned long long)dev->stats.tx_dropped);
    }

    node->length = (uint32_t)pos;
    if (offset >= (uint32_t)pos) { kfree(buf); return 0; }
    if (offset + size > (uint32_t)pos) size = (uint32_t)pos - offset;
    memcpy(buffer, buf + offset, size);
    kfree(buf);
    return size;
}

// ---- /proc/net/tcp ----------------------------------------------------------
// Each line: sl  local_address  rem_address  st  tx_queue:rx_queue  ...

static uint32_t procfs_net_tcp_read(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
    if (sched_get_current() && sched_get_current()->net_ns)
      return procfs_private_net_read(node, offset, size, buffer, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    struct tcp_entry_snapshot *snaps = kmalloc(sizeof(struct tcp_entry_snapshot) * TCP_MAX_CONNECTIONS);
    if (!snaps) return 0;
    int n = tcp_get_snapshot(snaps, TCP_MAX_CONNECTIONS);

    char *buf = kmalloc(256 + n * 176);
    if (!buf) {
        kfree(snaps);
        return 0;
    }

    int pos = snprintf(buf, 256,
        "  sl  local_address rem_address   st tx_queue rx_queue "
        "tr tm->when retrnsmt   uid  timeout inode\n");

    char lip[9], rip[9], lport[5], rport[5];
    for (int i = 0; i < n; i++) {
        fmt_ipv4_hex(lip,   snaps[i].local_ip);
        fmt_ipv4_hex(rip,   snaps[i].remote_ip);
        fmt_port_hex(lport, snaps[i].local_port);
        fmt_port_hex(rport, snaps[i].remote_port);
        uint8_t st = tcp_state_to_linux(snaps[i].state);
        pos += snprintf(buf + pos, 176,
            "%4d: %s:%s %s:%s %02X %08X:%08X 00 00000000 %llu 0 0 0\n",
            i, lip, lport, rip, rport, st,
            (unsigned)(snaps[i].bytes_sent & 0xffffffffu),
            (unsigned)snaps[i].rx_queued,
            (unsigned long long)snaps[i].retransmits);
    }
    kfree(snaps);

    node->length = (uint32_t)pos;
    if (offset >= (uint32_t)pos) { kfree(buf); return 0; }
    if (offset + size > (uint32_t)pos) size = (uint32_t)pos - offset;
    memcpy(buffer, buf + offset, size);
    kfree(buf);
    return size;
}

// ---- /proc/net/udp ----------------------------------------------------------

static uint32_t procfs_net_udp_read(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
    if (sched_get_current() && sched_get_current()->net_ns)
      return procfs_private_net_read(node, offset, size, buffer, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    struct udp_entry_snapshot *snaps =
        kmalloc(sizeof(struct udp_entry_snapshot) * UDP_MAX_SOCKETS);
    if (!snaps) return 0;
    int n = udp_get_snapshot(snaps, UDP_MAX_SOCKETS);

    char *buf = kmalloc(256 + n * 128);
    if (!buf) {
        kfree(snaps);
        return 0;
    }

    int pos = snprintf(buf, 256,
        "  sl  local_address rem_address   st tx_queue rx_queue "
        "tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");

    char lip[9], rip[9], lport[5], rport[5];
    for (int i = 0; i < n; i++) {
        fmt_ipv4_hex(lip,   snaps[i].local_ip);
        fmt_ipv4_hex(rip,   snaps[i].remote_ip);
        fmt_port_hex(lport, snaps[i].local_port);
        fmt_port_hex(rport, snaps[i].remote_port);
        // UDP state: 07 = CLOSE (unconnected), 01 = ESTABLISHED (connected)
        uint8_t st = snaps[i].connected ? 0x01 : 0x07;
        pos += snprintf(buf + pos, 128,
            "%4d: %s:%s %s:%s %02X 00000000:00000000 00 00000000 0 0 0 0\n",
            i, lip, lport, rip, rport, st);
    }
    kfree(snaps);

    node->length = (uint32_t)pos;
    if (offset >= (uint32_t)pos) { kfree(buf); return 0; }
    if (offset + size > (uint32_t)pos) size = (uint32_t)pos - offset;
    memcpy(buffer, buf + offset, size);
    kfree(buf);
    return size;
}

// ---- /proc/net/route --------------------------------------------------------
// Iface  Destination  Gateway  Flags  RefCnt  Use  Metric  Mask  MTU  Window  IRTT

static uint32_t procfs_net_route_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
    if (sched_get_current() && sched_get_current()->net_ns)
      return procfs_private_net_read(node, offset, size, buffer, "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n");
    char buf[512];

    int pos = snprintf(buf, sizeof(buf),
        "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\t"
        "MTU\tWindow\tIRTT\n");

    const struct ipv4_config *cfg = ipv4_get_config();
    struct net_device *dev = net_device_default();
    const char *ifname = dev ? dev->name : "eth0";

    if (cfg && cfg->address) {
        // Network route: destination = address & mask, gateway = 0.0.0.0
        uint32_t net_dst = cfg->address & cfg->netmask;
        pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
            "%s\t%08X\t%08X\t0001\t0\t0\t0\t%08X\t0\t0\t0\n",
            ifname, net_dst, 0U, cfg->netmask);

        // Default route: destination = 0.0.0.0, gateway = gateway
        if (cfg->gateway) {
            pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
                "%s\t%08X\t%08X\t0003\t0\t0\t100\t%08X\t0\t0\t0\n",
                ifname, 0U, cfg->gateway, 0U);
        }
    }

    node->length = (uint32_t)pos;
    if (offset >= (uint32_t)pos) return 0;
    if (offset + size > (uint32_t)pos) size = (uint32_t)pos - offset;
    memcpy(buffer, buf + offset, size);
    return size;
}

// ---- /proc/net/if_inet6 -----------------------------------------------------
// One line per IPv6-capable interface. We only have loopback for now.
// Format: addr devindex prefixlen scope flags ifname

static uint32_t procfs_net_if_inet6_read(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer) {
    if (sched_get_current() && sched_get_current()->net_ns)
      return procfs_private_net_read(node, offset, size, buffer, "");
    (void)node;
    // Loopback ::1 only
    const char *data =
        "00000000000000000000000000000001 01 80 10 80       lo\n";
    uint32_t len = (uint32_t)strlen(data);
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, data + offset, size);
    return size;
}

// ---- /proc/net/sockstat -----------------------------------------------------

static uint32_t procfs_net_sockstat_read(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer) {
    if (sched_get_current() && sched_get_current()->net_ns)
      return procfs_private_net_read(node, offset, size, buffer, "sockets: used 0\nTCP: inuse 0 orphan 0 tw 0 alloc 0 mem 0\nUDP: inuse 0 mem 0\nRAW: inuse 0\nFRAG: inuse 0 memory 0\n");
    struct tcp_entry_snapshot *tcp_snaps = kmalloc(sizeof(struct tcp_entry_snapshot) * TCP_MAX_CONNECTIONS);
    int tcp_n = 0;
    if (tcp_snaps) {
        tcp_n = tcp_get_snapshot(tcp_snaps, TCP_MAX_CONNECTIONS);
    }

    struct udp_entry_snapshot *udp_snaps =
        kmalloc(sizeof(struct udp_entry_snapshot) * UDP_MAX_SOCKETS);
    int udp_n = 0;
    if (udp_snaps)
        udp_n = udp_get_snapshot(udp_snaps, UDP_MAX_SOCKETS);

    // Count established TCP connections
    int tcp_estab = 0;
    for (int i = 0; i < tcp_n; i++)
        if (tcp_state_to_linux(tcp_snaps[i].state) == 0x01)
            tcp_estab++;

    if (tcp_snaps) kfree(tcp_snaps);

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "sockets: used %d\n"
        "TCP: inuse %d orphan 0 tw 0 alloc %d mem 0\n"
        "UDP: inuse %d mem 0\n"
        "RAW: inuse 0\n"
        "FRAG: inuse 0 memory 0\n",
        tcp_n + udp_n,
        tcp_estab, tcp_n,
        udp_n);

    node->length = (uint32_t)len;
    if (udp_snaps) kfree(udp_snaps);
    if (offset >= (uint32_t)len) return 0;
    if (offset + size > (uint32_t)len) size = (uint32_t)len - offset;
    memcpy(buffer, buf + offset, size);
    return size;
}

// ---- wire everything into a /proc/net/ directory ----------------------------

void procfs_net_init(vfs_node_t *procfs_root) {
    // Create the /proc/net directory node
    vfs_node_t *net_dir = kmalloc(sizeof(vfs_node_t));
    if (!net_dir) return;
    procfs_node_init(net_dir);
    strcpy(net_dir->name, "net");
    net_dir->flags = FS_DIRECTORY | FS_PERSISTENT;
    net_dir->mask  = 0555;
    ramfs_mount_on(net_dir);

    // Helper macro to reduce boilerplate
#define ADD_NET_FILE(fname, rfunc, flen)                      \
    do {                                                       \
        vfs_node_t *_n = kmalloc(sizeof(vfs_node_t));         \
        if (_n) {                                              \
            procfs_node_init(_n);                                 \
            strncpy(_n->name, fname, 127);                     \
            _n->flags  = FS_FILE | FS_PERSISTENT;              \
            _n->mask   = 0444;                                 \
            _n->read   = rfunc;                                \
            _n->length = flen;                                 \
            ramfs_mount_node(net_dir, _n);                     \
        }                                                      \
    } while (0)

    ADD_NET_FILE("dev",      procfs_net_dev_read,      2048);
    ADD_NET_FILE("tcp",      procfs_net_tcp_read,      4096);
    ADD_NET_FILE("udp",      procfs_net_udp_read,      4096);
    ADD_NET_FILE("route",    procfs_net_route_read,     512);
    ADD_NET_FILE("if_inet6", procfs_net_if_inet6_read,  128);
    ADD_NET_FILE("sockstat", procfs_net_sockstat_read,  256);

#undef ADD_NET_FILE

    ramfs_mount_node(procfs_root, net_dir);
}

// =============================================================================
// /proc/sys/
// =============================================================================

static uint32_t procfs_sys_overflowuid_read(vfs_node_t *node, uint32_t offset,
                                            uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/kernel/overflowuid\n");
    const char *val = "65534\n";
    uint32_t len = 6;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_overflowgid_read(vfs_node_t *node, uint32_t offset,
                                            uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/kernel/overflowgid\n");
    const char *val = "65534\n";
    uint32_t len = 6;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_pid_max_read(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/kernel/pid_max\n");
    const char *val = "32768\n";
    uint32_t len = 6;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_hostname_read(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/kernel/hostname\n");
    char val[67], domain[65];
    uts_namespace_read(sched_get_current(), val, domain);
    uint32_t len = (uint32_t)strlen(val);
    val[len++] = '\n';
    if (offset >= len) return 0;
    if (size > len - offset) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_osrelease_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/kernel/osrelease\n");
    const char *val = "2.5.0-beta\n";
    uint32_t len = 11;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_ostype_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/kernel/ostype\n");
    const char *val = "Linux\n";
    uint32_t len = 6;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_version_read(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/kernel/version\n");
    const char *val = "#1 SMP\n";
    uint32_t len = 7;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_cap_last_cap_read(vfs_node_t *node, uint32_t offset,
                                            uint32_t size, uint8_t *buffer) {
    (void)node;
    const char *val = "40\n";
    uint32_t len = 3;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_ngroups_max_read(vfs_node_t *node, uint32_t offset,
                                           uint32_t size, uint8_t *buffer) {
    (void)node;
    const char *val = "65536\n";
    uint32_t len = 6;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_max_user_ns_read(vfs_node_t *node, uint32_t offset,
                                            uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/user/max_user_namespaces\n");
    const char *val = "0\n";
    uint32_t len = 2;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_unprivileged_userns_clone_read(vfs_node_t *node, uint32_t offset,
                                                          uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/kernel/unprivileged_userns_clone\n");
    const char *val = "0\n";
    uint32_t len = 2;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_file_max_read(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/fs/file-max\n");
    const char *val = "1048576\n";
    uint32_t len = 8;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_inotify_limit_read(vfs_node_t *node, uint32_t offset,
                                               uint32_t size, uint8_t *buffer) {
    char value[24];
    uint32_t len = snprintf(value, sizeof(value), "%u\n", node->impl);
    if (offset >= len) return 0;
    if (size > len - offset) size = len - offset;
    memcpy(buffer, value + offset, size);
    return size;
}

static uint32_t procfs_sys_mount_max_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/fs/mount-max\n");
    const char *val = "100000\n";
    uint32_t len = 7;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_max_map_count_read(vfs_node_t *node, uint32_t offset,
                                              uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/vm/max_map_count\n");
    const char *val = "65530\n";
    uint32_t len = 6;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_drop_caches_read(vfs_node_t *node, uint32_t offset,
                                            uint32_t size, uint8_t *buffer) {
    (void)node;
    const char *val = "0\n";
    uint32_t len = 2;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_drop_caches_write(vfs_node_t *node, uint32_t offset,
                                             uint32_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    if (!buffer || size == 0) return 0;

    extern size_t mm_file_cache_reclaim(size_t target) __attribute__((weak));
    extern size_t vfs_dentry_reclaim_pages(size_t target) __attribute__((weak));
    extern size_t ext2_buffer_cache_shrink(size_t max_buffers) __attribute__((weak));
    extern void kmem_cache_shrink_all(void) __attribute__((weak));
    extern size_t thread_stack_cache_reclaim(size_t max_reclaim) __attribute__((weak));
    extern void pmm_pcp_drain_all(void) __attribute__((weak));

    char c = (char)buffer[0];
    if (c == '1' || c == '2' || c == '3') {
        if (c == '1' || c == '3') {
            if (mm_file_cache_reclaim)
                mm_file_cache_reclaim((size_t)-1);
            if (vfs_dentry_reclaim_pages)
                vfs_dentry_reclaim_pages((size_t)-1);
            if (ext2_buffer_cache_shrink) {
                for (int iter = 0; iter < 16; iter++) {
                    if (ext2_buffer_cache_shrink(256) == 0)
                        break;
                }
            }
        }
        if (c == '2' || c == '3') {
            if (vfs_dentry_reclaim_pages)
                vfs_dentry_reclaim_pages((size_t)-1);
            if (kmem_cache_shrink_all)
                kmem_cache_shrink_all();
            if (thread_stack_cache_reclaim)
                thread_stack_cache_reclaim(64);
        }
        if (pmm_pcp_drain_all)
            pmm_pcp_drain_all();
        if (c == '3') {
            ksm_scan_pass();
        }
        klog_puts("[PROCFS] drop_caches completed\n");
    }
    return size;
}

static uint32_t procfs_sys_ksm_run_read(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
    (void)node;
    char val[8];
    snprintf(val, sizeof(val), "%d\n", ksm_is_running() ? 1 : 0);
    uint32_t len = strlen(val);
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, val + offset, size);
    return size;
}

static uint32_t procfs_sys_ksm_run_write(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    if (!buffer || size == 0) return 0;
    char c = (char)buffer[0];
    if (c == '1') {
        ksm_set_running(true);
    } else if (c == '0') {
        ksm_set_running(false);
    }
    return size;
}

static uint32_t procfs_sys_ksm_pages_shared_read(vfs_node_t *node, uint32_t offset,
                                                 uint32_t size, uint8_t *buffer) {
    (void)node;
    struct ksm_stats st;
    ksm_get_stats(&st);
    uint64_t val = st.pages_shared;
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)val);
    uint32_t len = strlen(buf);
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t procfs_sys_ksm_pages_sharing_read(vfs_node_t *node, uint32_t offset,
                                                  uint32_t size, uint8_t *buffer) {
    (void)node;
    struct ksm_stats st;
    ksm_get_stats(&st);
    uint64_t val = st.pages_sharing + st.zero_pages_merged;
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)val);
    uint32_t len = strlen(buf);
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t procfs_sys_ksm_zero_pages_merged_read(vfs_node_t *node, uint32_t offset,
                                                      uint32_t size, uint8_t *buffer) {
    (void)node;
    struct ksm_stats st;
    ksm_get_stats(&st);
    uint64_t val = st.zero_pages_merged;
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)val);
    uint32_t len = strlen(buf);
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, buf + offset, size);
    return size;
}

/* Background passes run while free RAM exceeded the eager watermark. */
static uint32_t procfs_sys_ksm_passes_idle_read(vfs_node_t *node, uint32_t offset,
                                                uint32_t size, uint8_t *buffer) {
    (void)node;
    struct ksm_stats st;
    ksm_get_stats(&st);
    uint64_t val = st.passes_idle;
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)val);
    uint32_t len = strlen(buf);
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buffer, buf + offset, size);
    return size;
}

static vfs_node_t *procfs_create_dir_node(const char *name) {
    vfs_node_t *dir = kmalloc(sizeof(vfs_node_t));
    if (!dir) return NULL;
    procfs_node_init(dir);
    strncpy(dir->name, name, 127);
    dir->flags = FS_DIRECTORY | FS_PERSISTENT;
    dir->mask  = 0555;
    ramfs_mount_on(dir);
    return dir;
}

static void procfs_add_file_node(vfs_node_t *parent, const char *name, read_type_t rfunc, uint32_t len) {
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n) return;
    procfs_node_init(n);
    strncpy(n->name, name, 127);
    n->flags = FS_FILE | FS_PERSISTENT;
    n->mask = 0444;
    n->read = rfunc;
    n->length = len;
    ramfs_mount_node(parent, n);
}

void procfs_sys_init(vfs_node_t *procfs_root) {
    vfs_node_t *sys_dir = procfs_create_dir_node("sys");
    if (!sys_dir) return;

    // /proc/sys/kernel/
    vfs_node_t *kernel_dir = procfs_create_dir_node("kernel");
    if (kernel_dir) {
        procfs_add_file_node(kernel_dir, "unprivileged_userns_clone", procfs_sys_unprivileged_userns_clone_read, 16);
        procfs_add_file_node(kernel_dir, "overflowuid", procfs_sys_overflowuid_read, 16);
        procfs_add_file_node(kernel_dir, "overflowgid", procfs_sys_overflowgid_read, 16);
        procfs_add_file_node(kernel_dir, "pid_max",     procfs_sys_pid_max_read, 16);
        procfs_add_file_node(kernel_dir, "hostname",    procfs_sys_hostname_read, 32);
        procfs_add_file_node(kernel_dir, "osrelease",   procfs_sys_osrelease_read, 32);
        procfs_add_file_node(kernel_dir, "ostype",      procfs_sys_ostype_read, 16);
        procfs_add_file_node(kernel_dir, "version",     procfs_sys_version_read, 32);
        procfs_add_file_node(kernel_dir, "cap_last_cap", procfs_sys_cap_last_cap_read, 16);
        procfs_add_file_node(kernel_dir, "ngroups_max",  procfs_sys_ngroups_max_read, 16);
        ramfs_mount_node(sys_dir, kernel_dir);
    }

    // /proc/sys/user/
    vfs_node_t *user_dir = procfs_create_dir_node("user");
    if (user_dir) {
        procfs_add_file_node(user_dir, "max_user_namespaces", procfs_sys_max_user_ns_read, 16);
        ramfs_mount_node(sys_dir, user_dir);
    }

    // /proc/sys/fs/
    vfs_node_t *fs_dir = procfs_create_dir_node("fs");
    if (fs_dir) {
        procfs_add_file_node(fs_dir, "file-max",  procfs_sys_file_max_read, 16);
        procfs_add_file_node(fs_dir, "mount-max", procfs_sys_mount_max_read, 16);
        vfs_node_t *inotify_dir = procfs_create_dir_node("inotify");
        if (inotify_dir) {
            const char *names[] = {"max_user_watches", "max_user_instances"};
            const uint32_t limits[] = {MAX_INOTIFY_WATCHES, MAX_INOTIFY_INSTANCES};
            for (unsigned i = 0; i < 2; i++) {
                vfs_node_t *n = kmalloc(sizeof(*n));
                if (!n) continue;
                procfs_node_init(n);
                strcpy(n->name, names[i]);
                n->flags = FS_FILE | FS_PERSISTENT;
                n->mask = 0444;
                n->impl = limits[i];
                n->length = 16;
                n->read = procfs_sys_inotify_limit_read;
                ramfs_mount_node(inotify_dir, n);
            }
            ramfs_mount_node(fs_dir, inotify_dir);
        }
        ramfs_mount_node(sys_dir, fs_dir);
    }

    // /proc/sys/vm/
    vfs_node_t *vm_dir = procfs_create_dir_node("vm");
    if (vm_dir) {
        procfs_add_file_node(vm_dir, "max_map_count", procfs_sys_max_map_count_read, 16);

        vfs_node_t *dc_node = kmalloc(sizeof(*dc_node));
        if (dc_node) {
            procfs_node_init(dc_node);
            strcpy(dc_node->name, "drop_caches");
            dc_node->flags = FS_FILE | FS_PERSISTENT;
            dc_node->mask = 0644;
            dc_node->length = 4;
            dc_node->read = procfs_sys_drop_caches_read;
            dc_node->write = procfs_sys_drop_caches_write;
            ramfs_mount_node(vm_dir, dc_node);
        }

        vfs_node_t *ksm_run_node = kmalloc(sizeof(*ksm_run_node));
        if (ksm_run_node) {
            procfs_node_init(ksm_run_node);
            strcpy(ksm_run_node->name, "ksm_run");
            ksm_run_node->flags = FS_FILE | FS_PERSISTENT;
            ksm_run_node->mask = 0644;
            ksm_run_node->length = 4;
            ksm_run_node->read = procfs_sys_ksm_run_read;
            ksm_run_node->write = procfs_sys_ksm_run_write;
            ramfs_mount_node(vm_dir, ksm_run_node);
        }

        procfs_add_file_node(vm_dir, "ksm_pages_shared", procfs_sys_ksm_pages_shared_read, 16);
        procfs_add_file_node(vm_dir, "ksm_pages_sharing", procfs_sys_ksm_pages_sharing_read, 16);
        procfs_add_file_node(vm_dir, "ksm_zero_pages_merged", procfs_sys_ksm_zero_pages_merged_read, 16);
        procfs_add_file_node(vm_dir, "ksm_passes_idle", procfs_sys_ksm_passes_idle_read, 16);

        ramfs_mount_node(sys_dir, vm_dir);
    }

    ramfs_mount_node(procfs_root, sys_dir);
}
