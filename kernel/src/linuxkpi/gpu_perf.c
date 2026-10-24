#include <linuxkpi/gpu_perf.h>
#include <linuxkpi/gpu_diag.h>
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "lib/string.h"
#include "sched/sched.h"

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

/* Attribute the upstream timeline path separately from the ready fast path.
 * Poll probes and actual waits must not share an average: Mesa issues both. */
static struct syncobj_wait_cost {
    uint64_t calls, total_ns, errors, latency[6];
} syncobj_wait_costs[2];

uint64_t gpu_perf_syncobj_wait_start(void) {
    if (!__atomic_load_n(&enabled, __ATOMIC_RELAXED))
        return 0;
    uint64_t now = lapic_timer_get_ns();
    struct thread *t = sched_get_current();
    if (t)
        __atomic_store_n(&t->perf_syncobj_active_ns, now, __ATOMIC_RELEASE);
    return now;
}

void gpu_perf_syncobj_wait_finish(uint64_t start, int polling, long result) {
    if (!start)
        return;
    uint64_t elapsed = lapic_timer_get_ns() - start;
    struct thread *t = sched_get_current();
    if (t)
        __atomic_store_n(&t->perf_syncobj_active_ns, 0, __ATOMIC_RELEASE);
    struct syncobj_wait_cost *cost = &syncobj_wait_costs[!!polling];
    unsigned int bucket = elapsed < 5000 ? 0 : elapsed < 50000 ? 1 :
        elapsed < 250000 ? 2 : elapsed < 1000000 ? 3 :
        elapsed < 10000000 ? 4 : 5;
    __atomic_add_fetch(&cost->total_ns, elapsed, __ATOMIC_RELAXED);
    __atomic_add_fetch(&cost->errors, result < 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&cost->latency[bucket], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&cost->calls, 1, __ATOMIC_RELAXED);
}

static struct syncobj_sleep_cost {
    uint64_t parks, notified, before_notify_ns, after_notify_ns, unnotified_ns;
} syncobj_sleep_costs[2];

void gpu_perf_syncobj_park(void *thread) {
    struct thread *t = thread;
    if (!__atomic_load_n(&t->perf_syncobj_active_ns, __ATOMIC_ACQUIRE))
        return;
    uint64_t now = lapic_timer_get_ns();
    __atomic_store_n(&t->perf_syncobj_park_ns, now, __ATOMIC_RELEASE);
    __atomic_store_n(&t->perf_syncobj_wake_ns, now, __ATOMIC_RELEASE);
}

void gpu_perf_syncobj_wake(void *thread) {
    struct thread *t = thread;
    uint64_t park = __atomic_load_n(&t->perf_syncobj_park_ns, __ATOMIC_ACQUIRE);
    if (!park)
        return;
    /* Only the first notification for this park wins. Using the park token
     * as the expected value rejects a late writer from a previous sleep. */
    __atomic_compare_exchange_n(&t->perf_syncobj_wake_ns, &park,
                               lapic_timer_get_ns(), false,
                               __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

void gpu_perf_syncobj_resume(void *thread) {
    struct thread *t = thread;
    uint64_t park = __atomic_exchange_n(&t->perf_syncobj_park_ns, 0, __ATOMIC_ACQ_REL);
    if (!park)
        return;
    uint64_t now = lapic_timer_get_ns();
    if (now < park)
        return;
    uint64_t wake = __atomic_exchange_n(&t->perf_syncobj_wake_ns, 0, __ATOMIC_ACQ_REL);
    struct syncobj_sleep_cost *cost = &syncobj_sleep_costs[!strcmp(t->comm, "glxgears")];
    if (wake > park && wake <= now) {
        __atomic_add_fetch(&cost->before_notify_ns, wake - park, __ATOMIC_RELAXED);
        __atomic_add_fetch(&cost->after_notify_ns, now - wake, __ATOMIC_RELAXED);
        __atomic_add_fetch(&cost->notified, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&cost->unnotified_ns, now - park, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&cost->parks, 1, __ATOMIC_RELAXED);
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
    for (unsigned int i = 0; i < 2; i++) {
        struct syncobj_wait_cost *cost = &syncobj_wait_costs[i];
        klogf("[GPUPERF] syncobj_upstream=%s calls=%llu total_ns=%llu errors=%llu "
              "lt5us=%llu lt50us=%llu lt250us=%llu lt1ms=%llu lt10ms=%llu ge10ms=%llu\n",
              i ? "poll" : "wait",
              (unsigned long long)__atomic_load_n(&cost->calls, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->total_ns, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->errors, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->latency[0], __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->latency[1], __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->latency[2], __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->latency[3], __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->latency[4], __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->latency[5], __ATOMIC_RELAXED));
    }
    for (unsigned int i = 0; i < 2; i++) {
        struct syncobj_sleep_cost *cost = &syncobj_sleep_costs[i];
        klogf("[GPUPERF] syncobj_sleep caller=%s parks=%llu notified=%llu "
              "before_notify_ns=%llu after_notify_ns=%llu unnotified_ns=%llu\n",
              i ? "glxgears" : "other",
              (unsigned long long)__atomic_load_n(&cost->parks, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->notified, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->before_notify_ns, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->after_notify_ns, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&cost->unnotified_ns, __ATOMIC_RELAXED));
    }
    klog_puts("[GPUPERF] end\n");
}
