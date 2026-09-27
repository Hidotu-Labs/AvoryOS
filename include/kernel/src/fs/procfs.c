#include "fs/procfs.h"
#include "console/klog.h"
#include "apic/lapic_timer.h"
#include "drivers/storage/block.h"
#include "drivers/gpu/drm/drm.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "drivers/timer/rtc.h"
#include "cpu/tsc.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/tlb_shootdown.h"
#include "sched/sched.h"
#include "smp/cpu.h"
#include <stdint.h>

uint32_t procfs_meminfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  char buf[512];

  uint64_t total_kb = pmm_get_usable_memory() / 1024;
  uint64_t free_kb  = (uint64_t)pmm_get_free_pages() * 4096 / 1024;
  uint64_t cached_kb = (uint64_t)vfs_cache_page_count() * 4096 / 1024;
  uint64_t available_kb = free_kb + cached_kb;
  if (available_kb > total_kb)
    available_kb = total_kb;

  int len = snprintf(buf, sizeof(buf),
      "MemTotal:       %llu kB\n"
      "MemFree:        %llu kB\n"
      "MemAvailable:   %llu kB\n"
      "Buffers:        0 kB\n"
      "Cached:         %llu kB\n"
      "MemUsable:      %llu kB\n",
      (unsigned long long)total_kb,
      (unsigned long long)free_kb,
      (unsigned long long)available_kb,
      (unsigned long long)cached_kb,
      (unsigned long long)total_kb);

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
  char buf[2048];
  struct drm_stats stats;
  struct virtio_gpu_stats gpu_stats;
  drm_stats_snapshot(&stats);
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
      "tsc_khz: %llu\n"
      "virtio_commands_submitted: %llu\n"
      "virtio_commands_completed: %llu\n"
      "virtio_present_batches: %llu\n"
      "virtio_frames_presented: %llu\n"
      "virtio_damage_pixels: %llu\n"
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
      (unsigned long long)tsc_get_freq_khz(),
      (unsigned long long)gpu_stats.commands_submitted,
      (unsigned long long)gpu_stats.commands_completed,
      (unsigned long long)gpu_stats.present_batches,
      (unsigned long long)gpu_stats.frames_presented,
      (unsigned long long)gpu_stats.damage_pixels,
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
    CPUINFO_FLAG(features_ecx, 13, "cx16");
    CPUINFO_FLAG(features_ecx, 19, "sse4_1");
    CPUINFO_FLAG(features_ecx, 20, "sse4_2");
    CPUINFO_FLAG(features_ecx, 22, "movbe");
    CPUINFO_FLAG(features_ecx, 23, "popcnt");
    CPUINFO_FLAG(features_ecx, 25, "aes");
    CPUINFO_FLAG(features_ecx, 30, "rdrand");
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

uint32_t procfs_partitions_read(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  char *buf = kmalloc(4096);
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
    int written = snprintf(buf + pos, 4096 - (size_t)pos,
        "   1     %-8d%-10llu%s\n",
        i, (unsigned long long)blocks, dev->name);
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

uint32_t procfs_mounts_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  char *buf = kmalloc(2048);
  if (!buf)
    return 0;

  vfs_mount_info_t mounts[16];
  int count = vfs_get_mounts(mounts, 16);
  int pos = 0;

  for (int i = 0; i < count; i++) {
    pos += snprintf(buf + pos, 2048 - pos,
        "%s %s %s rw,relatime 0 0\n",
        mounts[i].dev_name, mounts[i].mountpoint, mounts[i].fs_type);
  }

  if (count == 0)
    pos += snprintf(buf, 2048, "/dev/sata01 / ext2 rw,relatime 0 0\n");

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

uint32_t procfs_stat_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
  char *buf = kmalloc(2048);
  if (!buf)
    return 0;

  uint32_t ncpus = cpu_get_count();
  if (ncpus == 0)
    ncpus = 1;

  /* Get aggregated user/idle time in ms, then convert to jiffies (USER_HZ=100,
   * so 1 jiffy = 10 ms).  Divide evenly across CPUs for the per-cpu lines. */
  uint64_t total_user_ms, total_idle_ms;
  sched_get_total_cpu_ms(&total_user_ms, &total_idle_ms);

  uint64_t user_jiffies = total_user_ms / 10;
  uint64_t idle_jiffies = total_idle_ms / 10;

  /* "cpu" aggregate line: fields are user nice system idle iowait irq softirq */
  int pos = snprintf(buf, 2048,
      "cpu  %llu 0 0 %llu 0 0 0 0 0 0\n",
      (unsigned long long)user_jiffies,
      (unsigned long long)idle_jiffies);

  /* Per-CPU lines — split the aggregate evenly across cores. */
  uint64_t per_cpu_user = user_jiffies / ncpus;
  uint64_t per_cpu_idle = idle_jiffies / ncpus;
  for (uint32_t i = 0; i < ncpus; i++) {
    pos += snprintf(buf + pos, 2048 - pos,
        "cpu%u %llu 0 0 %llu 0 0 0 0 0 0\n",
        i,
        (unsigned long long)per_cpu_user,
        (unsigned long long)per_cpu_idle);
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
  const char *fs =
      "nodev\tsysfs\n"
      "nodev\ttmpfs\n"
      "nodev\tdevtmpfs\n"
      "nodev\tproc\n"
      "nodev\tdevfs\n"
      "nodev\tramfs\n"
      "\text2\n"
      "\text3\n";

  uint32_t len = (uint32_t)strlen(fs);
  node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, fs + offset, size);
  return size;
}

uint32_t procfs_cmdline_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  (void)node;
  const char *cmd = "Xfbdev\n";
  uint32_t len = (uint32_t)strlen(cmd);

  if (offset >= len)
    return 0;
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, cmd + offset, size);
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
static uint32_t procfs_pid_stat_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  uint32_t pid = node->impl;
  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  char buf[512];
  uint64_t jiffies = t.runtime_total / 10;

  // Fields: pid (comm) state ppid pgrp session tty_nr tpgid flags
  //         minflt cminflt majflt cmajflt utime stime [37 stub zeros]
  int len = snprintf(buf, sizeof(buf),
      "%u (%s) %c %u %u 0 0 0 0 0 0 0 0 %llu 0 "
      "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
      pid,
      t.comm[0] ? t.comm : "unknown",
      thread_state_char(t.state),
      t.parent_tid,
      t.pgid,
      (unsigned long long)jiffies);

  node->length = (uint32_t)len;
  if (offset >= (uint32_t)len)
    return 0;
  if (offset + size > (uint32_t)len)
    size = (uint32_t)len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

// /proc/<pid>/status
static uint32_t procfs_pid_status_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  uint32_t pid = node->impl;
  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  char *buf = kmalloc(768);
  if (!buf)
    return 0;

  uint64_t virt_kb = 2048;
  uint64_t rss_kb  = 512;
  if (t.virt_bytes) {
    uint64_t virt_bytes = t.virt_bytes;
    if (virt_bytes < 2 * 1024 * 1024)
      virt_bytes = 2 * 1024 * 1024;
    virt_kb = virt_bytes / 1024;
    rss_kb  = virt_kb / 4;
    if (rss_kb < 512)
      rss_kb = 512;
  }

  int len = snprintf(buf, 768,
      "Name:\t%s\n"
      "State:\t%c\n"
      "Tgid:\t%u\n"
      "Pid:\t%u\n"
      "PPid:\t%u\n"
      "Uid:\t%u\t%u\t%u\t%u\n"
      "Gid:\t%u\t%u\t%u\t%u\n"
      "Threads:\t1\n"
      "VmSize:\t%llu kB\n"
      "VmRSS:\t%llu kB\n",
      t.comm[0] ? t.comm : "unknown",
      thread_state_char(t.state),
      pid, pid,
      t.parent_tid,
      t.uid, t.euid, t.suid, t.uid,
      t.gid, t.egid, t.sgid, t.gid,
      (unsigned long long)virt_kb,
      (unsigned long long)rss_kb);

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
static uint32_t procfs_pid_comm_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
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

// /proc/<pid>/statm — memory in pages
static uint32_t procfs_pid_statm_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  uint32_t pid = node->impl;
  struct sched_thread_snapshot t;
  if (!sched_get_thread_snapshot(pid, &t))
    return 0;

  char buf[64];

  uint64_t virt_bytes = t.virt_bytes;
  uint64_t res_bytes = t.resident_bytes;
  if (virt_bytes < 2 * 1024 * 1024) virt_bytes = 2 * 1024 * 1024;
  if (res_bytes  < 512 * 1024)      res_bytes  = 512 * 1024;

  uint64_t vp = virt_bytes / 4096;
  uint64_t rp = res_bytes  / 4096;

  int len = snprintf(buf, sizeof(buf),
      "%llu %llu 0 0 0 %llu 0\n",
      (unsigned long long)vp,
      (unsigned long long)rp,
      (unsigned long long)vp);

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
  uint32_t pid = node->impl;
  return (uint32_t)sched_read_thread_auxv(pid, offset, size, buffer);
}

// /proc/<pid>/fd/ support

static int procfs_pid_fd_link_readlink(vfs_node_t *node, char *buf,
                                       uint32_t size) {
  uint32_t pid_fd = node->impl;
  uint32_t pid = pid_fd >> 16;
  uint32_t fd = pid_fd & 0xFFFF;

  char path[256];
  if (!sched_get_fd_path_snapshot(pid, fd, path, sizeof(path)))
    return -2; // ENOENT

  uint32_t len = (uint32_t)strlen(path);
  if (len > size)
    len = size;
  memcpy(buf, path, len);
  return (int)len;
}

static vfs_node_t *procfs_pid_fd_finddir(vfs_node_t *node, char *name) {
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
  vfs_node_init(link);
  strcpy(link->name, name);
  link->flags = FS_SYMLINK;
  link->mask = 0777;
  link->impl = (pid << 16) | (fd & 0xFFFF);
  link->readlink = procfs_pid_fd_link_readlink;
  return link;
}

static struct dirent *procfs_pid_fd_readdir(vfs_node_t *node, uint32_t index) {
  uint32_t pid = node->impl;

  static struct dirent d;
  memset(&d, 0, sizeof(d));

  if (index == 0) {
    strcpy(d.name, ".");
    d.ino = node->inode;
    d.d_type = DT_DIR;
    return &d;
  }
  if (index == 1) {
    strcpy(d.name, "..");
    d.ino = node->inode;
    d.d_type = DT_DIR;
    return &d;
  }

  uint32_t fd;
  if (!sched_get_nth_open_fd(pid, index - 2, &fd))
    return NULL;
  snprintf(d.name, sizeof(d.name), "%u", fd);
  d.ino = (pid << 16) | fd;
  d.d_type = DT_LNK; // /proc/<pid>/fd/N entries are symlinks
  return &d;
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

static void procfs_make_pid_ramfs_ephemeral(vfs_node_t *dir) {
  /* ramfs_mount_on() defaults to persistent nodes. The proc cache owns these
   * instead, while open paths/FDs may keep them alive past cache removal. */
  dir->flags &= ~FS_PERSISTENT;
  dir->finddir = procfs_pid_ramfs_finddir;
  dir->close = procfs_pid_dir_close;
}

static vfs_node_t *make_pid_dir(uint32_t pid) {
  vfs_node_t *dir = kmalloc(sizeof(vfs_node_t));
  if (!dir)
    return NULL;
  vfs_node_init(dir);
  snprintf(dir->name, sizeof(dir->name), "%u", pid);
  dir->flags = FS_DIRECTORY; // not FS_PERSISTENT — ephemeral
  dir->mask = 0555;
  dir->inode = 0x10000 + pid;
  ramfs_mount_on(dir);
  procfs_make_pid_ramfs_ephemeral(dir);
  dir->flags |= FS_DENTRY_NOCACHE;

  // stat
  vfs_node_t *stat_node = kmalloc(sizeof(vfs_node_t));
  if (stat_node) {
    vfs_node_init(stat_node);
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
    vfs_node_init(status_node);
    strcpy(status_node->name, "status");
    status_node->flags = FS_FILE;
    status_node->mask = 0444;
    status_node->impl = pid;
    status_node->length = 256;
    status_node->read = procfs_pid_status_read;
    ramfs_mount_node(dir, status_node);
  }

  // cmdline
  vfs_node_t *cmdline_node = kmalloc(sizeof(vfs_node_t));
  if (cmdline_node) {
    vfs_node_init(cmdline_node);
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
    vfs_node_init(comm_node);
    strcpy(comm_node->name, "comm");
    comm_node->flags = FS_FILE;
    comm_node->mask = 0444;
    comm_node->impl = pid;
    comm_node->length = 16;
    comm_node->read = procfs_pid_comm_read;
    ramfs_mount_node(dir, comm_node);
  }

  // statm — memory usage in pages (VIRT/RES for htop)
  vfs_node_t *statm_node = kmalloc(sizeof(vfs_node_t));
  if (statm_node) {
    vfs_node_init(statm_node);
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
    vfs_node_init(io_node);
    strcpy(io_node->name, "io");
    io_node->flags = FS_FILE;
    io_node->mask = 0444;
    io_node->impl = pid;
    io_node->length = 64;
    io_node->read = procfs_pid_io_read;
    ramfs_mount_node(dir, io_node);
  }

  // auxv — ELF auxiliary vector
  vfs_node_t *auxv_node = kmalloc(sizeof(vfs_node_t));
  if (auxv_node) {
    vfs_node_init(auxv_node);
    strcpy(auxv_node->name, "auxv");
    auxv_node->flags = FS_FILE;
    auxv_node->mask = 0444;
    auxv_node->impl = pid;
    auxv_node->length = 512;
    auxv_node->read = procfs_pid_auxv_read;
    ramfs_mount_node(dir, auxv_node);
  }

  // fd directory
  vfs_node_t *fd_dir = kmalloc(sizeof(vfs_node_t));
  if (fd_dir) {
    vfs_node_init(fd_dir);
    strcpy(fd_dir->name, "fd");
    fd_dir->flags = FS_DIRECTORY | FS_DENTRY_NOCACHE;
    fd_dir->mask = 0555;
    fd_dir->impl = pid;
    fd_dir->readdir = procfs_pid_fd_readdir;
    fd_dir->finddir = procfs_pid_fd_finddir;
    ramfs_mount_node(dir, fd_dir);
  }

  // task/<pid>/ directory — htop opens task/<pid>/stat to read per-thread stat.
  // On Linux this mirrors /proc/<pid>/stat for the main thread.
  vfs_node_t *task_dir = kmalloc(sizeof(vfs_node_t));
  if (task_dir) {
    vfs_node_init(task_dir);
    strcpy(task_dir->name, "task");
    task_dir->flags = FS_DIRECTORY;
    task_dir->mask = 0555;
    ramfs_mount_on(task_dir);
    procfs_make_pid_ramfs_ephemeral(task_dir);
    task_dir->flags |= FS_DENTRY_NOCACHE;

    // task/<pid>/ sub-directory
    vfs_node_t *tid_dir = kmalloc(sizeof(vfs_node_t));
    if (tid_dir) {
      vfs_node_init(tid_dir);
      snprintf(tid_dir->name, sizeof(tid_dir->name), "%u", pid);
      tid_dir->flags = FS_DIRECTORY;
      tid_dir->mask = 0555;
      ramfs_mount_on(tid_dir);
      procfs_make_pid_ramfs_ephemeral(tid_dir);
      tid_dir->flags |= FS_DENTRY_NOCACHE;

      // task/<pid>/stat  — same content as /proc/<pid>/stat
      vfs_node_t *tstat_node = kmalloc(sizeof(vfs_node_t));
      if (tstat_node) {
        vfs_node_init(tstat_node);
        strcpy(tstat_node->name, "stat");
        tstat_node->flags = FS_FILE;
        tstat_node->mask = 0444;
        tstat_node->impl = pid;
        tstat_node->length = 128;
        tstat_node->read = procfs_pid_stat_read;
        ramfs_mount_node(tid_dir, tstat_node);
      }

      ramfs_mount_node(task_dir, tid_dir);
    }

    ramfs_mount_node(dir, task_dir);
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
  snprintf(pid_str, sizeof(pid_str), "%u", t->tid);
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

static struct dirent procfs_dent; // single static buffer (safe: no preemption
                                  // between readdir calls in getdents64 loop)

static struct dirent *procfs_root_readdir(vfs_node_t *node, uint32_t index) {
  memset(&procfs_dent, 0, sizeof(procfs_dent));

  if (index == 0) {
    strcpy(procfs_dent.name, ".");
    procfs_dent.ino = node->inode;
    procfs_dent.d_type = DT_DIR;
    return &procfs_dent;
  }
  if (index == 1) {
    strcpy(procfs_dent.name, "..");
    procfs_dent.ino = node->inode;
    procfs_dent.d_type = DT_DIR;
    return &procfs_dent;
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
    strcpy(procfs_dent.name, curr->node->name);
    procfs_dent.ino = curr->node->inode;
    procfs_dent.d_type = vfs_dtype(curr->node->flags);
    return &procfs_dent;
  }

  // Count total static children
  uint32_t num_static = 0;
  for (child_node_t *c = rdir->children; c; c = c->next)
    num_static++;

  // PID entries
  uint32_t pid;
  if (!sched_get_nth_thread_tid(static_idx - num_static, &pid))
    return NULL;
  snprintf(procfs_dent.name, sizeof(procfs_dent.name), "%u", pid);
  procfs_dent.ino = 0x10000 + pid;
  procfs_dent.d_type = DT_DIR;
  return &procfs_dent;
}

// Custom finddir for /proc

static vfs_node_t *procfs_root_finddir(vfs_node_t *node, char *name) {
  if (strcmp(name, "self") == 0) {
    vfs_node_t *self_link = kmalloc(sizeof(vfs_node_t));
    if (!self_link)
      return NULL;
    vfs_node_init(self_link);
    strcpy(self_link->name, "self");
    self_link->flags = FS_SYMLINK;
    self_link->mask = 0777;
    self_link->readlink = procfs_self_readlink;
    return self_link;
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
  uint32_t pid = str_to_pid(name);
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
    vfs_node_init(procfs_root);
    strcpy(procfs_root->name, "proc");
    procfs_root->flags = FS_DIRECTORY;
    procfs_root->mask = 0555;
    ramfs_mount_on(procfs_root);

    vfs_open(procfs_root); // Permanent reference for the mount entry

    // Apply the mount: anyone looking up 'proc' will now get our virtual root
    vfs_mount_ex(proc_dir, procfs_root, "proc", "procfs");
    vfs_close(proc_dir);

    // Add /proc/meminfo
    vfs_node_t *meminfo_node = kmalloc(sizeof(vfs_node_t));
    if (meminfo_node) {
      vfs_node_init(meminfo_node);
      strncpy(meminfo_node->name, "meminfo", 127);
      meminfo_node->flags = FS_FILE | FS_PERSISTENT;
      meminfo_node->mask = 0444; // Read-only
      meminfo_node->read = procfs_meminfo_read;
      meminfo_node->length = 512; // Dummy size, redefined on read

      ramfs_mount_node(procfs_root, meminfo_node);
    }

    // Add /proc/drmstats
    vfs_node_t *drmstats_node = kmalloc(sizeof(vfs_node_t));
    if (drmstats_node) {
      vfs_node_init(drmstats_node);
      strncpy(drmstats_node->name, "drmstats", 127);
      drmstats_node->flags = FS_FILE | FS_PERSISTENT;
      drmstats_node->mask = 0444;
      drmstats_node->read = procfs_drmstats_read;
      drmstats_node->length = 2048;
      ramfs_mount_node(procfs_root, drmstats_node);
    }

    // Add /proc/tlbstats: what TLB invalidation costs (mm/tlb_shootdown.c)
    vfs_node_t *tlbstats_node = kmalloc(sizeof(vfs_node_t));
    if (tlbstats_node) {
      vfs_node_init(tlbstats_node);
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
      vfs_node_init(cpuinfo_node);
      strncpy(cpuinfo_node->name, "cpuinfo", 127);
      cpuinfo_node->flags = FS_FILE | FS_PERSISTENT;
      cpuinfo_node->mask = 0444; // Read-only
      cpuinfo_node->read = procfs_cpuinfo_read;
      cpuinfo_node->length = 2048; // Dummy size

      ramfs_mount_node(procfs_root, cpuinfo_node);
    }

    // Add /proc/partitions
    vfs_node_t *part_node = kmalloc(sizeof(vfs_node_t));
    if (part_node) {
      vfs_node_init(part_node);
      strncpy(part_node->name, "partitions", 127);
      part_node->flags = FS_FILE | FS_PERSISTENT;
      part_node->mask = 0444; // Read-only
      part_node->read = procfs_partitions_read;
      part_node->length = 1024; // Dummy size

      ramfs_mount_node(procfs_root, part_node);
    }

    // Add /proc/mounts
    vfs_node_t *mounts_node = kmalloc(sizeof(vfs_node_t));
    if (mounts_node) {
      vfs_node_init(mounts_node);
      strncpy(mounts_node->name, "mounts", 127);
      mounts_node->flags = FS_FILE | FS_PERSISTENT;
      mounts_node->mask = 0444;
      mounts_node->read = procfs_mounts_read;
      ramfs_mount_node(procfs_root, mounts_node);
    }

    // Add /proc/uptime
    vfs_node_t *uptime_node = kmalloc(sizeof(vfs_node_t));
    if (uptime_node) {
      vfs_node_init(uptime_node);
      strncpy(uptime_node->name, "uptime", 127);
      uptime_node->flags = FS_FILE | FS_PERSISTENT;
      uptime_node->mask = 0444;
      uptime_node->read = procfs_uptime_read;
      ramfs_mount_node(procfs_root, uptime_node);
    }

    // Add /proc/stat
    vfs_node_t *static_node = kmalloc(sizeof(vfs_node_t));
    if (static_node) {
      vfs_node_init(static_node);
      strncpy(static_node->name, "stat", 127);
      static_node->flags = FS_FILE | FS_PERSISTENT;
      static_node->mask = 0444;
      static_node->read = procfs_stat_read;
      ramfs_mount_node(procfs_root, static_node);
    }

    // Add /proc/sched_debug
    vfs_node_t *sched_node = kmalloc(sizeof(vfs_node_t));
    if (sched_node) {
      vfs_node_init(sched_node);
      strncpy(sched_node->name, "sched_debug", 127);
      sched_node->flags = FS_FILE | FS_PERSISTENT;
      sched_node->mask = 0444;
      sched_node->read = procfs_sched_debug_read;
      ramfs_mount_node(procfs_root, sched_node);
    }

    // Add /proc/loadavg
    vfs_node_t *loadavg_node = kmalloc(sizeof(vfs_node_t));
    if (loadavg_node) {
      vfs_node_init(loadavg_node);
      strncpy(loadavg_node->name, "loadavg", 127);
      loadavg_node->flags = FS_FILE | FS_PERSISTENT;
      loadavg_node->mask = 0444;
      loadavg_node->read = procfs_loadavg_read;
      ramfs_mount_node(procfs_root, loadavg_node);
    }

    // Add /proc/heapinfo
    vfs_node_t *heapinfo_node = kmalloc(sizeof(vfs_node_t));
    if (heapinfo_node) {
      vfs_node_init(heapinfo_node);
      strncpy(heapinfo_node->name, "heapinfo", 127);
      heapinfo_node->flags = FS_FILE | FS_PERSISTENT;
      heapinfo_node->mask = 0444;
      heapinfo_node->read = procfs_heapinfo_read;
      ramfs_mount_node(procfs_root, heapinfo_node);
    }

    // Add /proc/cmdline
    vfs_node_t *cmdline_node = kmalloc(sizeof(vfs_node_t));
    if (cmdline_node) {
      vfs_node_init(cmdline_node);
      strncpy(cmdline_node->name, "cmdline", 127);
      cmdline_node->flags = FS_FILE | FS_PERSISTENT;
      cmdline_node->mask = 0444;
      cmdline_node->read = procfs_cmdline_read;
      ramfs_mount_node(procfs_root, cmdline_node);
    }

    // Add /proc/version
    vfs_node_t *version_node = kmalloc(sizeof(vfs_node_t));
    if (version_node) {
      vfs_node_init(version_node);
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
      vfs_node_init(filesystems_node);
      strncpy(filesystems_node->name, "filesystems", 127);
      filesystems_node->flags = FS_FILE | FS_PERSISTENT;
      filesystems_node->mask = 0444;
      filesystems_node->read = procfs_filesystems_read;
      filesystems_node->length = 128;
      ramfs_mount_node(procfs_root, filesystems_node);
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

static uint32_t procfs_net_dev_read(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
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
    vfs_node_init(net_dir);
    strcpy(net_dir->name, "net");
    net_dir->flags = FS_DIRECTORY | FS_PERSISTENT;
    net_dir->mask  = 0555;
    ramfs_mount_on(net_dir);

    // Helper macro to reduce boilerplate
#define ADD_NET_FILE(fname, rfunc, flen)                      \
    do {                                                       \
        vfs_node_t *_n = kmalloc(sizeof(vfs_node_t));         \
        if (_n) {                                              \
            vfs_node_init(_n);                                 \
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
    const char *val = "AvoryOS\n";
    uint32_t len = 8;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
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

static uint32_t procfs_sys_max_user_ns_read(vfs_node_t *node, uint32_t offset,
                                            uint32_t size, uint8_t *buffer) {
    (void)node;
    klog_puts("[PROCFS] read /proc/sys/user/max_user_namespaces\n");
    const char *val = "65536\n";
    uint32_t len = 6;
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

static vfs_node_t *procfs_create_dir_node(const char *name) {
    vfs_node_t *dir = kmalloc(sizeof(vfs_node_t));
    if (!dir) return NULL;
    vfs_node_init(dir);
    strncpy(dir->name, name, 127);
    dir->flags = FS_DIRECTORY | FS_PERSISTENT;
    dir->mask  = 0555;
    ramfs_mount_on(dir);
    return dir;
}

static void procfs_add_file_node(vfs_node_t *parent, const char *name, read_type_t rfunc, uint32_t len) {
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n) return;
    vfs_node_init(n);
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
        procfs_add_file_node(kernel_dir, "overflowuid", procfs_sys_overflowuid_read, 16);
        procfs_add_file_node(kernel_dir, "overflowgid", procfs_sys_overflowgid_read, 16);
        procfs_add_file_node(kernel_dir, "pid_max",     procfs_sys_pid_max_read, 16);
        procfs_add_file_node(kernel_dir, "hostname",    procfs_sys_hostname_read, 32);
        procfs_add_file_node(kernel_dir, "osrelease",   procfs_sys_osrelease_read, 32);
        procfs_add_file_node(kernel_dir, "ostype",      procfs_sys_ostype_read, 16);
        procfs_add_file_node(kernel_dir, "version",     procfs_sys_version_read, 32);
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
        ramfs_mount_node(sys_dir, fs_dir);
    }

    // /proc/sys/vm/
    vfs_node_t *vm_dir = procfs_create_dir_node("vm");
    if (vm_dir) {
        procfs_add_file_node(vm_dir, "max_map_count", procfs_sys_max_map_count_read, 16);
        ramfs_mount_node(sys_dir, vm_dir);
    }

    ramfs_mount_node(procfs_root, sys_dir);
}
