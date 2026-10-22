#include <linuxkpi/gpu_perf.h>
#include <linuxkpi/gpu_diag.h>
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "lib/string.h"

static unsigned int enabled;
static uint64_t syncobj_ready;
void gpu_perf_syncobj_ready(void) {
    if (__atomic_load_n(&enabled, __ATOMIC_RELAXED))
        __atomic_add_fetch(&syncobj_ready, 1, __ATOMIC_RELAXED);
}
static struct gpu_ioctl_cost {
    uint64_t calls, total_ns, max_ns, errors;
} ioctl_costs[256];

static bool option(const char *cmdline, const char *wanted) {
    if (!cmdline)
        return false;
    size_t length = strlen(wanted);
    for (const char *cursor = cmdline; *cursor; cursor++)
        if ((cursor == cmdline || cursor[-1] == ' ') &&
            !strncmp(cursor, wanted, length) &&
            (!cursor[length] || cursor[length] == ' '))
            return true;
    return false;
}

void gpu_perf_init(const char *cmdline) {
    __atomic_store_n(&enabled, option(cmdline, "gpu_perf=1"), __ATOMIC_RELAXED);
    if (option(cmdline, "gpu_diag=0"))
        gpu_diag_control(1, (const unsigned char *)"0");
}

uint64_t gpu_perf_ioctl_start(unsigned int request) {
    if (!__atomic_load_n(&enabled, __ATOMIC_RELAXED) || ((request >> 8) & 255) != 'd')
        return 0;
    return lapic_timer_get_ns();
}

void gpu_perf_ioctl_finish(unsigned int request, uint64_t start, long result) {
    if (!start)
        return;
    uint64_t elapsed = lapic_timer_get_ns() - start;
    struct gpu_ioctl_cost *cost = &ioctl_costs[request & 255];
    __atomic_add_fetch(&cost->total_ns, elapsed, __ATOMIC_RELAXED);
    __atomic_add_fetch(&cost->errors, result < 0, __ATOMIC_RELAXED);
    uint64_t previous = __atomic_load_n(&cost->max_ns, __ATOMIC_RELAXED);
    while (elapsed > previous &&
           !__atomic_compare_exchange_n(&cost->max_ns, &previous, elapsed, false,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    __atomic_add_fetch(&cost->calls, 1, __ATOMIC_RELAXED);
}

void gpu_perf_dump(void) {
    struct virtio_gpu_stats stats;
    virtio_gpu_get_stats(&stats);
    klogf("[GPUPERF] snapshot_ns=%llu enabled=%u; ioctl wall time, combined DRM devices\n",
          (unsigned long long)lapic_timer_get_ns(), __atomic_load_n(&enabled, __ATOMIC_RELAXED));
    klogf("[GPUPERF] syncobj_ready_fast=%llu\n",
          (unsigned long long)__atomic_load_n(&syncobj_ready, __ATOMIC_RELAXED));
    klogf("[GPUPERF] frames=%llu upload_pixels=%llu scanout_pixels=%llu unchanged=%llu sync_ns=%llu copy_ns=%llu present_ns=%llu\n",
          (unsigned long long)stats.frames_presented, (unsigned long long)stats.damage_pixels,
          (unsigned long long)stats.scanout_pixels, (unsigned long long)stats.unchanged_frames,
          (unsigned long long)stats.producer_sync_ns, (unsigned long long)stats.snapshot_copy_ns,
          (unsigned long long)stats.transport_wait_ns);
    for (unsigned int index = 0; index < 256; index++) {
        struct gpu_ioctl_cost *cost = &ioctl_costs[index];
        uint64_t calls = __atomic_load_n(&cost->calls, __ATOMIC_RELAXED);
        if (!calls)
            continue;
        klogf("[GPUPERF] ioctl_nr=0x%02x calls=%llu total_ns=%llu max_ns=%llu errors=%llu\n",
              index, (unsigned long long)calls,
              (unsigned long long)__atomic_load_n(&cost->total_ns, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->max_ns, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->errors, __ATOMIC_RELAXED));
    }
    klog_puts("[GPUPERF] end\n");
}
