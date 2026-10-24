#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu_scanout.h"
#include "../../../lock/lockdiag.h"
#include "../../../fb/vblank.h"
#include "../../../fb/framebuffer.h"
#include "apic/lapic_timer.h"
#include "cpu/irq.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/gpu/drm/drm.h"
#include "drivers/input/mouse.h"
#include "mm/heap.h"
#include "lib/string.h"
#include <linuxkpi/gpu_perf.h>
#include "mm/pmm.h"
#include "socket/af_netlink.h"
#include "fs/sysfs.h"
#include "fs/vfs.h"
#include <stdint.h>
#define GPU_CONTROLQ   0
#define GPU_CURSORQ    1
#define GPU_TIMEOUT_MS 1000
#define REQ_OFF        0
#define REQ_CAP        1024
#define RESP_OFF       2048
#define RESP_CAP       (PAGE_SIZE - RESP_OFF)
static struct virtio_gpu_device gpu;
static uint32_t fbcon_scanout_id = 0;
static spinlock_t gpu_poll_lock;
static spinlock_t gpu_present_lock;
/* Task-context presentation gate. Native and imported producer GEMs share
 * each head's immutable staging pair, SET_SCANOUT and front-resource state.
 * Acquire before taking a framebuffer snapshot and retain through publication.
 * Yield with the caller's IRQ state unchanged; never hold an IRQ spinlock
 * across reservation/fence or vblank waits. IRQ/completion paths do not take
 * this gate, so they can make progress while a presenter owns it. */
static uint32_t gpu_scanout_busy;
/* Preserve full-snapshot coalescing for uncapped single-output PRIME
 * producers. Multi-output commits are never discarded by another head. */
static uint64_t gpu_prime_enter_seq;
static uint32_t gpu_prime_waiters;
static struct vgpu_scanout_head scanout_heads[VIRTIO_GPU_MAX_SCANOUTS];
static const struct vgpu_scanout_ops scanout_ops;
static void gpu_scanout_acquire(void) {
    while (__atomic_exchange_n(&gpu_scanout_busy, 1, __ATOMIC_ACQUIRE))
        sched_yield();
}
static void gpu_scanout_release(void) {
    __atomic_store_n(&gpu_scanout_busy, 0, __ATOMIC_RELEASE);
}
static wait_queue_t gpu_worker_wait;
static bool gpu_worker_started;
static uint32_t gpu_async_flip_state;
static bool gpu_flip_worker_started;
static wait_queue_t gpu_flip_wait;
static struct {
    struct drm_framebuffer framebuffer;
    struct drm_file *file;
    struct vfs_node *node;
    uint32_t crtc, head;
    uint64_t cookie;
    bool event;
} gpu_async_flip;
static void virtio_gpu_flip_worker(void);
static bool gpu_irq_installed;
enum gpu_irq_mode { GPU_IRQ_POLL, GPU_IRQ_INTX, GPU_IRQ_MSIX };
static enum gpu_irq_mode gpu_irq_mode;
static uint8_t gpu_msix_vectors[3] = {0xFF, 0xFF, 0xFF};
static volatile uint32_t gpu_pending_work;
#define GPU_WORK_CONTROL (1U << 0)
#define GPU_WORK_CURSOR  (1U << 1)
#define GPU_WORK_CONFIG  (1U << 2)
#define GPU_WORK_PERF    (1U << 3)
void virtio_gpu_request_perf_dump(void) {
    if (!gpu_worker_started)
        return;
    __atomic_fetch_or(&gpu_pending_work, GPU_WORK_PERF, __ATOMIC_RELEASE);
    wait_queue_wake_all(&gpu_worker_wait);
}
static bool gpu_phase8_started, gpu_phase8_reported;
static uint64_t gpu_phase8_started_ms;
/* Protected by gpu_poll_lock. A timeout does not return DMA ownership: the
 * host may still read the request or write the response. Keep the shared
 * command pages unavailable until every submitted descriptor is returned. */
static uint64_t gpu_sync_command_pending;
#define GPU_BATCH_COMMANDS 3U
#define GPU_BATCH_SLOT_SIZE 256U
#define GPU_BATCH_RESP_OFF 128U
static uint64_t gpu_batch_fences[GPU_BATCH_COMMANDS];
static uint32_t gpu_batch_pending;
static bool gpu_batch_waiter, gpu_batch_failed;
static bool gpu_present_batch_enabled;
static bool handle_batch_completion(void *cookie, uint32_t used) {
    for (uint32_t index = 0; index < GPU_BATCH_COMMANDS; index++) {
        uint32_t bit = 1U << index;
        if (!(gpu_batch_pending & bit) || cookie != &gpu_batch_fences[index])
            continue;
        struct virtio_gpu_ctrl_hdr *response = (void *)((uint8_t *)gpu.present_dma->virt +
            index * GPU_BATCH_SLOT_SIZE + GPU_BATCH_RESP_OFF);
        if (used != sizeof(*response) || !(response->flags & VIRTIO_GPU_FLAG_FENCE) ||
            response->fence_id != gpu_batch_fences[index] ||
            response->type != VIRTIO_GPU_RESP_OK_NODATA) {
            gpu_batch_failed = true;
            if (used == sizeof(*response) && response->type >= VIRTIO_GPU_RESP_ERR_UNSPEC &&
                response->type <= VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER)
                gpu.stats.host_errors++;
            else
                gpu.stats.invalid_responses++;
        }
        gpu_batch_pending &= ~bit;
        gpu.stats.commands_completed++;
        if (!gpu_batch_pending && !gpu_batch_waiter)
            gpu.command_busy = false;
        return true;
    }
    return false;
}
static uint32_t gpu_sync_command_used;
static bool gpu_sync_command_waiter;
static uint32_t gpu_command_owner;
static wait_queue_t gpu_command_wait;
static bool handle_sync_completion(void *cookie, uint32_t used) {
    uintptr_t tag = (uintptr_t)cookie;
    if (gpu_sync_command_pending && tag == gpu_sync_command_pending)
        gpu_sync_command_pending = 0;
    else
        return false;
    gpu.stats.commands_completed++;
    gpu_sync_command_used = used;
    if (!gpu_sync_command_waiter)
        gpu.command_busy = false;
    return true;
}
static void gpu_reap_control_locked(void) {
    void *cookie;
    uint32_t used;
    while (virtq_poll_complete(&gpu.controlq, &cookie, &used))
        if (!handle_batch_completion(cookie, used) && !handle_sync_completion(cookie, used))
            gpu.stats.invalid_responses++;
}
static bool gpu_control_ready(void) {
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    if (gpu.command_busy)
        gpu_reap_control_locked();
    bool ready = !gpu.command_busy;
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    return ready;
}
static bool disable_scanout(uint32_t head);
static bool response_valid(uint32_t t) {
    return (t >= VIRTIO_GPU_RESP_OK_NODATA && t <= VIRTIO_GPU_RESP_OK_MAP_INFO) ||
           (t >= VIRTIO_GPU_RESP_ERR_UNSPEC && t <= VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
}
static void gpu_wait_for_control(uint64_t start) {
    struct thread *self = sched_get_current();
    if (!gpu_worker_started || !self || !hal_irq_enabled()) {
        __asm__ volatile("pause");
        return;
    }
    wait_queue_entry_t entry = {.thread = self};
    uint64_t elapsed = lapic_timer_get_ms() - start;
    if (elapsed >= GPU_TIMEOUT_MS)
        return;
    hal_irq_state_t irq_flags = hal_irq_save();
    self->wakeup_ticks = lapic_timer_get_ticks() +
        (gpu_irq_installed ? GPU_TIMEOUT_MS - elapsed : 1);
    self->state = THREAD_BLOCKED;
    wait_queue_add(&gpu_command_wait, &entry);
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    bool ready = (!gpu_sync_command_pending && !gpu_batch_pending) ||
        gpu.controlq.last_used_idx != gpu.controlq.used->idx ||
        lapic_timer_get_ms() - start >= GPU_TIMEOUT_MS;
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    if (ready) {
        wait_queue_remove(&gpu_command_wait, &entry);
        if (self->state == THREAD_BLOCKED)
            self->state = THREAD_RUNNING;
        self->wakeup_ticks = 0;
        hal_irq_restore(irq_flags);
    } else {
        hal_irq_restore(irq_flags);
        sched_yield();
        wait_queue_remove(&gpu_command_wait, &entry);
        if (self->state == THREAD_BLOCKED)
            self->state = THREAD_RUNNING;
    }
    self->wakeup_ticks = 0;
}
static bool gpu_command_locked(
    const void *req, uint32_t req_len, void *resp, uint32_t resp_len, uint32_t *resp_type) {
    if (gpu.command_busy)
        gpu_reap_control_locked();
    if (!gpu.initialized || !gpu.command_dma || !req || !resp || gpu.command_busy ||
        req_len < sizeof(struct virtio_gpu_ctrl_hdr) || req_len > REQ_CAP ||
        resp_len < sizeof(struct virtio_gpu_ctrl_hdr) || resp_len > RESP_CAP)
        return false;
    gpu.command_busy = true;
    uint8_t *base = (uint8_t *)gpu.command_dma->virt;
    memset(base + REQ_OFF, 0, REQ_CAP);
    memset(base + RESP_OFF, 0, RESP_CAP);
    memcpy(base + REQ_OFF, req, req_len);
    uint64_t seq = __atomic_add_fetch(&gpu.next_fence_id, 1, __ATOMIC_RELAXED);
    struct virtio_gpu_ctrl_hdr *wire = (struct virtio_gpu_ctrl_hdr *)(base + REQ_OFF);
    wire->flags |= VIRTIO_GPU_FLAG_FENCE;
    wire->fence_id = seq;
    struct virtq_iov out = {gpu.command_dma->phys + REQ_OFF, req_len};
    struct virtq_iov in = {gpu.command_dma->phys + RESP_OFF, resp_len};
    if (virtq_submit(&gpu.controlq, &out, 1, &in, 1, (void *)(uintptr_t)seq) < 0) {
        gpu.command_busy = false;
        return false;
    }
    gpu_sync_command_pending = seq;
    gpu_sync_command_waiter = true;
    gpu_sync_command_used = 0;
    gpu.stats.commands_submitted++;

    uint64_t start = lapic_timer_get_ms();
    for (;;) {
        gpu_reap_control_locked();
        if (!gpu_sync_command_pending) {
            uint32_t used_len = gpu_sync_command_used;
            gpu_sync_command_waiter = false;
            if (used_len < sizeof(struct virtio_gpu_ctrl_hdr) || used_len > resp_len) {
                gpu.stats.invalid_responses++;
                gpu.command_busy = false;
                return false;
            }
            struct virtio_gpu_ctrl_hdr *h = (struct virtio_gpu_ctrl_hdr *)(base + RESP_OFF);
            if (!response_valid(h->type) || !(h->flags & VIRTIO_GPU_FLAG_FENCE) ||
                h->fence_id != seq) {
                gpu.stats.invalid_responses++;
                gpu.command_busy = false;
                return false;
            }
            memcpy(resp, h, used_len);
            if (used_len < resp_len)
                memset((uint8_t *)resp + used_len, 0, resp_len - used_len);
            if (resp_type)
                *resp_type = h->type;
            if (h->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
                gpu.stats.host_errors++;
            gpu.command_busy = false;
            return true;
        }
        if (lapic_timer_get_ms() - start >= GPU_TIMEOUT_MS) {
            gpu.stats.commands_timed_out++;
            gpu_sync_command_waiter = false;
            return false;
        }
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
        gpu_wait_for_control(start);
        LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    }
}
static bool
gpu_command(const void *req, uint32_t req_len, void *resp, uint32_t resp_len, uint32_t *resp_type) {
    while (__atomic_exchange_n(&gpu_command_owner, 1, __ATOMIC_ACQUIRE)) {
        /* Console flushing holds its IRQ-saving lock. An earlier command
         * may have parked on this CPU waiting for a control-queue interrupt;
         * spinning here would prevent both its wakeup and its resumption.
         * Atomic callers must retry later instead of waiting for the owner. */
        if (!hal_irq_enabled())
            return false;
        if (gpu_worker_started && sched_get_current() && hal_irq_enabled())
            sched_yield();
        else
            __asm__ volatile("pause");
    }
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    bool ok = gpu_command_locked(req, req_len, resp, resp_len, resp_type);
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    __atomic_store_n(&gpu_command_owner, 0, __ATOMIC_RELEASE);
    return ok;
}
static bool gpu_command_batch(const void *const *requests, const uint32_t *lengths,
                               uint32_t count) {
    while (__atomic_exchange_n(&gpu_command_owner, 1, __ATOMIC_ACQUIRE)) {
        if (!hal_irq_enabled())
            return false;
        if (gpu_worker_started && sched_get_current() && hal_irq_enabled())
            sched_yield();
        else
            __asm__ volatile("pause");
    }
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    gpu_reap_control_locked();
    bool ok = false;
    if (!gpu.initialized || !gpu.present_dma || gpu.command_busy ||
        !count || count > GPU_BATCH_COMMANDS)
        goto release;
    for (uint32_t index = 0; index < count; index++)
        if (!requests[index] || lengths[index] < sizeof(struct virtio_gpu_ctrl_hdr) ||
            lengths[index] > GPU_BATCH_RESP_OFF)
            goto release;
    gpu.command_busy = true;
    gpu_batch_waiter = true;
    gpu_batch_failed = false;
    uint32_t submitted = 0;
    for (uint32_t index = 0; index < count; index++) {
        uint8_t *slot = (uint8_t *)gpu.present_dma->virt + index * GPU_BATCH_SLOT_SIZE;
        memset(slot, 0, GPU_BATCH_SLOT_SIZE);
        memcpy(slot, requests[index], lengths[index]);
        struct virtio_gpu_ctrl_hdr *header = (void *)slot;
        gpu_batch_fences[index] = __atomic_add_fetch(&gpu.next_fence_id, 1, __ATOMIC_RELAXED);
        header->flags |= VIRTIO_GPU_FLAG_FENCE;
        header->fence_id = gpu_batch_fences[index];
        struct virtq_iov outgoing = {gpu.present_dma->phys + index * GPU_BATCH_SLOT_SIZE, lengths[index]};
        struct virtq_iov incoming = {gpu.present_dma->phys + index * GPU_BATCH_SLOT_SIZE +
                                    GPU_BATCH_RESP_OFF, sizeof(*header)};
        if (virtq_submit_deferred(&gpu.controlq, &outgoing, 1, &incoming, 1,
                                  &gpu_batch_fences[index]) < 0)
            break;
        gpu_batch_pending |= 1U << index;
        submitted++;
        gpu.stats.commands_submitted++;
    }
    if (submitted)
        virtq_kick(&gpu.controlq);
    uint64_t start = lapic_timer_get_ms();
    for (;;) {
        gpu_reap_control_locked();
        if (!gpu_batch_pending) {
            ok = submitted == count && !gpu_batch_failed;
            gpu_batch_waiter = false;
            gpu.command_busy = false;
            break;
        }
        if (lapic_timer_get_ms() - start >= GPU_TIMEOUT_MS) {
            gpu.stats.commands_timed_out++;
            gpu_batch_waiter = false;
            break;
        }
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
        gpu_wait_for_control(start);
        LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    }
release:
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    __atomic_store_n(&gpu_command_owner, 0, __ATOMIC_RELEASE);
    return ok;
}
static struct pci_device *find_gpu(void) {
    for (uint32_t i = 0; i < pci_get_device_count(); i++) {
        struct pci_device *p = asc_pci_get_device(i);
        if (p && p->vendor_id == VIRTIO_GPU_PCI_VENDOR_ID &&
            p->device_id == VIRTIO_GPU_PCI_DEVICE_ID_MODERN)
            return p;
    }
    return NULL;
}
static void read_config(void) {
    uint8_t a, b;
    do {
        a = gpu.transport.common->config_generation;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        gpu.num_scanouts = gpu.config->num_scanouts;
        gpu.num_capsets = gpu.config->num_capsets;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = gpu.transport.common->config_generation;
    } while (a != b);
}
bool virtio_gpu_get_display_info(struct virtio_gpu_resp_display_info *out) {
    if (!gpu.initialized || !out)
        return false;
    struct virtio_gpu_ctrl_hdr req;
    memset(&req, 0, sizeof(req));
    req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    struct virtio_gpu_resp_display_info resp;
    uint32_t type = 0;
    if (!gpu_command(&req, sizeof(req), &resp, sizeof(resp), &type) ||
        type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
        return false;
    *out = resp;
    return true;
}

static void gpu_mode_add(
    struct virtio_gpu_scanout_state *s, uint32_t w, uint32_t h, uint32_t refresh, bool preferred) {
    if (!s || !w || !h || w > 8192 || h > 8192)
        return;
    if (!refresh)
        refresh = 60;
    for (uint32_t i = 0; i < s->mode_count; i++)
        if (s->modes[i].width == w && s->modes[i].height == h && s->modes[i].refresh == refresh) {
            if (preferred)
                s->modes[i].preferred = 1;
            return;
        }
    if (s->mode_count >= VIRTIO_GPU_MAX_MODES)
        return;
    struct virtio_gpu_mode *m = &s->modes[s->mode_count++];
    m->width = (uint16_t)w;
    m->height = (uint16_t)h;
    m->refresh = (uint16_t)refresh;
    m->preferred = preferred ? 1 : 0;
}
static void gpu_parse_dtd(struct virtio_gpu_scanout_state *s, const uint8_t *d) {
    uint32_t clock = (uint32_t)(d[0] | ((uint32_t)d[1] << 8)) * 10000U;
    if (!clock)
        return;
    uint32_t w = d[2] | ((uint32_t)(d[4] & 0xF0) << 4);
    uint32_t hb = d[3] | ((uint32_t)(d[4] & 0x0F) << 8);
    uint32_t h = d[5] | ((uint32_t)(d[7] & 0xF0) << 4);
    uint32_t vb = d[6] | ((uint32_t)(d[7] & 0x0F) << 8);
    uint64_t total = (uint64_t)(w + hb) * (h + vb);
    gpu_mode_add(s, w, h, total ? (uint32_t)((clock + total / 2) / total) : 60, false);
}
static bool gpu_edid_valid(const uint8_t *e, uint32_t size) {
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (size < 128 || memcmp(e, header, 8))
        return false;
    uint32_t blocks = 1U + e[126];
    if (blocks > size / 128U)
        return false;
    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t sum = 0;
        for (uint32_t i = 0; i < 128; i++)
            sum = (uint8_t)(sum + e[b * 128U + i]);
        if (sum)
            return false;
    }
    return true;
}
static bool gpu_read_edid(uint32_t scanout, struct virtio_gpu_scanout_state *s) {
    if (!(gpu.negotiated_features & VIRTIO_GPU_F_EDID) || !s)
        return false;
    struct virtio_gpu_get_edid req;
    struct virtio_gpu_resp_edid resp;
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_GET_EDID;
    req.scanout = scanout;
    uint32_t type = 0;
    gpu.stats.edid_reads++;
    if (!gpu_command(&req, sizeof(req), &resp, sizeof(resp), &type) ||
        type != VIRTIO_GPU_RESP_OK_EDID || resp.size > sizeof(resp.edid) ||
        !gpu_edid_valid(resp.edid, resp.size)) {
        gpu.stats.edid_failures++;
        return false;
    }
    s->edid_valid = true;
    s->edid_size = resp.size;
    memcpy(s->edid, resp.edid, resp.size);
    for (uint32_t off = 54; off + 18 <= 126; off += 18)
        gpu_parse_dtd(s, resp.edid + off);
    for (uint32_t off = 38; off < 54; off += 2) {
        uint8_t a = resp.edid[off], b = resp.edid[off + 1];
        if (a == 1 && b == 1)
            continue;
        uint32_t w = (a + 31U) * 8U, h;
        switch (b >> 6) {
        case 0:
            h = w * 10U / 16U;
            break;
        case 1:
            h = w * 3U / 4U;
            break;
        case 2:
            h = w * 4U / 5U;
            break;
        default:
            h = w * 9U / 16U;
            break;
        }
        gpu_mode_add(s, w, h, (b & 63U) + 60U, false);
    }
    uint32_t blocks = 1U + resp.edid[126];
    for (uint32_t block = 1; block < blocks; block++) {
        const uint8_t *x = resp.edid + block * 128U;
        if (x[0] != 0x02)
            continue;
        uint32_t end = x[2];
        if (end < 4 || end > 127)
            continue;
        for (uint32_t off = end; off + 18 <= 127; off += 18)
            gpu_parse_dtd(s, x + off);
    }
    gpu.stats.edid_modes += s->mode_count;
    return true;
}
static bool gpu_refresh_displays(void) {
    struct virtio_gpu_resp_display_info info;
    if (!virtio_gpu_get_display_info(&info))
        return false;
    gpu.display_info = info;
    gpu.stats.display_refreshes++;
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        struct virtio_gpu_scanout_state *s = &gpu.scanouts[i];
        memset(s, 0, sizeof(*s));
        struct virtio_gpu_display_one *d = &info.pmodes[i];
        s->enabled = d->enabled && d->r.width && d->r.height;
        if (!s->enabled)
            continue;
        gpu_mode_add(s, d->r.width, d->r.height, 60, true);
        gpu_read_edid(i, s);
    }
    return true;
}

static void virtio_gpu_irq(struct registers *regs) {
    (void)regs;
    if (!gpu.transport.isr)
        return;
    uint8_t status = *gpu.transport.isr;
    if (!status)
        return;
    gpu.stats.interrupts++;
    gpu.stats.intx_interrupts++;
    uint32_t pending = 0;
    if (status & 1U) {
        pending |= GPU_WORK_CONTROL | GPU_WORK_CURSOR;
        gpu.stats.control_interrupts++;
    }
    if (status & 2U) {
        pending |= GPU_WORK_CONFIG;
        gpu.stats.config_interrupts++;
    }
    __atomic_fetch_or(&gpu_pending_work, pending, __ATOMIC_RELEASE);
    if (pending & GPU_WORK_CONTROL)
        wait_queue_wake_all(&gpu_command_wait);
    wait_queue_wake_one(&gpu_worker_wait);
}

static void virtio_gpu_msix_irq(struct registers *regs) {
    uint32_t pending = 0;
    gpu.stats.interrupts++;
    gpu.stats.msix_interrupts++;
    if (regs->int_no == gpu_msix_vectors[0]) {
        pending = GPU_WORK_CONTROL;
        gpu.stats.control_interrupts++;
    } else if (regs->int_no == gpu_msix_vectors[1]) {
        pending = GPU_WORK_CURSOR;
        gpu.stats.cursor_interrupts++;
    } else if (regs->int_no == gpu_msix_vectors[2]) {
        pending = GPU_WORK_CONFIG;
        gpu.stats.config_interrupts++;
    }
    if (pending)
        __atomic_fetch_or(&gpu_pending_work, pending, __ATOMIC_RELEASE);
    if (pending & GPU_WORK_CONTROL)
        wait_queue_wake_all(&gpu_command_wait);
    wait_queue_wake_one(&gpu_worker_wait);
}

static bool virtio_gpu_setup_msix(void) {
    if (!virtio_pci_msix_init(&gpu.transport) || gpu.transport.msix.table_size < 3)
        return false;
    for (uint32_t i = 0; i < 3; i++) {
        int vector = interrupt_vector_alloc(virtio_gpu_msix_irq);
        if (vector < 0)
            goto fail;
        gpu_msix_vectors[i] = (uint8_t)vector;
        if (!virtio_pci_msix_route(
                &gpu.transport, (uint16_t)i, (uint8_t)vector, (uint8_t)lapic_get_id()))
            goto fail;
    }
    if (!virtio_pci_msix_enable(&gpu.transport) ||
        !virtio_pci_msix_assign_queue(&gpu.transport, GPU_CONTROLQ, 0) ||
        !virtio_pci_msix_assign_queue(&gpu.transport, GPU_CURSORQ, 1) ||
        !virtio_pci_msix_assign_config(&gpu.transport, 2))
        goto fail;
    for (uint16_t i = 0; i < 3; i++)
        pci_msix_mask(&gpu.transport.msix, i, false);
    gpu_irq_mode = GPU_IRQ_MSIX;
    gpu_irq_installed = true;
    return true;
fail:
    virtio_pci_msix_disable(&gpu.transport);
    for (uint32_t i = 0; i < 3; i++)
        if (gpu_msix_vectors[i] != 0xFF) {
            interrupt_vector_free(gpu_msix_vectors[i]);
            gpu_msix_vectors[i] = 0xFF;
        }
    return false;
}

static bool virtio_gpu_setup_irq(void) {
    if (gpu_irq_installed)
        return true;
    if (virtio_gpu_setup_msix())
        return true;
    struct pci_device *pci = gpu.transport.pci;
    if (pci && pci->irq_line < 224 &&
        irq_install_handler(pci->irq_line, virtio_gpu_irq, 0x000F)) {
        uint16_t command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
        pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, command & ~(1U << 10));
        gpu_irq_mode = GPU_IRQ_INTX;
        gpu_irq_installed = true;
        return true;
    }
    return false;
}

bool virtio_gpu_init(void) {
    memset(&gpu, 0, sizeof(gpu));
    spinlock_init(&gpu_poll_lock);
    spinlock_init(&gpu_present_lock);
    wait_queue_init(&gpu_worker_wait);
    wait_queue_init(&gpu_command_wait);
    struct pci_device *pci = find_gpu();
    if (!pci) {
        klog_puts("[VIRTIO-GPU] Modern PCI device 1af4:1050 not found\n");
        return false;
    }
    klog_puts("[VIRTIO-GPU] Found PCI device\n");
    if (!virtio_pci_init(&gpu.transport, pci) || !gpu.transport.device_cfg) {
        klog_puts("[VIRTIO-GPU] Transport capabilities unavailable\n");
        return false;
    }
    gpu.config = (volatile struct virtio_gpu_config *)gpu.transport.device_cfg;
    uint64_t wanted = VIRTIO_F_VERSION_1 | VIRTIO_GPU_F_EDID;
    if (!virtio_pci_negotiate(
            &gpu.transport, wanted, VIRTIO_F_VERSION_1, &gpu.negotiated_features)) {
        klog_puts("[VIRTIO-GPU] Feature negotiation failed\n");
        return false;
    }
    if (!virtio_pci_setup_queue(&gpu.transport, GPU_CONTROLQ, &gpu.controlq) ||
        !virtio_pci_setup_queue(&gpu.transport, GPU_CURSORQ, &gpu.cursorq)) {
        klog_puts("[VIRTIO-GPU] Queue setup failed\n");
        virtio_pci_set_failed(&gpu.transport);
        return false;
    }
    gpu.command_dma = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT);
    gpu.present_dma = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT);
    if (!gpu.command_dma || !gpu.present_dma) {
        klog_puts("[VIRTIO-GPU] Command DMA allocation failed\n");
        virtio_pci_set_failed(&gpu.transport);
        return false;
    }
    virtio_gpu_setup_irq();
    read_config();
    if (!virtio_pci_set_driver_ok(&gpu.transport)) {
        klog_puts("[VIRTIO-GPU] DRIVER_OK rejected\n");
        virtio_pci_set_failed(&gpu.transport);
        return false;
    }
    gpu.initialized = true;
    extern const char *kernel_boot_cmdline;
    const char *batch_option = kernel_boot_cmdline ?
        strstr(kernel_boot_cmdline, "virtio_gpu.present_batch=1") : NULL;
    size_t batch_length = sizeof("virtio_gpu.present_batch=1") - 1;
    gpu_present_batch_enabled = batch_option &&
        (batch_option == kernel_boot_cmdline || batch_option[-1] == ' ') &&
        (batch_option[batch_length] == 0 || batch_option[batch_length] == ' ');
    if (gpu_present_batch_enabled)
        klog_puts("[VIRTIO-GPU] Batched scanout commands enabled; immutable DMA staging retained\n");
    if (!gpu_refresh_displays()) {
        klog_puts("[VIRTIO-GPU] GET_DISPLAY_INFO failed\n");
        virtio_pci_set_failed(&gpu.transport);
        gpu.initialized = false;
        return false;
    }
    klog_puts("[VIRTIO-GPU] Ready: scanouts=");
    klog_uint64(gpu.num_scanouts);
    klog_puts(" capsets=");
    klog_uint64(gpu.num_capsets);
    klog_puts(" EDID=");
    klog_putchar((gpu.negotiated_features & VIRTIO_GPU_F_EDID) ? 49 : 48);
    klog_puts("\n");
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        struct virtio_gpu_display_one *m = &gpu.display_info.pmodes[i];
        if (!m->enabled)
            continue;
        klog_puts("[VIRTIO-GPU] scanout ");
        klog_uint64(i);
        klog_puts(": ");
        klog_uint64(m->r.width);
        klog_putchar(120);
        klog_uint64(m->r.height);
        klog_puts("\n");
    }
    return true;
}
bool virtio_gpu_phase2_stress_test(uint32_t iterations) {
    if (!gpu.initialized || !iterations)
        return false;
    klog_puts("[VIRTIO-GPU] Phase 2 command stress starting: iterations=");
    klog_uint64(iterations);
    klog_puts("\n");
    for (uint32_t i = 0; i < iterations; i++) {
        struct virtio_gpu_resp_display_info r;
        if (!virtio_gpu_get_display_info(&r) || !r.pmodes[0].enabled || !r.pmodes[0].r.width ||
            !r.pmodes[0].r.height) {
            klog_puts("[VIRTIO-GPU] FAIL: display-info iteration ");
            klog_uint64(i);
            klog_puts("\n");
            return false;
        }
    }
    struct virtio_gpu_ctrl_hdr bad, answer;
    memset(&bad, 0, sizeof(bad));
    bad.type = 0xDEAD;
    uint32_t type = 0;
    if (!gpu_command(&bad, sizeof(bad), &answer, sizeof(answer), &type) ||
        type < VIRTIO_GPU_RESP_ERR_UNSPEC) {
        klog_puts("[VIRTIO-GPU] FAIL: invalid command response\n");
        return false;
    }
    struct virtio_gpu_resp_display_info recovery;
    if (!virtio_gpu_get_display_info(&recovery)) {
        klog_puts("[VIRTIO-GPU] FAIL: host-error recovery\n");
        return false;
    }
    struct virtq_stats qs;
    virtq_get_stats(&gpu.controlq, &qs);
    if (!virtq_is_idle(&gpu.controlq) ||
        gpu.stats.commands_submitted != gpu.stats.commands_completed ||
        gpu.stats.commands_timed_out || gpu.stats.invalid_responses ||
        qs.submitted != qs.completed) {
        klog_puts("[VIRTIO-GPU] FAIL: final counters mismatch\n");
        return false;
    }
    klog_puts("[VIRTIO-GPU] PASS: 100000 display queries, host-error recovery, idle queue\n");
    return true;
}
static void init_hdr(struct virtio_gpu_ctrl_hdr *h, uint32_t type) {
    memset(h, 0, sizeof(*h));
    h->type = type;
}
static bool nodata(const void *req, uint32_t len) {
    struct virtio_gpu_ctrl_hdr resp;
    uint32_t type = 0;
    return gpu_command(req, len, &resp, sizeof(resp), &type) && type == VIRTIO_GPU_RESP_OK_NODATA;
}
static bool create_2d_format(uint32_t id, uint32_t w, uint32_t h, uint32_t format) {
    struct virtio_gpu_resource_create_2d r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    r.resource_id = id;
    r.format = format;
    r.width = w;
    r.height = h;
    if (!nodata(&r, sizeof(r)))
        return false;
    gpu.resources_live++;
    return true;
}
static bool create_2d(uint32_t id, uint32_t w, uint32_t h) {
    return create_2d_format(id, w, h, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
}
static bool attach_backing(uint32_t id, uint64_t phys, uint32_t len) {
    struct virtio_gpu_resource_attach_backing r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    r.resource_id = id;
    r.nr_entries = 1;
    r.entry.addr = phys;
    r.entry.length = len;
    return nodata(&r, sizeof(r));
}
static bool detach_backing(uint32_t id) {
    struct virtio_gpu_resource_detach_backing r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
    r.resource_id = id;
    return nodata(&r, sizeof(r));
}
static bool unref_resource(uint32_t id) {
    struct virtio_gpu_resource_unref r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    r.resource_id = id;
    if (!nodata(&r, sizeof(r)))
        return false;
    if (gpu.resources_live)
        gpu.resources_live--;
    return true;
}
static bool transfer_2d(uint32_t id, uint32_t w, uint32_t h) {
    struct virtio_gpu_transfer_to_host_2d r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    r.r.width = w;
    r.r.height = h;
    r.resource_id = id;
    return nodata(&r, sizeof(r));
}
static bool transfer_2d_rect(uint32_t id, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t h, uint64_t off) {
    struct virtio_gpu_transfer_to_host_2d r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    r.r.x = x;
    r.r.y = y;
    r.r.width = w;
    r.r.height = h;
    r.offset = off;
    r.resource_id = id;
    return nodata(&r, sizeof(r));
}
static bool flush_rect(uint32_t id, uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h) {
    struct virtio_gpu_resource_flush r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    r.r.x = x;
    r.r.y = y;
    r.r.width = w;
    r.r.height = h;
    r.resource_id = id;
    return nodata(&r, sizeof(r));
}
static bool flush_resource(uint32_t id, uint32_t w, uint32_t h) {
    struct virtio_gpu_resource_flush r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    r.r.width = w;
    r.r.height = h;
    r.resource_id = id;
    return nodata(&r, sizeof(r));
}
static bool set_scanout(uint32_t scanout, uint32_t id, uint32_t w, uint32_t h) {
    struct virtio_gpu_set_scanout r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    r.r.width = w;
    r.r.height = h;
    r.scanout_id = scanout;
    r.resource_id = id;
    return nodata(&r, sizeof(r));
}
static uint32_t scanout_resource_id(void) {
    return __atomic_add_fetch(&gpu.next_resource_id, 1, __ATOMIC_RELAXED);
}
static bool scanout_destroy(uint32_t id, bool counted) {
    struct virtio_gpu_resource_unref req;
    struct virtio_gpu_ctrl_hdr resp;
    uint32_t type = 0;
    memset(&req, 0, sizeof(req));
    init_hdr(&req.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    req.resource_id = id;
    if (!gpu_command(&req, sizeof(req), &resp, sizeof(resp), &type) ||
        (type != VIRTIO_GPU_RESP_OK_NODATA && type != VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID))
        return false;
    if (counted && gpu.resources_live)
        gpu.resources_live--;
    return true;
}
static void scanout_record_selection(uint32_t head, uint32_t id) {
    gpu.head_resource[head] = id;
    gpu.queued_head_resource[head] = id;
    if (head == 0) {
        gpu.scanout_resource_id = id;
        gpu.queued_scanout_resource_id = id;
    }
    gpu.owns_scanout = false;
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
        if (gpu.head_resource[i] && gpu.head_resource[i] != UINT32_MAX)
            gpu.owns_scanout = true;
}
static bool scanout_select(uint32_t head, uint32_t id, uint32_t width, uint32_t height) {
    bool ok = id ? set_scanout(head, id, width, height) : disable_scanout(head);
    if (!ok) {
        gpu.head_resource[head] = UINT32_MAX;
        return false;
    }
    scanout_record_selection(head, id);
    return true;
}
static int scanout_present_batch(uint32_t head, uint32_t id, uint32_t width,
    uint32_t height, const struct vgpu_scanout_damage *damage, bool transfer, bool select) {
    if (!gpu_present_batch_enabled)
        return 0;
    struct virtio_gpu_transfer_to_host_2d upload = {0};
    struct virtio_gpu_set_scanout scanout = {0};
    struct virtio_gpu_resource_flush flush = {0};
    const void *requests[GPU_BATCH_COMMANDS];
    uint32_t lengths[GPU_BATCH_COMMANDS];
    uint32_t count = 0;
    struct virtio_gpu_rect rect = {damage->x, damage->y, damage->width, damage->height};
    if (!rect.width || !rect.height)
        rect = (struct virtio_gpu_rect){0, 0, width, height};
    if (transfer) {
        init_hdr(&upload.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
        upload.r = rect;
        upload.offset = ((uint64_t)rect.y * width + rect.x) * 4;
        upload.resource_id = id;
        requests[count] = &upload;
        lengths[count++] = sizeof(upload);
    }
    if (select) {
        init_hdr(&scanout.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
        scanout.r = (struct virtio_gpu_rect){0, 0, width, height};
        scanout.scanout_id = head;
        scanout.resource_id = id;
        requests[count] = &scanout;
        lengths[count++] = sizeof(scanout);
    }
    init_hdr(&flush.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    flush.r = rect;
    flush.resource_id = id;
    requests[count] = &flush;
    lengths[count++] = sizeof(flush);
    if (!gpu_command_batch(requests, lengths, count)) {
        if (select)
            gpu.head_resource[head] = UINT32_MAX;
        return -1;
    }
    if (select)
        scanout_record_selection(head, id);
    return 1;
}
static const struct vgpu_scanout_ops scanout_ops = {
    .ready = gpu_control_ready, .resource_id = scanout_resource_id,
    .create = create_2d, .attach = attach_backing, .destroy = scanout_destroy,
    .transfer = transfer_2d, .select = scanout_select, .flush = flush_resource,
    .transfer_rect = transfer_2d_rect, .flush_rect = flush_rect,
    .present_batch = scanout_present_batch,
};
static void draw_pattern(uint32_t frame) {
    uint32_t *p = (uint32_t *)gpu.scanout_dma->virt;
    uint32_t w = gpu.scanout_width, h = gpu.scanout_height;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = (x + frame * 7) & 255U, g = (y + frame * 3) & 255U,
                     b = ((x ^ y) + frame * 11) & 255U;
            if (y < 64) {
                uint32_t band = (x / (w / 8)) & 7U;
                r = (band & 1) ? 255 : 32;
                g = (band & 2) ? 255 : 32;
                b = (band & 4) ? 255 : 32;
            }
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
                r = g = b = 255;
            p[(uint64_t)y * w + x] = (r << 16) | (g << 8) | b;
        }
}
bool virtio_gpu_phase3_init_scanout(void) {
    if (!gpu.initialized || gpu.owns_scanout)
        return false;
    struct virtio_gpu_display_one *m = &gpu.display_info.pmodes[0];
    if (!m->enabled || !m->r.width || !m->r.height || m->r.width > 8192 || m->r.height > 8192)
        return false;
    uint64_t bytes = (uint64_t)m->r.width * m->r.height * 4ULL;
    if (bytes > 0xFFFFFFFFULL)
        return false;
    gpu.scanout_dma = dma_alloc((size_t)bytes, DMA_FLAG_32BIT);
    if (!gpu.scanout_dma) {
        klog_puts("[VIRTIO-GPU] Phase 3 framebuffer allocation failed\n");
        return false;
    }
    gpu.scanout_width = m->r.width;
    gpu.scanout_height = m->r.height;
    uint32_t id = __atomic_add_fetch(&gpu.next_resource_id, 1, __ATOMIC_RELAXED);
    if (!create_2d(id, gpu.scanout_width, gpu.scanout_height) ||
        !attach_backing(id, gpu.scanout_dma->phys, (uint32_t)bytes)) {
        if (gpu.resources_live)
            unref_resource(id);
        klog_puts("[VIRTIO-GPU] Phase 3 resource setup failed\n");
        return false;
    }
    draw_pattern(0);
    if (!transfer_2d(id, gpu.scanout_width, gpu.scanout_height) ||
        !set_scanout(0, id, gpu.scanout_width, gpu.scanout_height) ||
        !flush_resource(id, gpu.scanout_width, gpu.scanout_height)) {
        klog_puts("[VIRTIO-GPU] Phase 3 native scanout commit failed\n");
        return false;
    }
    gpu.scanout_resource_id = id;
    gpu.queued_scanout_resource_id = id;
    gpu.head_resource[0] = id;
    gpu.queued_head_resource[0] = id;
    gpu.owns_scanout = true;
    fbcon_scanout_id = id;
    klog_puts("[VIRTIO-GPU] Native scanout active: resource=");
    klog_uint64(id);
    klog_puts(" size=");
    klog_uint64(gpu.scanout_width);
    klog_putchar(120);
    klog_uint64(gpu.scanout_height);
    klog_puts("\n");
    return true;
}
bool virtio_gpu_phase3_stress_test(uint32_t cycles, uint32_t frames) {
    if (!gpu.owns_scanout || !cycles || !frames)
        return false;
    klog_puts("[VIRTIO-GPU] Phase 3 stress: resource cycles=");
    klog_uint64(cycles);
    klog_puts(" frames=");
    klog_uint64(frames);
    klog_puts("\n");
    for (uint32_t i = 0; i < cycles; i++) {
        uint32_t id = __atomic_add_fetch(&gpu.next_resource_id, 1, __ATOMIC_RELAXED);
        if (!create_2d(id, 64, 64) || !attach_backing(id, gpu.scanout_dma->phys, 64U * 64U * 4U) ||
            !transfer_2d(id, 64, 64) || !detach_backing(id) || !unref_resource(id)) {
            klog_puts("[VIRTIO-GPU] FAIL: resource lifecycle at ");
            klog_uint64(i);
            klog_puts("\n");
            return false;
        }
    }
    for (uint32_t i = 1; i <= frames; i++) {
        draw_pattern(i);
        if (!transfer_2d(gpu.scanout_resource_id, gpu.scanout_width, gpu.scanout_height) ||
            !set_scanout(0, gpu.scanout_resource_id, gpu.scanout_width, gpu.scanout_height) ||
            !flush_resource(gpu.scanout_resource_id, gpu.scanout_width, gpu.scanout_height)) {
            klog_puts("[VIRTIO-GPU] FAIL: frame stress at ");
            klog_uint64(i);
            klog_puts("\n");
            return false;
        }
    }
    struct virtq_stats qs;
    virtq_get_stats(&gpu.controlq, &qs);
    if (gpu.resources_live != 1 || !virtq_is_idle(&gpu.controlq) ||
        gpu.stats.commands_submitted != gpu.stats.commands_completed ||
        gpu.stats.commands_timed_out || gpu.stats.invalid_responses ||
        qs.submitted != qs.completed) {
        klog_puts("[VIRTIO-GPU] FAIL: Phase 3 final state mismatch\n");
        return false;
    }
    klog_puts(
        "[VIRTIO-GPU] PASS: native scanout, 10000 resource lifecycles, fenced frame transfers\n");
    return true;
}
#define GPU_CURSOR_SLOTS       16U
#define GPU_CURSOR_IMAGE_BYTES (64U * 64U * 4U)
struct gpu_cursor_slot {
    bool in_use;
};
static struct gpu_cursor_slot cursor_slots[GPU_CURSOR_SLOTS];
static dma_buffer_t *cursor_command_dma, *cursor_shape_dma;
static spinlock_t gpu_cursor_lock, gpu_cursor_shape_lock;
static uint32_t cursor_resource_id, cursor_in_flight, cursor_active_head;
static uint64_t cursor_phase7_started_ms;
static bool cursor_pending_move, cursor_visible, cursor_phase7_reported;
static int32_t cursor_pending_x, cursor_pending_y;
static uint32_t cursor_pending_resource, cursor_pending_head;
static int32_t cursor_pending_hot_x, cursor_pending_hot_y;
static struct drm_gem_object *cursor_last_gem;
static uint32_t cursor_last_width, cursor_last_height, cursor_last_pitch;
static uint32_t cursor_last_head, cursor_shape_reports;
static int32_t cursor_last_hot_x, cursor_last_hot_y;

static uint32_t cursor_coord(int32_t value, uint32_t limit) {
    if (value < 0)
        return 0;
    uint32_t v = (uint32_t)value;
    return v > limit ? limit : v;
}

static bool cursor_submit_locked(
    uint32_t head, uint32_t type, uint32_t resource_id,
    int32_t x, int32_t y, int32_t hot_x, int32_t hot_y) {
    uint32_t slot = GPU_CURSOR_SLOTS;
    for (uint32_t i = 0; i < GPU_CURSOR_SLOTS; i++)
        if (!cursor_slots[i].in_use) {
            slot = i;
            break;
        }
    if (slot == GPU_CURSOR_SLOTS)
        return false;
    struct virtio_gpu_update_cursor *cmd =
        (void *)((uint8_t *)cursor_command_dma->virt + slot * 64U);
    memset(cmd, 0, 64);
    init_hdr(&cmd->hdr, type);
    cmd->pos.scanout_id = head;
    uint32_t cw = gpu.display_info.pmodes[head].r.width,
             ch = gpu.display_info.pmodes[head].r.height;
    cmd->pos.x = cursor_coord(x, cw + 63U);
    cmd->pos.y = cursor_coord(y, ch + 63U);
    cmd->resource_id = resource_id;
    cmd->hot_x = cursor_coord(hot_x, 63);
    cmd->hot_y = cursor_coord(hot_y, 63);
    struct virtq_iov out = {cursor_command_dma->phys + slot * 64U, sizeof(*cmd)};
    cursor_slots[slot].in_use = true;
    if (virtq_submit(&gpu.cursorq, &out, 1, NULL, 0, &cursor_slots[slot]) < 0) {
        cursor_slots[slot].in_use = false;
        return false;
    }
    cursor_in_flight++;
    if (cursor_in_flight > gpu.stats.cursor_max_in_flight)
        gpu.stats.cursor_max_in_flight = cursor_in_flight;
    gpu.stats.cursor_commands++;
    if (type == VIRTIO_GPU_CMD_MOVE_CURSOR)
        gpu.stats.cursor_moves++;
    else if (resource_id)
        gpu.stats.cursor_updates++;
    else
        gpu.stats.cursor_hides++;
    return true;
}

static bool cursor_submit(
    uint32_t type, uint32_t resource_id, int32_t x, int32_t y, int32_t hot_x, int32_t hot_y) {
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_CURSOR, &gpu_cursor_lock);
    bool ok = cursor_submit_locked(cursor_active_head, type, resource_id,
                                   x, y, hot_x, hot_y);
    /* A newer queued command supersedes a move retained when the queue was
     * full. Replaying it after a hide/show or a newer move would restore stale
     * coordinates (and can hide the newly shown cursor). */
    if (ok)
        cursor_pending_move = false;
    if (!ok && type == VIRTIO_GPU_CMD_MOVE_CURSOR) {
        cursor_pending_move = true;
        cursor_pending_x = x;
        cursor_pending_y = y;
        cursor_pending_resource = resource_id;
        cursor_pending_head = cursor_active_head;
        cursor_pending_hot_x = hot_x;
        cursor_pending_hot_y = hot_y;
        gpu.stats.cursor_coalesced++;
        ok = true;
    }
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_CURSOR, &gpu_cursor_lock);
    return ok;
}

static void gpu_drain_cursor_completions(void) {
    for (;;) {
        void *cookie = NULL;
        uint32_t used = 0;
        if (!virtq_poll_complete(&gpu.cursorq, &cookie, &used))
            break;
        (void)used;
        LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_CURSOR, &gpu_cursor_lock);
        struct gpu_cursor_slot *slot = (struct gpu_cursor_slot *)cookie;
        if ((uintptr_t)slot < (uintptr_t)cursor_slots ||
            (uintptr_t)slot >= (uintptr_t)(cursor_slots + GPU_CURSOR_SLOTS) || !slot->in_use)
            gpu.stats.cursor_failures++;
        else {
            slot->in_use = false;
            if (cursor_in_flight)
                cursor_in_flight--;
            gpu.stats.cursor_completions++;
        }
        if (cursor_pending_move) {
            int32_t x = cursor_pending_x, y = cursor_pending_y;
            cursor_pending_move = false;
            /* Keep the exact command state. cursor_visible is changed after
             * submission by the producer, so sampling it here races show/hide
             * and can turn a valid deferred move into resource_id=0 (hide). */
            if (!cursor_submit_locked(cursor_pending_head,
                                      VIRTIO_GPU_CMD_MOVE_CURSOR,
                                      cursor_pending_resource,
                                      x,
                                      y,
                                      cursor_pending_hot_x,
                                      cursor_pending_hot_y)) {
                cursor_pending_move = true;
                cursor_pending_x = x;
                cursor_pending_y = y;
            }
        }
        if (!cursor_phase7_reported &&
            lapic_timer_get_ms() - cursor_phase7_started_ms >= 3600000ULL &&
            gpu.stats.cursor_moves >= 10000000ULL && gpu.stats.cursor_updates >= 1000ULL &&
            gpu.stats.cursor_failures == 0 &&
            gpu.stats.cursor_commands == gpu.stats.cursor_completions + cursor_in_flight) {
            cursor_phase7_reported = true;
            klog_puts("[VIRTIO-GPU] PASS: Phase 7 10000000 cursor moves, shape/hotspot churn, "
                      "independent cursorq\n");
        }
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_CURSOR, &gpu_cursor_lock);
    }
}

static bool cursor_submit_important(
    uint32_t type, uint32_t resource_id, int32_t x, int32_t y, int32_t hot_x, int32_t hot_y) {
    if (cursor_submit(type, resource_id, x, y, hot_x, hot_y))
        return true;
    gpu_drain_cursor_completions();
    if (cursor_submit(type, resource_id, x, y, hot_x, hot_y))
        return true;
    gpu.stats.cursor_failures++;
    return false;
}

static void gpu_drain_completions(void) {
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    gpu_reap_control_locked();
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
}

/* Reclaim retired generations after late acknowledgements even if the
 * producer stopped issuing commits. Never wait behind a fence-owning task
 * in the completion worker; control/cursor draining must keep progressing. */
static void gpu_collect_scanouts(void) {
    uint32_t available = 0;
    if (!__atomic_compare_exchange_n(&gpu_scanout_busy, &available, 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
        vgpu_scanout_collect(&scanout_heads[i], &scanout_ops);
    gpu_scanout_release();
}

static void gpu_handle_config_event(void) {
    uint32_t events = __atomic_load_n(&gpu.config->events_read, __ATOMIC_ACQUIRE);
    if (!(events & VIRTIO_GPU_EVENT_DISPLAY)) {
        read_config();
        return;
    }
    __atomic_store_n(&gpu.config->events_clear, events, __ATOMIC_RELEASE);
    gpu.stats.config_events++;
    bool old_enabled[VIRTIO_GPU_MAX_SCANOUTS];
    uint32_t old_w[VIRTIO_GPU_MAX_SCANOUTS], old_h[VIRTIO_GPU_MAX_SCANOUTS];
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        old_enabled[i] = gpu.scanouts[i].enabled;
        old_w[i] = gpu.display_info.pmodes[i].r.width;
        old_h[i] = gpu.display_info.pmodes[i].r.height;
    }
    read_config();
    if (!gpu_refresh_displays())
        return;
    if (!gpu_phase8_started)
        return;
    ascentdrm_ensure_outputs(&global_ascentdrm_dev, gpu.num_scanouts);
    for (uint32_t i = 0; i < gpu.num_scanouts && i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        bool changed = old_enabled[i] != gpu.scanouts[i].enabled ||
                       old_w[i] != gpu.display_info.pmodes[i].r.width ||
                       old_h[i] != gpu.display_info.pmodes[i].r.height;
        ascentdrm_update_output_state(&global_ascentdrm_dev, i, gpu.scanouts[i].enabled);
        gpu_scanout_acquire();
        if (old_enabled[i] && !gpu.scanouts[i].enabled && gpu.head_resource[i]) {
            if (vgpu_scanout_detach(&scanout_heads[i], &scanout_ops, i)) {
                gpu.head_resource[i] = 0;
                gpu.queued_head_resource[i] = 0;
                if (i == 0) {
                    gpu.scanout_resource_id = 0;
                    gpu.queued_scanout_resource_id = 0;
                }
            }
        }
        gpu_scanout_release();
        char modes[512];
        bool enabled = false;
        virtio_gpu_scanout_summary(i, &enabled, modes, sizeof(modes));
        sysfs_gpu_update_connector(i, enabled, modes);
        if (changed) {
            char path[192], num[12];
            strcpy(path, sysfs_gpu_devpath);
            strcat(path, "/card0-HDMI-A-");
            char *p = num + sizeof(num);
            *--p = 0;
            uint32_t v = i + 1;
            if (!v)
                *--p = 48;
            while (v) {
                *--p = (char)(48 + v % 10);
                v /= 10;
            }
            strcat(path, p);
            netlink_broadcast_drm_hotplug(path, i + 1);
            gpu.stats.hotplug_events++;
        }
    }
    if (!gpu_phase8_reported && lapic_timer_get_ms() - gpu_phase8_started_ms >= 3600000ULL &&
        gpu.stats.config_events >= 10000ULL) {
        gpu_phase8_reported = true;
        bool pass = gpu.stats.present_failures == 0 && gpu.stats.commands_timed_out == 0 &&
                    gpu.stats.invalid_responses == 0;
        klog_puts(pass ? "[VIRTIO-GPU] PASS: Phase 8 one-hour resize/hotplug stress, live "
                         "EDID/DRM/sysfs synchronization\n"
                       : "[VIRTIO-GPU] FAIL: Phase 8 runtime stress counters\n");
    }
}

static void virtio_gpu_worker(void) {
    struct thread *self = sched_get_current();
    wait_queue_entry_t entry = {.thread = self, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
    for (;;) {
        uint32_t pending = __atomic_exchange_n(&gpu_pending_work, 0, __ATOMIC_ACQ_REL);
        /* Poll periodically only when interrupts are unavailable.  In IRQ mode,
         * waking this worker every millisecond wastes a substantial share of a
         * QEMU vCPU and competes directly with Xorg and the renderer. */
        if (!pending && !gpu_irq_installed)
            gpu.stats.watchdog_polls++;
        if (pending & GPU_WORK_CONFIG)
            gpu_handle_config_event();
        if (pending & GPU_WORK_PERF)
            gpu_perf_dump();
        gpu_drain_completions();
        gpu_collect_scanouts();
        gpu_drain_cursor_completions();
        /* A timer must not switch us out after BLOCKED is published but
         * before the wake entry and fallback deadline exist. Keep the ready
         * recheck and fast-path cleanup inside the same IRQ-masked window. */
        hal_irq_state_t irq_flags = hal_irq_save();
        self->wakeup_ticks = gpu_irq_installed ? 0 : lapic_timer_get_ticks() + 1;
        self->state = THREAD_BLOCKED;
        wait_queue_add(&gpu_worker_wait, &entry);
        if (gpu.controlq.last_used_idx == gpu.controlq.used->idx &&
            gpu.cursorq.last_used_idx == gpu.cursorq.used->idx &&
            !__atomic_load_n(&gpu_pending_work, __ATOMIC_ACQUIRE)) {
            hal_irq_restore(irq_flags);
            sched_yield();
            irq_flags = hal_irq_save();
        }
        self->wakeup_ticks = 0;
        wait_queue_remove(&gpu_worker_wait, &entry);
        self->state = THREAD_RUNNING;
        hal_irq_restore(irq_flags);
    }
}

bool virtio_gpu_phase6_start(void) {
    virtio_gpu_setup_irq();
    struct thread *worker = sched_create_kernel_thread(virtio_gpu_worker, NULL, true);
    if (!worker)
        return false;
    strcpy(worker->comm, "virtio-gpu");
    gpu_worker_started = true;
    wait_queue_init(&gpu_flip_wait);
    struct thread *presenter = sched_create_kernel_thread(virtio_gpu_flip_worker, NULL, true);
    if (!presenter)
        return false;
    strcpy(presenter->comm, "virtio-present");
    gpu_flip_worker_started = true;
    if (gpu_irq_mode == GPU_IRQ_MSIX)
        klog_puts("[VIRTIO-GPU] Phase 6: MSI-X control/cursor/config, scanout-owned frame pairs\n");
    else if (gpu_irq_mode == GPU_IRQ_INTX)
        klog_puts("[VIRTIO-GPU] Phase 6: INTx control/cursor/config, scanout-owned frame pairs\n");
    else
        klog_puts("[VIRTIO-GPU] Phase 6: polling control/cursor/config, scanout-owned frame pairs\n");
    return true;
}

static void virtio_gpu_cursor_update(uint32_t crtc_id,
                                     struct drm_gem_object *gem,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t pitch,
                                     int32_t x,
                                     int32_t y,
                                     int32_t hot_x,
                                     int32_t hot_y,
                                     uint32_t flags) {
    if (!cursor_resource_id)
        return;
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_SHAPE, &gpu_cursor_shape_lock);
    cursor_active_head = ascentdrm_crtc_scanout_id(crtc_id);
    if (cursor_active_head >= VIRTIO_GPU_MAX_SCANOUTS)
        cursor_active_head = 0;
    bool shape = (flags & DRM_MODE_CURSOR_BO) || gem != cursor_last_gem ||
                 cursor_active_head != cursor_last_head ||
                 width != cursor_last_width || height != cursor_last_height ||
                 pitch != cursor_last_pitch || hot_x != cursor_last_hot_x ||
                 hot_y != cursor_last_hot_y;
    if (!gem) {
        if (cursor_visible || (flags & DRM_MODE_CURSOR_BO))
            cursor_submit_important(VIRTIO_GPU_CMD_UPDATE_CURSOR, 0, x, y, hot_x, hot_y);
        LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_CURSOR, &gpu_cursor_lock);
        cursor_visible = false;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_CURSOR, &gpu_cursor_lock);
        cursor_last_gem = NULL;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_SHAPE, &gpu_cursor_shape_lock);
        return;
    }
    if (!width || !height || width > 64 || height > 64 || !gem->virt_addr) {
        gpu.stats.cursor_failures++;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_SHAPE, &gpu_cursor_shape_lock);
        return;
    }
    if (!pitch)
        pitch = width * 4U;
    if (pitch < width * 4U || (uint64_t)(height - 1) * pitch + width * 4U > gem->size) {
        gpu.stats.cursor_failures++;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_SHAPE, &gpu_cursor_shape_lock);
        return;
    }
    bool ok = true;
    if (shape) {
        uint32_t visible_pixels = 0;
        memset(cursor_shape_dma->virt, 0, GPU_CURSOR_IMAGE_BYTES);
        for (uint32_t row = 0; row < height; row++) {
            memcpy((uint8_t *)cursor_shape_dma->virt + row * 64U * 4U,
                   (uint8_t *)gem->virt_addr + row * pitch,
                   width * 4U);
            uint32_t *pixels = (uint32_t *)((uint8_t *)gem->virt_addr + row * pitch);
            for (uint32_t col = 0; col < width; col++)
                if (pixels[col] >> 24) visible_pixels++;
        }
        if (cursor_shape_reports++ < 4) {
            klog_puts("[VIRTIO-GPU] cursor upload: head=");
            klog_uint64(cursor_active_head);
            klog_puts(" visible_pixels=");
            klog_uint64(visible_pixels);
            klog_puts(" resource=");
            klog_uint64(cursor_resource_id);
            klog_puts("\n");
        }
        if (!transfer_2d(cursor_resource_id, 64, 64)) {
            gpu.stats.cursor_failures++;
            ok = false;
        } else
            ok = cursor_submit_important(
                VIRTIO_GPU_CMD_UPDATE_CURSOR, cursor_resource_id, x, y, hot_x, hot_y);
    } else
        ok = cursor_submit(VIRTIO_GPU_CMD_MOVE_CURSOR, cursor_resource_id, x, y, hot_x, hot_y);
    if (ok) {
        LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_CURSOR, &gpu_cursor_lock);
        cursor_visible = true;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_CURSOR, &gpu_cursor_lock);
        cursor_last_gem = gem;
        cursor_last_width = width;
        cursor_last_height = height;
        cursor_last_pitch = pitch;
        cursor_last_head = cursor_active_head;
        cursor_last_hot_x = hot_x;
        cursor_last_hot_y = hot_y;
    }
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_SHAPE, &gpu_cursor_shape_lock);
}

bool virtio_gpu_phase7_start(void) {
    if (!gpu.initialized || !gpu_worker_started)
        return false;
    /* A virtual cursor is positioned by the host pointer, rather than drawn
     * into the scanout. With relative input the frontend captures/hides that
     * pointer; it cannot represent an independently positioned guest cursor.
     * Leave the cursor backend unavailable so DRM clients use their normal
     * software cursor path, including hiding it for relative camera capture.
     * USB enumeration precedes this initialization. */
    if (!mouse_has_absolute_axes()) {
        ascentdrm_register_cursor_backend(NULL);
        klog_puts("[VIRTIO-GPU] Relative pointer: framebuffer cursor required\n");
        return true;
    }
    spinlock_init(&gpu_cursor_lock);
    spinlock_init(&gpu_cursor_shape_lock);
    cursor_shape_dma = dma_alloc(GPU_CURSOR_IMAGE_BYTES, DMA_FLAG_32BIT);
    cursor_command_dma = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT);
    if (!cursor_shape_dma || !cursor_command_dma) {
        if (cursor_shape_dma)
            dma_free(cursor_shape_dma);
        if (cursor_command_dma)
            dma_free(cursor_command_dma);
        return false;
    }
    cursor_resource_id = __atomic_add_fetch(&gpu.next_resource_id, 1, __ATOMIC_RELAXED);
    bool created = create_2d_format(cursor_resource_id, 64, 64, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM);
    if (!created ||
        !attach_backing(cursor_resource_id, cursor_shape_dma->phys, GPU_CURSOR_IMAGE_BYTES)) {
        if (created)
            unref_resource(cursor_resource_id);
        cursor_resource_id = 0;
        dma_free(cursor_shape_dma);
        dma_free(cursor_command_dma);
        cursor_shape_dma = NULL;
        cursor_command_dma = NULL;
        return false;
    }
    cursor_phase7_started_ms = lapic_timer_get_ms();
    ascentdrm_register_cursor_backend(virtio_gpu_cursor_update);
    klog_puts("[VIRTIO-GPU] Phase 7 hardware cursor active: 64x64 ARGB, fenced uploads, "
              "independent cursorq\n");
    return true;
}

bool virtio_gpu_phase8_start(void) {
    if (!gpu.initialized || !gpu_worker_started)
        return false;
    uint8_t malformed[128];
    memset(malformed, 0, sizeof(malformed));
    if (gpu_edid_valid(malformed, sizeof(malformed)))
        return false;
    for (uint32_t i = 0; i < gpu.num_scanouts && i < VIRTIO_GPU_MAX_SCANOUTS; i++)
        if (gpu.scanouts[i].enabled && !gpu.scanouts[i].mode_count)
            return false;
    gpu_phase8_started = true;
    gpu_phase8_started_ms = lapic_timer_get_ms();
    ascentdrm_ensure_outputs(&global_ascentdrm_dev, gpu.num_scanouts);
    for (uint32_t i = 0; i < gpu.num_scanouts && i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        ascentdrm_update_output_state(&global_ascentdrm_dev, i, gpu.scanouts[i].enabled);
        char modes[512];
        bool enabled = false;
        virtio_gpu_scanout_summary(i, &enabled, modes, sizeof(modes));
        sysfs_gpu_update_connector(i, enabled, modes);
    }
    uint32_t pending = __atomic_load_n(&gpu.config->events_read, __ATOMIC_ACQUIRE);
    if (pending & VIRTIO_GPU_EVENT_DISPLAY)
        gpu_handle_config_event();
    klog_puts("[VIRTIO-GPU] Phase 8 display management active: EDID modes, config hotplug, "
              "multihead connectors\n");
    for (uint32_t i = 0; i < gpu.num_scanouts && i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        klog_puts("[VIRTIO-GPU] head ");
        klog_uint64(i);
        klog_puts(gpu.scanouts[i].enabled ? " connected modes=" : " disconnected modes=");
        klog_uint64(gpu.scanouts[i].mode_count);
        klog_puts(gpu.scanouts[i].edid_valid ? " EDID=valid\n" : " EDID=fallback\n");
    }
    return true;
}

static bool disable_scanout(uint32_t head) {
    struct virtio_gpu_set_scanout r;
    memset(&r, 0, sizeof(r));
    init_hdr(&r.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    r.scanout_id = head;
    r.resource_id = 0;
    return nodata(&r, sizeof(r));
}
static void virtio_gpu_restore_console_scanout_locked(void) {
    /* -vga none: no VGA fallback, so disabling leaves "Display output is
     * not active". Re-present a native fbcon resource instead, seeded
     * from the GOP fb (already redrawn with the login prompt). Phase3
     * init never runs on this config, so allocate the native resource
     * on demand the first time Plymouth quits. */
    if (!gpu.initialized)
        return;
    if (!fbcon_scanout_id) {
        uint32_t dw = fb_get_width(), dh = fb_get_height();
        if (!dw || !dh || dw > 8192 || dh > 8192) {
            dw = gpu.scanout_width;
            dh = gpu.scanout_height;
        }
        if (!dw || !dh || dw > 8192 || dh > 8192)
            return;
        uint64_t bytes = (uint64_t)dw * dh * 4ULL;
        if (!bytes || bytes > 0xFFFFFFFFULL)
            return;
        dma_buffer_t *dma = dma_alloc((size_t)bytes, DMA_FLAG_32BIT);
        if (!dma)
            return;
        uint32_t id = __atomic_add_fetch(&gpu.next_resource_id, 1, __ATOMIC_RELAXED);
        if (!create_2d(id, dw, dh) ||
            !attach_backing(id, dma->phys, (uint32_t)bytes)) {
            if (gpu.resources_live)
                unref_resource(id);
            return;
        }
        gpu.scanout_dma = dma;
        gpu.scanout_width = dw;
        gpu.scanout_height = dh;
        fbcon_scanout_id = id;
    }
    if (!gpu.scanout_dma || !gpu.scanout_dma->virt)
        return;
    void *base = fb_get_base();
    uint32_t fw = fb_get_width(), fh = fb_get_height(), pitch = fb_get_pitch();
    uint32_t sw = gpu.scanout_width, sh = gpu.scanout_height;
    if (!base || !fw || !fh || !pitch || !sw || !sh)
        return;
    uint32_t w = fw < sw ? fw : sw;
    uint32_t h = fh < sh ? fh : sh;
    uint8_t *dst = (uint8_t *)gpu.scanout_dma->virt;
    uint8_t *src = (uint8_t *)base;
    uint32_t dst_pitch = sw * 4;
    for (uint32_t y = 0; y < h; y++)
        memcpy(dst + (uint64_t)y * dst_pitch, src + (uint64_t)y * pitch,
               (size_t)w * 4);
    __asm__ volatile("" ::: "memory");
    transfer_2d(fbcon_scanout_id, sw, sh);
    set_scanout(0, fbcon_scanout_id, sw, sh);
    flush_resource(fbcon_scanout_id, sw, sh);
    gpu.head_resource[0] = fbcon_scanout_id;
    gpu.queued_head_resource[0] = fbcon_scanout_id;
    gpu.scanout_resource_id = fbcon_scanout_id;
    gpu.queued_scanout_resource_id = fbcon_scanout_id;
    gpu.owns_scanout = true;
}
void virtio_gpu_restore_console_scanout(void) {
    gpu_scanout_acquire();
    if (!vgpu_scanout_detach(&scanout_heads[0], &scanout_ops, 0)) {
        gpu_scanout_release();
        return;
    }
    virtio_gpu_restore_console_scanout_locked();
    gpu_scanout_release();
}
/* Live console mirror: fb_swap just copied back->front (GOP). While the
 * fbcon resource is scanned out (post-Plymouth text login), copy the same
 * dirty rect front->scanout_dma and present it, so typing/scrolling shows.
 * Skipped when a DRM master (Plymouth splash, SDDM, Weston) owns the head —
 * those present themselves. */
static uint64_t last_console_commit_ms = 0;
static uint32_t pending_cx = 0, pending_cy = 0, pending_cw = 0, pending_ch = 0;
static bool console_commit_pending = false;

void virtio_gpu_console_commit(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!gpu.initialized || !fbcon_scanout_id || !gpu.scanout_dma ||
        !gpu.scanout_dma->virt)
        return;
    if (gpu.head_resource[0] != fbcon_scanout_id)
        return;
    void *base = fb_get_base();
    uint32_t fw = fb_get_width(), fh = fb_get_height(), pitch = fb_get_pitch();
    uint32_t sw = gpu.scanout_width, sh = gpu.scanout_height;
    if (!base || !fw || !fh || !pitch || !sw || !sh || !w || !h)
        return;
    if (x >= fw || y >= fh || x >= sw || y >= sh)
        return;
    if (x + w > fw) w = fw - x;
    if (y + h > fh) h = fh - y;
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (!w || !h)
        return;

    if (!console_commit_pending) {
        pending_cx = x;
        pending_cy = y;
        pending_cw = w;
        pending_ch = h;
        console_commit_pending = true;
    } else {
        uint32_t x1 = pending_cx < x ? pending_cx : x;
        uint32_t y1 = pending_cy < y ? pending_cy : y;
        uint32_t x2 = (pending_cx + pending_cw) > (x + w) ? (pending_cx + pending_cw) : (x + w);
        uint32_t y2 = (pending_cy + pending_ch) > (y + h) ? (pending_cy + pending_ch) : (y + h);
        pending_cx = x1;
        pending_cy = y1;
        pending_cw = x2 - x1;
        pending_ch = y2 - y1;
    }

    uint64_t now = lapic_timer_get_ms();
    /* Rate limit synchronous VirtIO GPU commits to ~60 FPS (16ms)
     * to eliminate busy-spinning and VM exit overhead during rapid console writes. */
    if (now - last_console_commit_ms < 16)
        return;

    last_console_commit_ms = now;
    uint32_t cx = pending_cx, cy = pending_cy, cw = pending_cw, ch = pending_ch;
    console_commit_pending = false;

    uint8_t *dst = (uint8_t *)gpu.scanout_dma->virt;
    uint8_t *src = (uint8_t *)base;
    uint32_t dst_pitch = sw * 4;
    for (uint32_t r = 0; r < ch; r++)
        memcpy(dst + (uint64_t)(cy + r) * dst_pitch + (uint64_t)cx * 4,
               src + (uint64_t)(cy + r) * pitch + (uint64_t)cx * 4,
               (size_t)cw * 4);
    __asm__ volatile("" ::: "memory");
    uint64_t off = (uint64_t)cy * dst_pitch + (uint64_t)cx * 4;
    if (!transfer_2d_rect(fbcon_scanout_id, cx, cy, cw, ch, off) ||
        !flush_rect(fbcon_scanout_id, cx, cy, cw, ch)) {
        /* A command owner can be asleep while this IRQ-masked console
         * flush runs. Keep the damage for the next console commit. */
        pending_cx = cx;
        pending_cy = cy;
        pending_cw = cw;
        pending_ch = ch;
        console_commit_pending = true;
    }
}
int virtio_gpu_prepare_prime_fb(struct drm_gem_object *obj, uint32_t width,
                                uint32_t height, uint32_t pitch, uint32_t offset) {
    if (!gpu.initialized || !obj || !obj->prime_buffer)
        return -22;
    struct vgpu_scanout_source source = {
        .pixels = obj->virt_addr, .size = obj->size, .offset = offset,
        .width = width, .height = height, .pitch = pitch,
        .view_width = width, .view_height = height,
    };
    /* ADDFB validates a source layout; it creates no host resources. Host
     * scanout lifetime is independent of this imported GEM and its handles. */
    return vgpu_scanout_source_valid(&source) ? 0 : -22;
}
static int virtio_gpu_create_dumb(struct drm_device *dev, uint32_t width,
                                  uint32_t height, uint32_t bpp,
                                  struct drm_gem_object **out) {
    if (!gpu.initialized || !dev || !out || bpp != 32 || !width || !height ||
        width > 8192 || height > 8192)
        return -22;
    struct drm_gem_object *obj = ascentdrm_gem_object_create(dev, (size_t)width * height * 4);
    if (!obj)
        return -12;
    *out = obj;
    return 0;
}
static struct drm_framebuffer *virtio_gpu_active_fb(struct drm_device *dev,
                                                    uint32_t target,
                                                    uint32_t *head,
                                                    struct virtio_gpu_rect *view,
                                                    struct drm_framebuffer *snapshot) {
    struct drm_framebuffer *fb = NULL;
    if (head)
        *head = 0;
    if (view)
        memset(view, 0, sizeof(*view));
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *o;
    list_for_each_entry(o, &dev->kms_objects, list) {
        if (o->type == DRM_MODE_OBJECT_CRTC) {
            struct drm_crtc *c = (struct drm_crtc *)o;
            if (c->fb && c->fb->gem_obj &&
                (!target || o->id == target || c->fb->base.id == target)) {
                /* The framebuffer may be removed while fence waits sleep.
                 * Retain the GEM and copy the metadata before dropping the
                 * KMS lock; no live framebuffer pointer leaves this lock. */
                *snapshot = *c->fb;
                fb = snapshot;
                ascentdrm_gem_get(fb->gem_obj);
                if (head)
                    *head = c->scanout_id;
                if (view) {
                    view->x = c->primary ? c->primary->src_x >> 16 : 0;
                    view->y = c->primary ? c->primary->src_y >> 16 : 0;
                    view->width =
                        c->primary && c->primary->src_w ? c->primary->src_w >> 16 : c->fb->width;
                    view->height =
                        c->primary && c->primary->src_h ? c->primary->src_h >> 16 : c->fb->height;
                    if (view->x >= c->fb->width) {
                        view->x = 0;
                        view->width = c->fb->width;
                    } else if (view->x + view->width > c->fb->width)
                        view->width = c->fb->width - view->x;
                    if (view->y >= c->fb->height) {
                        view->y = 0;
                        view->height = c->fb->height;
                    } else if (view->y + view->height > c->fb->height)
                        view->height = c->fb->height - view->y;
                    if (!view->width)
                        view->width = c->fb->width;
                    if (!view->height)
                        view->height = c->fb->height;
                }
                break;
            }
        }
    }
    spinlock_release(&dev->lock);
    return fb;
}
static void virtio_gpu_queue_flip(
    struct drm_file *file, struct vfs_node *node, uint32_t crtc, uint32_t fb, uint64_t user_data) {
    (void)fb;
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
    gpu.pending_flip_file = file;
    gpu.pending_flip_node = node;
    gpu.pending_flip_crtc_id = crtc;
    gpu.pending_flip_user_data = user_data;
    gpu.pending_flip = true;
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
}
static void virtio_gpu_complete_flip(uint32_t head) {
    struct drm_file *file;
    struct vfs_node *node;
    uint64_t user_data;
    uint32_t crtc_id, sequence;
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
    if (!gpu.pending_flip || ascentdrm_crtc_scanout_id(gpu.pending_flip_crtc_id) != head) {
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
        return;
    }
    file = gpu.pending_flip_file;
    node = gpu.pending_flip_node;
    user_data = gpu.pending_flip_user_data;
    crtc_id = gpu.pending_flip_crtc_id;
    sequence = ++gpu.flip_sequence;
    gpu.pending_flip = false;
    gpu.pending_flip_file = NULL;
    gpu.pending_flip_node = NULL;
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
    /* Deliver on the next vblank boundary: deliver on the next boundary
     * after the immutable upload finished.
     * Runs in the commit (ioctl) context with no locks held. */
    vblank_wait_next();
    sequence = vblank_sequence();
    struct drm_event_vblank ev;
    memset(&ev, 0, sizeof(ev));
    uint32_t sec, usec;
    vblank_timestamp(sequence, &sec, &usec);
    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof(ev);
    ev.user_data = user_data;
    ev.tv_sec = sec;
    ev.tv_usec = usec;
    ev.sequence = sequence;
    ev.crtc_id = crtc_id;
    ascentdrm_file_send_event(file, &ev, node);
}
static void virtio_gpu_commit_damage(struct drm_device *dev,
                                     const struct drm_clip_rect *clips,
                                     uint32_t n, uint32_t target) {
    uint32_t head = 0;
    bool snapshot_committed = false;
    struct virtio_gpu_rect view;
    struct drm_framebuffer snapshot;
    uint64_t entered = __atomic_add_fetch(&gpu_prime_enter_seq, 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&gpu_prime_waiters, 1, __ATOMIC_ACQ_REL);
    gpu_scanout_acquire();
    __atomic_sub_fetch(&gpu_prime_waiters, 1, __ATOMIC_ACQ_REL);
    struct drm_framebuffer *fb = virtio_gpu_active_fb(dev, target, &head, &view, &snapshot);
    if (!fb || !fb->gem_obj)
        goto release_scanout;
    if (head >= gpu.num_scanouts || head >= VIRTIO_GPU_MAX_SCANOUTS || fb->bpp != 32)
        goto release_gem;
    bool coalesce = gpu.num_scanouts == 1 && fb->gem_obj->prime_buffer;
    if (coalesce && (__atomic_load_n(&gpu_prime_waiters, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&gpu_prime_enter_seq, __ATOMIC_ACQUIRE) != entered))
        goto release_gem;
    struct vgpu_scanout_source source = {
        .pixels = fb->gem_obj->virt_addr, .size = fb->gem_obj->size,
        .offset = fb->backing_offset, .width = fb->width, .height = fb->height,
        .pitch = fb->pitch, .x = view.x, .y = view.y,
        .view_width = view.width, .view_height = view.height,
    };
    struct vgpu_scanout_damage damage = {0};
    bool have_damage = false;
    if (clips && n && n <= 4096) {
        uint32_t left = view.width, top = view.height, right = 0, bottom = 0;
        for (uint32_t index = 0; index < n; index++) {
            uint32_t x1 = clips[index].x1 > view.x ? clips[index].x1 : view.x;
            uint32_t y1 = clips[index].y1 > view.y ? clips[index].y1 : view.y;
            uint32_t x2 = clips[index].x2 < view.x + view.width ? clips[index].x2 : view.x + view.width;
            uint32_t y2 = clips[index].y2 < view.y + view.height ? clips[index].y2 : view.y + view.height;
            if (x1 >= x2 || y1 >= y2)
                continue;
            x1 -= view.x;
            y1 -= view.y;
            x2 -= view.x;
            y2 -= view.y;
            if (x1 < left) left = x1;
            if (y1 < top) top = y1;
            if (x2 > right) right = x2;
            if (y2 > bottom) bottom = y2;
            have_damage = true;
        }
        if (have_damage)
            damage = (struct vgpu_scanout_damage){left, top, right - left, bottom - top};
    }
    if (!vgpu_scanout_source_valid(&source))
        goto release_gem;
    struct vgpu_scanout_image *image = vgpu_scanout_prepare(&scanout_heads[head],
        &scanout_ops, head, view.width, view.height);
    if (!image)
        goto release_gem;
    uint64_t sync_start = lapic_timer_get_ns();
    if (ascentdrm_gem_begin_cpu(fb->gem_obj))
        goto release_gem;
    uint64_t copy_start = lapic_timer_get_ns();
    uint64_t sync_ns = copy_start - sync_start;
    /* Only this copy holds the producer reservation. QEMU reads independent,
     * tightly packed staging after the reservation is released. Both native
     * and imported framebuffers use exactly the same publication engine. */
    bool copied = vgpu_scanout_copy_damage(&scanout_heads[head], image, &source,
        fb->base.id, have_damage ? &damage : NULL);
    uint64_t copy_end = lapic_timer_get_ns();
    ascentdrm_gem_end_cpu(fb->gem_obj);
    sync_ns += lapic_timer_get_ns() - copy_end;
    if (!copied)
        goto release_gem;
    if (coalesce && __atomic_load_n(&gpu_prime_enter_seq, __ATOMIC_ACQUIRE) != entered)
        goto release_gem;
    uint64_t start = lapic_timer_get_ms();
    uint64_t present_start = lapic_timer_get_ns();
    bool ok = vgpu_scanout_publish(&scanout_heads[head], &scanout_ops, head, image);
    /* Retry an immutable snapshot once. A flush retry never re-uploads a
     * selected image; a timed-out transfer prevents any retry until returned. */
    if (!ok && gpu_control_ready())
        ok = vgpu_scanout_publish(&scanout_heads[head], &scanout_ops, head, image);
    uint64_t elapsed = lapic_timer_get_ms() - start;
    uint64_t present_ns = lapic_timer_get_ns() - present_start;
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    gpu.stats.present_batches++;
    gpu.stats.producer_sync_ns += sync_ns;
    gpu.stats.snapshot_copy_ns += copy_end - copy_start;
    gpu.stats.transport_wait_ns += present_ns;
    gpu.stats.present_wait_ms += elapsed;
    if (elapsed > gpu.stats.max_present_wait_ms)
        gpu.stats.max_present_wait_ms = elapsed;
    if (ok) {
        snapshot_committed = true;
        gpu.stats.frames_presented++;
        gpu.stats.damage_pixels += (uint64_t)image->upload.width * image->upload.height;
        gpu.stats.scanout_pixels += (uint64_t)view.width * view.height;
        gpu.stats.compared_pixels += image->compared_pixels;
        if (!image->upload.width)
            gpu.stats.unchanged_frames++;
    } else {
        gpu.stats.present_failures++;
    }
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
release_gem:
    if (!snapshot_committed && head < VIRTIO_GPU_MAX_SCANOUTS)
        vgpu_scanout_invalidate(&scanout_heads[head]);
    virtio_gpu_complete_flip(head);
    gpu_scanout_release();
    if (ascentdrm_gem_put_last(fb->gem_obj))
        ascentdrm_gem_object_free(dev, fb->gem_obj);
    return;
release_scanout:
    virtio_gpu_complete_flip(head);
    gpu_scanout_release();
}
static int virtio_gpu_submit_flip(struct drm_file *file, struct vfs_node *node,
                                  uint32_t crtc, struct drm_framebuffer *framebuffer,
                                  uint64_t cookie, bool event) {
    if (!gpu_flip_worker_started || !framebuffer || !framebuffer->gem_obj)
        return -95;
    uint32_t head = ascentdrm_crtc_scanout_id(crtc);
    struct vgpu_scanout_source source = {
        .pixels = framebuffer->gem_obj->virt_addr, .size = framebuffer->gem_obj->size,
        .offset = framebuffer->backing_offset, .width = framebuffer->width,
        .height = framebuffer->height, .pitch = framebuffer->pitch,
        .view_width = framebuffer->width, .view_height = framebuffer->height,
    };
    if (head >= gpu.num_scanouts || head >= VIRTIO_GPU_MAX_SCANOUTS ||
        framebuffer->bpp != 32 || !vgpu_scanout_source_valid(&source))
        return -22;
    uint32_t available = 0;
    if (!__atomic_compare_exchange_n(&gpu_async_flip_state, &available, 3, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return -16;
    gpu_async_flip.framebuffer = *framebuffer;
    ascentdrm_gem_get(framebuffer->gem_obj);
    vfs_node_ref(node);
    gpu_async_flip.file = file;
    gpu_async_flip.node = node;
    gpu_async_flip.crtc = crtc;
    gpu_async_flip.head = head;
    gpu_async_flip.cookie = cookie;
    gpu_async_flip.event = event;
    __atomic_store_n(&gpu_async_flip_state, 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(&gpu_flip_wait);
    return 0;
}

static void virtio_gpu_flip_worker(void) {
    struct thread *self = sched_get_current();
    wait_queue_entry_t entry = {.thread = self};
    for (;;) {
        /* Publish the wait atomically with respect to local preemption.
         * Finish ready-path cleanup before making the CPU preemptible again. */
        hal_irq_state_t irq_flags = hal_irq_save();
        self->wakeup_ticks = 0;
        self->state = THREAD_BLOCKED;
        wait_queue_add(&gpu_flip_wait, &entry);
        if (__atomic_load_n(&gpu_async_flip_state, __ATOMIC_ACQUIRE) != 1) {
            hal_irq_restore(irq_flags);
            sched_yield();
            irq_flags = hal_irq_save();
        }
        wait_queue_remove(&gpu_flip_wait, &entry);
        self->state = THREAD_RUNNING;
        hal_irq_restore(irq_flags);
        if (__atomic_load_n(&gpu_async_flip_state, __ATOMIC_ACQUIRE) != 1)
            continue;
        __atomic_store_n(&gpu_async_flip_state, 2, __ATOMIC_RELEASE);
        struct drm_framebuffer *framebuffer = &gpu_async_flip.framebuffer;
        struct drm_gem_object *object = framebuffer->gem_obj;
        struct vgpu_scanout_source source = {
            .pixels = object->virt_addr, .size = object->size,
            .offset = framebuffer->backing_offset, .width = framebuffer->width,
            .height = framebuffer->height, .pitch = framebuffer->pitch,
            .view_width = framebuffer->width, .view_height = framebuffer->height,
        };
        ascentdrm_console_takeover();
        gpu_scanout_acquire();
        struct vgpu_scanout_image *image = vgpu_scanout_prepare(
            &scanout_heads[gpu_async_flip.head], &scanout_ops, gpu_async_flip.head,
            framebuffer->width, framebuffer->height);
        bool published = false;
        if (image && !ascentdrm_gem_begin_cpu(object)) {
            bool copied = vgpu_scanout_copy(image, &source);
            ascentdrm_gem_end_cpu(object);
            if (copied)
                published = vgpu_scanout_publish(&scanout_heads[gpu_async_flip.head],
                    &scanout_ops, gpu_async_flip.head, image);
        }
        gpu_scanout_release();
        if (published && gpu_async_flip.event) {
            vblank_wait_next();
            struct drm_event_vblank event = {0};
            event.base.type = DRM_EVENT_FLIP_COMPLETE;
            event.base.length = sizeof(event);
            event.user_data = gpu_async_flip.cookie;
            event.sequence = vblank_sequence();
            vblank_timestamp(event.sequence, &event.tv_sec, &event.tv_usec);
            event.crtc_id = gpu_async_flip.crtc;
            ascentdrm_file_send_event(gpu_async_flip.file, &event, gpu_async_flip.node);
        }
        if (!published)
            klog_puts("[FAIL] VirtIO asynchronous flip publication failed\n");
        if (ascentdrm_gem_put_last(object))
            ascentdrm_gem_object_free(gpu_async_flip.file->dev, object);
        vfs_close(gpu_async_flip.node);
        __atomic_store_n(&gpu_async_flip_state, 0, __ATOMIC_RELEASE);
    }
}

static uint32_t gpu_append_dec(char *buf, uint32_t pos, uint32_t cap, uint32_t v) {
    char t[12];
    uint32_t n = 0;
    if (!v)
        t[n++] = 48;
    while (v) {
        t[n++] = (char)(48 + v % 10);
        v /= 10;
    }
    while (n && pos + 1 < cap)
        buf[pos++] = t[--n];
    return pos;
}
uint32_t virtio_gpu_scanout_count(void) {
    return gpu.initialized ? gpu.num_scanouts : 0;
}
bool virtio_gpu_scanout_summary(uint32_t scanout, bool *enabled, char *modes, uint32_t capacity) {
    if (scanout >= VIRTIO_GPU_MAX_SCANOUTS || !enabled)
        return false;
    struct virtio_gpu_scanout_state *s = &gpu.scanouts[scanout];
    *enabled = s->enabled;
    if (!modes || !capacity)
        return true;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < s->mode_count; i++) {
        pos = gpu_append_dec(modes, pos, capacity, s->modes[i].width);
        if (pos + 1 < capacity)
            modes[pos++] = 120;
        pos = gpu_append_dec(modes, pos, capacity, s->modes[i].height);
        if (pos + 1 < capacity)
            modes[pos++] = 10;
    }
    modes[pos < capacity ? pos : capacity - 1] = 0;
    return true;
}
static char *gpu_dec(char *p, uint32_t v) {
    char t[12];
    uint32_t n = 0;
    if (!v)
        t[n++] = 48;
    while (v) {
        t[n++] = (char)(48 + v % 10);
        v /= 10;
    }
    while (n)
        *p++ = t[--n];
    return p;
}
static void virtio_gpu_get_modes(uint32_t connector, struct drm_mode_modeinfo *m, uint32_t *count) {
    if (!count)
        return;
    uint32_t scanout = ascentdrm_connector_scanout_id(connector);
    if (scanout >= VIRTIO_GPU_MAX_SCANOUTS) {
        *count = 0;
        return;
    }
    struct virtio_gpu_scanout_state *s = &gpu.scanouts[scanout];
    uint32_t cap = *count, total = s->enabled ? s->mode_count : 0;
    if (!m) {
        *count = total;
        return;
    }
    uint32_t fill = cap < total ? cap : total;
    for (uint32_t i = 0; i < fill; i++) {
        struct virtio_gpu_mode *m0 = &s->modes[i];
        struct drm_mode_modeinfo *o = &m[i];
        memset(o, 0, sizeof(*o));
        o->hdisplay = m0->width;
        o->hsync_start = o->hdisplay + 8;
        o->hsync_end = o->hdisplay + 16;
        o->htotal = o->hdisplay + 32;
        o->vdisplay = m0->height;
        o->vsync_start = o->vdisplay + 4;
        o->vsync_end = o->vdisplay + 8;
        o->vtotal = o->vdisplay + 12;
        o->vrefresh = m0->refresh;
        o->clock = (uint32_t)(((uint64_t)o->htotal * o->vtotal * o->vrefresh) / 1000ULL);
        o->type = 0x40 | (m0->preferred ? 8 : 0);
        char *p = gpu_dec(o->name, m0->width);
        *p++ = 120;
        p = gpu_dec(p, m0->height);
        *p = 0;
    }
    *count = total;
}
bool virtio_gpu_phase4_bind_drm(void) {
    if (!gpu.initialized)
        return false;
    ascentdrm_register_scanout_backend(virtio_gpu_create_dumb,
                                 virtio_gpu_commit_damage,
                                 virtio_gpu_get_modes,
                                 virtio_gpu_queue_flip);
    ascentdrm_register_async_flip(virtio_gpu_submit_flip);
    klog_puts("[VIRTIO-GPU] Phase 4 DRM/GEM backend bound; GOP retained until first KMS commit\n");
    return true;
}
bool virtio_gpu_phase4_stress_test(uint32_t cycles, uint32_t damages) {
    if (!gpu.initialized || !cycles || !damages)
        return false;
    for (uint32_t i = 0; i < cycles; i++) {
        struct drm_gem_object *obj = NULL;
        if (virtio_gpu_create_dumb(&global_ascentdrm_dev, 64, 64, 32, &obj))
            return false;
        ascentdrm_gem_object_free(&global_ascentdrm_dev, obj);
    }
    struct drm_gem_object *obj = NULL;
    if (virtio_gpu_create_dumb(&global_ascentdrm_dev, 128, 128, 32, &obj))
        return false;
    struct vgpu_scanout_source source = {
        .pixels = obj->virt_addr, .size = obj->size, .width = 128, .height = 128,
        .pitch = 512, .view_width = 128, .view_height = 128,
    };
    bool ok = true;
    gpu_scanout_acquire();
    for (uint32_t i = 0; i < damages && ok; i++) {
        memset(obj->virt_addr, (int)(i & 255), 128 * 128 * 4);
        struct vgpu_scanout_image *image = vgpu_scanout_prepare(&scanout_heads[0],
            &scanout_ops, 0, 128, 128);
        ok = image && vgpu_scanout_copy(image, &source) &&
             vgpu_scanout_publish(&scanout_heads[0], &scanout_ops, 0, image);
    }
    ok = vgpu_scanout_detach(&scanout_heads[0], &scanout_ops, 0) && ok;
    gpu_scanout_release();
    ascentdrm_gem_object_free(&global_ascentdrm_dev, obj);
    klog_puts(ok ? "[VIRTIO-GPU] PASS: source GEM lifetimes, immutable full-frame publication and teardown\n"
                : "[VIRTIO-GPU] FAIL: scanout engine stress\n");
    return ok;
}

bool virtio_gpu_is_initialized(void) {
    return gpu.initialized;
}
const struct pci_device *virtio_gpu_pci_device(void) {
    return gpu.initialized ? gpu.transport.pci : NULL;
}
void virtio_gpu_get_stats(struct virtio_gpu_stats *out) {
    if (out) {
        *out = gpu.stats;
        out->interrupt_mode = (uint64_t)gpu_irq_mode;
        out->frames_in_flight = gpu.command_busy ? 1 : 0;
        out->cursor_runtime_ms =
            cursor_phase7_started_ms ? lapic_timer_get_ms() - cursor_phase7_started_ms : 0;
    }
}
