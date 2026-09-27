#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "../../../lock/lockdiag.h"
#include "apic/lapic_timer.h"
#include "cpu/irq.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/gpu/drm/drm.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "socket/af_netlink.h"
#include "fs/sysfs.h"
#include <stdint.h>
#define GPU_CONTROLQ   0
#define GPU_CURSORQ    1
#define GPU_TIMEOUT_MS 1000
#define REQ_OFF        0
#define REQ_CAP        1024
#define RESP_OFF       2048
#define RESP_CAP       (PAGE_SIZE - RESP_OFF)
static struct virtio_gpu_device gpu;
static spinlock_t gpu_poll_lock;
static spinlock_t gpu_present_lock;
static wait_queue_t gpu_worker_wait;
static wait_queue_t gpu_gem_drain_wait;
static bool gpu_worker_started;
static bool gpu_irq_installed;
enum gpu_irq_mode { GPU_IRQ_POLL, GPU_IRQ_INTX, GPU_IRQ_MSIX };
static enum gpu_irq_mode gpu_irq_mode;
static uint8_t gpu_msix_vectors[3] = {0xFF, 0xFF, 0xFF};
static volatile uint32_t gpu_pending_work;
#define GPU_WORK_CONTROL (1U << 0)
#define GPU_WORK_CURSOR  (1U << 1)
#define GPU_WORK_CONFIG  (1U << 2)
static bool gpu_phase5_reported;
static bool gpu_phase8_started, gpu_phase8_reported;
static uint64_t gpu_phase8_started_ms;
static bool handle_async_completion(void *cookie, uint32_t used_len);
static bool disable_scanout(uint32_t head);
static void virtio_gpu_gem_free(struct drm_device *dev, struct drm_gem_object *obj);
static bool response_valid(uint32_t t) {
    return (t >= VIRTIO_GPU_RESP_OK_NODATA && t <= VIRTIO_GPU_RESP_OK_MAP_INFO) ||
           (t >= VIRTIO_GPU_RESP_ERR_UNSPEC && t <= VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
}
static bool gpu_command_locked(
    const void *req, uint32_t req_len, void *resp, uint32_t resp_len, uint32_t *resp_type) {
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
    gpu.stats.commands_submitted++;

    uint64_t start = lapic_timer_get_ms();
    for (;;) {
        void *cookie = NULL;
        uint32_t used_len = 0;
        if (virtq_poll_complete(&gpu.controlq, &cookie, &used_len)) {
            if (handle_async_completion(cookie, used_len))
                continue;
            if ((uintptr_t)cookie != (uintptr_t)seq ||
                used_len < sizeof(struct virtio_gpu_ctrl_hdr) || used_len > resp_len) {
                gpu.stats.invalid_responses++;
                gpu.command_busy = false;
                return false;
            }
            struct virtio_gpu_ctrl_hdr *h = (struct virtio_gpu_ctrl_hdr *)(base + RESP_OFF);
            if (!response_valid(h->type)) {
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
            gpu.stats.commands_completed++;
            gpu.command_busy = false;
            return true;
        }
        if (lapic_timer_get_ms() - start >= GPU_TIMEOUT_MS) {
            gpu.stats.commands_timed_out++;
            gpu.command_busy = false;
            return false;
        }
        __asm__ volatile("pause");
    }
}
static bool
gpu_command(const void *req, uint32_t req_len, void *resp, uint32_t resp_len, uint32_t *resp_type) {
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    bool ok = gpu_command_locked(req, req_len, resp, resp_len, resp_type);
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    return ok;
}
static struct pci_device *find_gpu(void) {
    for (uint32_t i = 0; i < pci_get_device_count(); i++) {
        struct pci_device *p = pci_get_device(i);
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
    wait_queue_init(&gpu_gem_drain_wait);
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
    uint32_t id = ++gpu.next_resource_id;
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
        uint32_t id = ++gpu.next_resource_id;
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
struct virtio_gpu_gem {
    uint32_t resource_id, width, height, pitch, inflight;
    bool attached;
};

#define PRESENT_SLOT_SIZE 128U
#define PRESENT_RESP_OFF  64U
static bool present_batch(uint32_t head,
                          uint32_t id,
                          const struct virtio_gpu_rect *rect,
                          uint64_t offset,
                          bool change_scanout,
                          const struct virtio_gpu_rect *scan_rect) {
    if (!gpu.initialized || !gpu.present_dma || !rect)
        return false;
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    if (gpu.command_busy) {
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
        return false;
    }
    uint32_t count = change_scanout ? 3U : 2U;
    uint8_t *base = (uint8_t *)gpu.present_dma->virt;
    memset(base, 0, count * PRESENT_SLOT_SIZE);
    uint32_t lens[3] = {0};
    uint64_t fences[3] = {0};
    struct virtio_gpu_transfer_to_host_2d *transfer = (void *)(base + 0 * PRESENT_SLOT_SIZE);
    init_hdr(&transfer->hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    transfer->r = *rect;
    transfer->offset = offset;
    transfer->resource_id = id;
    lens[0] = sizeof(*transfer);
    uint32_t flush_slot = 1;
    if (change_scanout) {
        struct virtio_gpu_set_scanout *scanout = (void *)(base + 1 * PRESENT_SLOT_SIZE);
        init_hdr(&scanout->hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
        scanout->r = *scan_rect;
        scanout->scanout_id = head;
        scanout->resource_id = id;
        lens[1] = sizeof(*scanout);
        flush_slot = 2;
    }
    struct virtio_gpu_resource_flush *flush = (void *)(base + flush_slot * PRESENT_SLOT_SIZE);
    init_hdr(&flush->hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    flush->r = *rect;
    flush->resource_id = id;
    lens[flush_slot] = sizeof(*flush);
    gpu.command_busy = true;
    uint32_t submitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct virtio_gpu_ctrl_hdr *h = (void *)(base + i * PRESENT_SLOT_SIZE);
        fences[i] = __atomic_add_fetch(&gpu.next_fence_id, 1, __ATOMIC_RELAXED);
        h->flags |= VIRTIO_GPU_FLAG_FENCE;
        h->fence_id = fences[i];
        struct virtq_iov out = {gpu.present_dma->phys + i * PRESENT_SLOT_SIZE, lens[i]};
        struct virtq_iov in = {gpu.present_dma->phys + i * PRESENT_SLOT_SIZE + PRESENT_RESP_OFF,
                               sizeof(struct virtio_gpu_ctrl_hdr)};
        if (virtq_submit_deferred(&gpu.controlq, &out, 1, &in, 1, (void *)(uintptr_t)(i + 1)) < 0)
            break;
        submitted++;
        gpu.stats.commands_submitted++;
    }
    if (submitted)
        virtq_kick(&gpu.controlq);
    uint64_t start = lapic_timer_get_ms();
    uint32_t completed = 0;
    bool ok = submitted == count;
    bool done[3] = {false, false, false};
    while (completed < submitted) {
        void *cookie = NULL;
        uint32_t used = 0;
        if (virtq_poll_complete(&gpu.controlq, &cookie, &used)) {
            if (handle_async_completion(cookie, used))
                continue;
            uintptr_t tag = (uintptr_t)cookie;
            if (tag < 1 || tag > count || done[tag - 1] ||
                used < sizeof(struct virtio_gpu_ctrl_hdr)) {
                gpu.stats.invalid_responses++;
                ok = false;
                continue;
            }
            uint32_t slot = (uint32_t)tag - 1;
            done[slot] = true;
            completed++;
            gpu.stats.commands_completed++;
            struct virtio_gpu_ctrl_hdr *resp =
                (void *)(base + slot * PRESENT_SLOT_SIZE + PRESENT_RESP_OFF);
            if (resp->type != VIRTIO_GPU_RESP_OK_NODATA || resp->fence_id != fences[slot]) {
                if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
                    gpu.stats.host_errors++;
                else
                    gpu.stats.invalid_responses++;
                ok = false;
            }
            continue;
        }
        if (lapic_timer_get_ms() - start >= GPU_TIMEOUT_MS) {
            gpu.stats.commands_timed_out++;
            ok = false;
            break;
        }
        __asm__ volatile("pause");
    }
    uint64_t elapsed = lapic_timer_get_ms() - start;
    gpu.stats.present_wait_ms += elapsed;
    if (elapsed > gpu.stats.max_present_wait_ms)
        gpu.stats.max_present_wait_ms = elapsed;
    gpu.stats.present_batches++;
    if (ok) {
        gpu.stats.frames_presented++;
        gpu.stats.damage_pixels += (uint64_t)rect->width * rect->height;
    } else
        gpu.stats.present_failures++;
    gpu.command_busy = false;
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
    return ok;
}

#define GPU_ASYNC_SLOTS 8U
struct gpu_async_cookie {
    uint8_t slot, command;
};
struct gpu_async_slot {
    dma_buffer_t *dma;
    struct gpu_async_cookie cookies[3];
    uint64_t fences[3], submitted_ms;
    uint32_t command_count, completed, resource_id, scanout_id;
    bool in_use, ok, change_scanout, has_event;
    struct drm_file *event_file;
    struct vfs_node *event_node;
    uint64_t event_user_data;
    uint32_t event_crtc_id;
    struct virtio_gpu_gem *owner;
};
static struct gpu_async_slot async_slots[GPU_ASYNC_SLOTS];
static uint32_t async_in_flight;

static bool handle_async_completion(void *cookie, uint32_t used_len) {
    struct gpu_async_slot *slot = NULL;
    uint32_t command = 0;
    for (uint32_t i = 0; i < GPU_ASYNC_SLOTS && !slot; i++)
        for (uint32_t c = 0; c < 3; c++)
            if (cookie == &async_slots[i].cookies[c]) {
                slot = &async_slots[i];
                command = c;
                break;
            }
    if (!slot)
        return false;
    struct drm_file *event_file = NULL;
    struct vfs_node *event_node = NULL;
    struct drm_event_vblank ev;
    bool send_event = false;
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
    if (!slot->in_use || command >= slot->command_count) {
        gpu.stats.invalid_responses++;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
        return true;
    }
    struct virtio_gpu_ctrl_hdr *resp =
        (void *)((uint8_t *)slot->dma->virt + command * PRESENT_SLOT_SIZE + PRESENT_RESP_OFF);
    if (used_len < sizeof(*resp) || resp->type != VIRTIO_GPU_RESP_OK_NODATA ||
        resp->fence_id != slot->fences[command]) {
        slot->ok = false;
        if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
            gpu.stats.host_errors++;
        else
            gpu.stats.invalid_responses++;
    }
    slot->completed++;
    gpu.stats.commands_completed++;
    if (slot->completed == slot->command_count) {
        uint64_t elapsed = lapic_timer_get_ms() - slot->submitted_ms;
        gpu.stats.present_wait_ms += elapsed;
        if (elapsed > gpu.stats.max_present_wait_ms)
            gpu.stats.max_present_wait_ms = elapsed;
        if (slot->ok) {
            gpu.stats.frames_presented++;
            gpu.stats.async_completed++;
            if (slot->change_scanout) {
                gpu.head_resource[slot->scanout_id] = slot->resource_id;
                if (slot->scanout_id == 0)
                    gpu.scanout_resource_id = slot->resource_id;
                gpu.owns_scanout = true;
            }
        } else
            gpu.stats.present_failures++;
        if (slot->has_event) {
            memset(&ev, 0, sizeof(ev));
            uint64_t ms = lapic_timer_get_ms();
            ev.base.type = DRM_EVENT_FLIP_COMPLETE;
            ev.base.length = sizeof(ev);
            ev.user_data = slot->event_user_data;
            ev.tv_sec = (uint32_t)(ms / 1000);
            ev.tv_usec = (uint32_t)((ms % 1000) * 1000);
            ev.sequence = ++gpu.flip_sequence;
            ev.crtc_id = slot->event_crtc_id;
            event_file = slot->event_file;
            event_node = slot->event_node;
            send_event = true;
        }
        if (slot->owner && __atomic_load_n(&slot->owner->inflight, __ATOMIC_ACQUIRE)) {
            if (__atomic_sub_fetch(&slot->owner->inflight, 1, __ATOMIC_RELEASE) == 0)
                wait_queue_wake_all(&gpu_gem_drain_wait);
        }
        slot->owner = NULL;
        slot->in_use = false;
        slot->has_event = false;
        if (async_in_flight)
            async_in_flight--;
        if (!gpu_phase5_reported &&
            gpu.stats.async_completed + gpu.stats.present_failures >= 10000) {
            gpu_phase5_reported = true;
            bool pass = gpu.stats.present_failures == 0 && gpu.stats.async_dropped == 0 &&
                        gpu.stats.async_submitted == gpu.stats.async_completed + async_in_flight &&
                        (!gpu_irq_installed || gpu.stats.interrupts > 0);
            klog_puts(pass ? "[VIRTIO-GPU] PASS: Phase 5 10000 async frames, fenced completion, "
                             "IRQ wakeups\n"
                           : "[VIRTIO-GPU] FAIL: Phase 5 runtime stress counters\n");
        }
    }
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
    if (send_event)
        drm_file_send_event(event_file, &ev, event_node);
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
static struct drm_gem_object *cursor_last_gem;
static uint32_t cursor_last_width, cursor_last_height, cursor_last_pitch;
static int32_t cursor_last_hot_x, cursor_last_hot_y;

static uint32_t cursor_coord(int32_t value, uint32_t limit) {
    if (value < 0)
        return 0;
    uint32_t v = (uint32_t)value;
    return v > limit ? limit : v;
}

static bool cursor_submit_locked(
    uint32_t type, uint32_t resource_id, int32_t x, int32_t y, int32_t hot_x, int32_t hot_y) {
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
    cmd->pos.scanout_id = cursor_active_head;
    uint32_t cw = gpu.display_info.pmodes[cursor_active_head].r.width,
             ch = gpu.display_info.pmodes[cursor_active_head].r.height;
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
    bool ok = cursor_submit_locked(type, resource_id, x, y, hot_x, hot_y);
    if (!ok && type == VIRTIO_GPU_CMD_MOVE_CURSOR) {
        cursor_pending_move = true;
        cursor_pending_x = x;
        cursor_pending_y = y;
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
            if (!cursor_submit_locked(VIRTIO_GPU_CMD_MOVE_CURSOR,
                                      cursor_visible ? cursor_resource_id : 0,
                                      x,
                                      y,
                                      cursor_last_hot_x,
                                      cursor_last_hot_y)) {
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
    for (;;) {
        void *cookie = NULL;
        uint32_t used = 0;
        if (!virtq_poll_complete(&gpu.controlq, &cookie, &used))
            break;
        if (!handle_async_completion(cookie, used))
            gpu.stats.invalid_responses++;
    }
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_POLL, &gpu_poll_lock);
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
    drm_ensure_outputs(&global_drm_dev, gpu.num_scanouts);
    for (uint32_t i = 0; i < gpu.num_scanouts && i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        bool changed = old_enabled[i] != gpu.scanouts[i].enabled ||
                       old_w[i] != gpu.display_info.pmodes[i].r.width ||
                       old_h[i] != gpu.display_info.pmodes[i].r.height;
        drm_update_output_state(&global_drm_dev, i, gpu.scanouts[i].enabled);
        if (old_enabled[i] && !gpu.scanouts[i].enabled && gpu.head_resource[i]) {
            if (disable_scanout(i)) {
                gpu.head_resource[i] = 0;
                gpu.queued_head_resource[i] = 0;
                if (i == 0) {
                    gpu.scanout_resource_id = 0;
                    gpu.queued_scanout_resource_id = 0;
                }
            }
        }
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
    wait_queue_entry_t entry = {.thread = self, .next = NULL};
    for (;;) {
        uint32_t pending = __atomic_exchange_n(&gpu_pending_work, 0, __ATOMIC_ACQ_REL);
        /* Poll periodically only when interrupts are unavailable.  In IRQ mode,
         * waking this worker every millisecond wastes a substantial share of a
         * QEMU vCPU and competes directly with Xorg and the renderer. */
        if (!pending && !gpu_irq_installed)
            gpu.stats.watchdog_polls++;
        if (pending & GPU_WORK_CONFIG)
            gpu_handle_config_event();
        gpu_drain_completions();
        gpu_drain_cursor_completions();
        wait_queue_add(&gpu_worker_wait, &entry);
        if (gpu.controlq.last_used_idx == gpu.controlq.used->idx &&
            gpu.cursorq.last_used_idx == gpu.cursorq.used->idx && !gpu_pending_work) {
            self->state = THREAD_BLOCKED;
            self->wakeup_ticks = gpu_irq_installed ? 0 : lapic_timer_get_ticks() + 1;
        } else
            wait_queue_wake_one(&gpu_worker_wait);
        sched_yield();
        self->wakeup_ticks = 0;
        wait_queue_remove(&gpu_worker_wait, &entry);
    }
}

static bool async_present_submit(uint32_t head,
                                 struct virtio_gpu_gem *owner,
                                 const struct virtio_gpu_rect *rect,
                                 uint64_t offset,
                                 bool change,
                                 const struct virtio_gpu_rect *scan_rect) {
    if (!gpu_worker_started || !owner || !rect)
        return false;
    uint32_t id = owner->resource_id;
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
    uint32_t index = GPU_ASYNC_SLOTS;
    for (uint32_t i = 0; i < GPU_ASYNC_SLOTS; i++)
        if (!async_slots[i].in_use) {
            index = i;
            break;
        }
    if (index == GPU_ASYNC_SLOTS) {
        gpu.stats.async_dropped++;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
        return false;
    }
    struct gpu_async_slot *slot = &async_slots[index];
    uint32_t count = change ? 3U : 2U;
    memset(slot->dma->virt, 0, count * PRESENT_SLOT_SIZE);
    slot->in_use = true;
    slot->ok = true;
    slot->change_scanout = change;
    slot->resource_id = id;
    slot->scanout_id = head;
    slot->owner = owner;
    __atomic_add_fetch(&owner->inflight, 1, __ATOMIC_RELEASE);
    slot->command_count = count;
    slot->completed = 0;
    slot->submitted_ms = lapic_timer_get_ms();
    slot->has_event = gpu.pending_flip && drm_crtc_scanout_id(gpu.pending_flip_crtc_id) == head;
    if (slot->has_event) {
        slot->event_file = gpu.pending_flip_file;
        slot->event_node = gpu.pending_flip_node;
        slot->event_user_data = gpu.pending_flip_user_data;
        slot->event_crtc_id = gpu.pending_flip_crtc_id;
        gpu.pending_flip = false;
        gpu.pending_flip_file = NULL;
        gpu.pending_flip_node = NULL;
    }
    uint8_t *base = (uint8_t *)slot->dma->virt;
    uint32_t lens[3] = {0};
    struct virtio_gpu_transfer_to_host_2d *transfer = (void *)base;
    init_hdr(&transfer->hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    transfer->r = *rect;
    transfer->offset = offset;
    transfer->resource_id = id;
    lens[0] = sizeof(*transfer);
    uint32_t flush_slot = 1;
    if (change) {
        struct virtio_gpu_set_scanout *scanout = (void *)(base + PRESENT_SLOT_SIZE);
        init_hdr(&scanout->hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
        scanout->r = *scan_rect;
        scanout->scanout_id = head;
        scanout->resource_id = id;
        lens[1] = sizeof(*scanout);
        flush_slot = 2;
    }
    struct virtio_gpu_resource_flush *flush = (void *)(base + flush_slot * PRESENT_SLOT_SIZE);
    init_hdr(&flush->hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    flush->r = *rect;
    flush->resource_id = id;
    lens[flush_slot] = sizeof(*flush);
    uint32_t submitted = 0;
    for (uint32_t c = 0; c < count; c++) {
        struct virtio_gpu_ctrl_hdr *h = (void *)(base + c * PRESENT_SLOT_SIZE);
        slot->fences[c] = __atomic_add_fetch(&gpu.next_fence_id, 1, __ATOMIC_RELAXED);
        h->flags |= VIRTIO_GPU_FLAG_FENCE;
        h->fence_id = slot->fences[c];
        slot->cookies[c].slot = (uint8_t)index;
        slot->cookies[c].command = (uint8_t)c;
        struct virtq_iov out = {slot->dma->phys + c * PRESENT_SLOT_SIZE, lens[c]};
        struct virtq_iov in = {slot->dma->phys + c * PRESENT_SLOT_SIZE + PRESENT_RESP_OFF,
                               sizeof(struct virtio_gpu_ctrl_hdr)};
        if (virtq_submit_deferred(&gpu.controlq, &out, 1, &in, 1, &slot->cookies[c]) < 0) {
            slot->ok = false;
            break;
        }
        submitted++;
        gpu.stats.commands_submitted++;
    }
    slot->command_count = submitted;
    if (!submitted) {
        if (slot->owner && __atomic_load_n(&slot->owner->inflight, __ATOMIC_ACQUIRE))
            __atomic_sub_fetch(&slot->owner->inflight, 1, __ATOMIC_RELEASE);
        slot->owner = NULL;
        slot->in_use = false;
        if (slot->has_event) {
            gpu.pending_flip = true;
            gpu.pending_flip_file = slot->event_file;
            gpu.pending_flip_node = slot->event_node;
            gpu.pending_flip_user_data = slot->event_user_data;
            gpu.pending_flip_crtc_id = slot->event_crtc_id;
        }
        slot->has_event = false;
        gpu.stats.async_dropped++;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
        return false;
    }
    async_in_flight++;
    if (async_in_flight > gpu.stats.max_frames_in_flight)
        gpu.stats.max_frames_in_flight = async_in_flight;
    gpu.stats.present_batches++;
    gpu.stats.async_submitted++;
    gpu.stats.damage_pixels += (uint64_t)rect->width * rect->height;
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_PRESENT, &gpu_present_lock);
    virtq_kick(&gpu.controlq);
    return true;
}

bool virtio_gpu_phase6_start(void) {
    for (uint32_t i = 0; i < GPU_ASYNC_SLOTS; i++) {
        async_slots[i].dma = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT);
        if (!async_slots[i].dma)
            return false;
    }
    virtio_gpu_setup_irq();
    struct thread *worker = sched_create_kernel_thread(virtio_gpu_worker, NULL, true);
    if (!worker)
        return false;
    strcpy(worker->comm, "virtio-gpu");
    gpu_worker_started = true;
    if (gpu_irq_mode == GPU_IRQ_MSIX)
        klog_puts("[VIRTIO-GPU] Phase 6 async mode: MSI-X control/cursor/config vectors, 8 frame "
                  "slots\n");
    else if (gpu_irq_mode == GPU_IRQ_INTX)
        klog_puts("[VIRTIO-GPU] Phase 6 async mode: shared INTx/ISR fallback, 8 frame slots\n");
    else
        klog_puts("[VIRTIO-GPU] Phase 6 async mode: bounded polling fallback, 8 frame slots\n");
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
    cursor_active_head = drm_crtc_scanout_id(crtc_id);
    if (cursor_active_head >= VIRTIO_GPU_MAX_SCANOUTS)
        cursor_active_head = 0;
    LOCKDIAG_SPOT_LOCK(LOCKDIAG_SPOT_GPU_SHAPE, &gpu_cursor_shape_lock);
    bool shape = (flags & DRM_MODE_CURSOR_BO) || gem != cursor_last_gem ||
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
    if (pitch < width * 4U) {
        gpu.stats.cursor_failures++;
        LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_SHAPE, &gpu_cursor_shape_lock);
        return;
    }
    bool ok = true;
    if (shape) {
        memset(cursor_shape_dma->virt, 0, GPU_CURSOR_IMAGE_BYTES);
        for (uint32_t row = 0; row < height; row++)
            memcpy((uint8_t *)cursor_shape_dma->virt + row * 64U * 4U,
                   (uint8_t *)gem->virt_addr + row * pitch,
                   width * 4U);
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
        cursor_last_hot_x = hot_x;
        cursor_last_hot_y = hot_y;
    }
    LOCKDIAG_SPOT_UNLOCK(LOCKDIAG_SPOT_GPU_SHAPE, &gpu_cursor_shape_lock);
}

bool virtio_gpu_phase7_start(void) {
    if (!gpu.initialized || !gpu_worker_started)
        return false;
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
    cursor_resource_id = ++gpu.next_resource_id;
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
    drm_register_cursor_backend(virtio_gpu_cursor_update);
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
    drm_ensure_outputs(&global_drm_dev, gpu.num_scanouts);
    for (uint32_t i = 0; i < gpu.num_scanouts && i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        drm_update_output_state(&global_drm_dev, i, gpu.scanouts[i].enabled);
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
static void virtio_gpu_gem_free(struct drm_device *dev, struct drm_gem_object *obj) {
    (void)dev;
    struct virtio_gpu_gem *vg = (struct virtio_gpu_gem *)obj->driver_private;
    if (vg) {
        if (gpu_irq_installed && gpu_worker_started && sched_get_current()) {
            struct thread *self = sched_get_current();
            wait_queue_entry_t entry = {.thread = self, .next = NULL};
            uint64_t deadline = lapic_timer_get_ticks() + (GPU_TIMEOUT_MS / 10 > 0 ? GPU_TIMEOUT_MS / 10 : 100);
            while (__atomic_load_n(&vg->inflight, __ATOMIC_ACQUIRE) > 0) {
                if (lapic_timer_get_ticks() >= deadline)
                    break;
                wait_queue_add(&gpu_gem_drain_wait, &entry);
                self->state = THREAD_BLOCKED;
                self->wakeup_ticks = deadline;
                sched_yield();
                self->wakeup_ticks = 0;
                wait_queue_remove(&gpu_gem_drain_wait, &entry);
            }
        } else {
            for (uint32_t tries = 0;
                 __atomic_load_n(&vg->inflight, __ATOMIC_ACQUIRE) && tries < GPU_TIMEOUT_MS;
                 tries++) {
                gpu_drain_completions();
                sched_yield();
            }
        }
        if (__atomic_load_n(&vg->inflight, __ATOMIC_ACQUIRE)) {
            klog_puts("[VIRTIO-GPU] WARN: quarantining GEM with in-flight frames\n");
            return;
        }
        for (uint32_t head = 0; head < VIRTIO_GPU_MAX_SCANOUTS; head++)
            if (gpu.head_resource[head] == vg->resource_id) {
                if (!disable_scanout(head)) {
                    klog_puts("[VIRTIO-GPU] WARN: quarantining active GEM after scanout detach "
                              "failure\n");
                    return;
                }
                gpu.head_resource[head] = 0;
                gpu.queued_head_resource[head] = 0;
                if (head == 0) {
                    gpu.scanout_resource_id = 0;
                    gpu.queued_scanout_resource_id = 0;
                }
            }
        if (vg->attached && !detach_backing(vg->resource_id)) {
            klog_puts("[VIRTIO-GPU] WARN: quarantining GEM after backing detach failure\n");
            return;
        }
        if (!unref_resource(vg->resource_id)) {
            klog_puts("[VIRTIO-GPU] WARN: quarantining GEM after unref failure\n");
            return;
        }
        kfree(vg);
    }
    pmm_free_blocks((void *)obj->phys_addr, obj->size / PAGE_SIZE);
    kfree(obj);
}
static int virtio_gpu_create_dumb(struct drm_device *dev,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t bpp,
                                  struct drm_gem_object **out) {
    if (!gpu.initialized || !dev || !out || bpp != 32 || !width || !height || width > 8192 ||
        height > 8192)
        return -22;
    uint64_t pitch = (uint64_t)width * 4ULL, bytes = pitch * height;
    if (bytes > 0xFFFFFFFFULL)
        return -22;
    struct drm_gem_object *obj = drm_gem_object_create(dev, (size_t)bytes);
    if (!obj)
        return -12;
    struct virtio_gpu_gem *vg = kmalloc(sizeof(*vg));
    if (!vg) {
        drm_gem_object_free(dev, obj);
        return -12;
    }
    memset(vg, 0, sizeof(*vg));
    vg->resource_id = ++gpu.next_resource_id;
    vg->width = width;
    vg->height = height;
    vg->pitch = (uint32_t)pitch;
    bool created = create_2d(vg->resource_id, width, height);
    if (!created || !attach_backing(vg->resource_id, obj->phys_addr, (uint32_t)bytes)) {
        if (created)
            unref_resource(vg->resource_id);
        kfree(vg);
        drm_gem_object_free(dev, obj);
        return -5;
    }
    vg->attached = true;
    obj->driver_private = vg;
    obj->free = virtio_gpu_gem_free;
    *out = obj;
    return 0;
}
static struct drm_framebuffer *virtio_gpu_active_fb(struct drm_device *dev,
                                                    uint32_t target,
                                                    uint32_t *head,
                                                    struct virtio_gpu_rect *view) {
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
            if (c->fb && (!target || o->id == target || c->fb->base.id == target)) {
                fb = c->fb;
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
    if (!gpu.pending_flip || drm_crtc_scanout_id(gpu.pending_flip_crtc_id) != head) {
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
    struct drm_event_vblank ev;
    memset(&ev, 0, sizeof(ev));
    uint64_t ms = lapic_timer_get_ms();
    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof(ev);
    ev.user_data = user_data;
    ev.tv_sec = (uint32_t)(ms / 1000);
    ev.tv_usec = (uint32_t)((ms % 1000) * 1000);
    ev.sequence = sequence;
    ev.crtc_id = crtc_id;
    drm_file_send_event(file, &ev, node);
}
static void virtio_gpu_commit_damage(struct drm_device *dev,
                                     const struct drm_clip_rect *clips,
                                     uint32_t n,
                                     uint32_t target) {
    uint32_t head = 0;
    struct virtio_gpu_rect view;
    struct drm_framebuffer *fb = virtio_gpu_active_fb(dev, target, &head, &view);
    if (!fb || !fb->gem_obj) {
        virtio_gpu_complete_flip(head);
        return;
    }
    struct virtio_gpu_gem *vg = (struct virtio_gpu_gem *)fb->gem_obj->driver_private;
    if (!vg || !vg->attached) {
        virtio_gpu_complete_flip(head);
        return;
    }
    if (head >= VIRTIO_GPU_MAX_SCANOUTS) {
        virtio_gpu_complete_flip(head);
        return;
    }
    uint32_t queued =
        __atomic_exchange_n(&gpu.queued_head_resource[head], vg->resource_id, __ATOMIC_ACQ_REL);
    if (head == 0)
        gpu.queued_scanout_resource_id = vg->resource_id;
    bool change = queued != vg->resource_id;
    struct virtio_gpu_rect r = {0, 0, vg->width, vg->height};
    if (clips && n) {
        uint32_t x1 = vg->width, y1 = vg->height, x2 = 0, y2 = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ax = clips[i].x1 < vg->width ? clips[i].x1 : vg->width;
            uint32_t ay = clips[i].y1 < vg->height ? clips[i].y1 : vg->height;
            uint32_t bx = clips[i].x2 < vg->width ? clips[i].x2 : vg->width;
            uint32_t by = clips[i].y2 < vg->height ? clips[i].y2 : vg->height;
            if (ax < bx && ay < by) {
                if (ax < x1)
                    x1 = ax;
                if (ay < y1)
                    y1 = ay;
                if (bx > x2)
                    x2 = bx;
                if (by > y2)
                    y2 = by;
            }
        }
        if (x1 >= x2 || y1 >= y2) {
            if (!change) {
                virtio_gpu_complete_flip(head);
                return;
            }
        } else {
            r.x = x1;
            r.y = y1;
            r.width = x2 - x1;
            r.height = y2 - y1;
        }
    }
    /* Selecting a different host resource requires a full upload: the host
     * copy may hold pixels from an older frame.  The damage reported for a
     * buffer switch is not a complete description of its contents either:
     * double-buffered compositors (e.g. Weston's pixman renderer) also redraw
     * the previous frame's damage into the new back buffer but only report
     * the current frame's damage, so a partial transfer would leave stale
     * pixels behind (trails/flicker).  Damage is only complete while the
     * same resource stays selected. */
    if (change) {
        r.x = 0;
        r.y = 0;
        r.width = vg->width;
        r.height = vg->height;
    }
    uint64_t off = change ? 0 : ((uint64_t)r.y * vg->pitch + (uint64_t)r.x * 4ULL);
    if (async_present_submit(head, vg, &r, off, change, &view))
        return;
    if (present_batch(head, vg->resource_id, &r, off, change, &view) && change) {
        gpu.head_resource[head] = vg->resource_id;
        if (head == 0)
            gpu.scanout_resource_id = vg->resource_id;
        gpu.owns_scanout = true;
    }
    virtio_gpu_complete_flip(head);
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
    uint32_t scanout = drm_connector_scanout_id(connector);
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
    drm_register_scanout_backend(virtio_gpu_create_dumb,
                                 virtio_gpu_commit_damage,
                                 virtio_gpu_get_modes,
                                 virtio_gpu_queue_flip);
    klog_puts("[VIRTIO-GPU] Phase 4 DRM/GEM backend bound; GOP retained until first KMS commit\n");
    return true;
}
bool virtio_gpu_phase4_stress_test(uint32_t cycles, uint32_t damages) {
    if (!gpu.initialized || !cycles || !damages)
        return false;
    uint32_t baseline = gpu.resources_live;
    klog_puts("[VIRTIO-GPU] Phase 4 stress: GEM cycles=");
    klog_uint64(cycles);
    klog_puts(" damage transfers=");
    klog_uint64(damages);
    klog_puts("\n");
    for (uint32_t i = 0; i < cycles; i++) {
        struct drm_gem_object *o = NULL;
        if (virtio_gpu_create_dumb(&global_drm_dev, 64, 64, 32, &o) != 0)
            return false;
        memset(o->virt_addr, (int)(i & 255), 64U * 64U * 4U);
        drm_gem_object_free(&global_drm_dev, o);
    }
    struct drm_gem_object *o = NULL;
    if (virtio_gpu_create_dumb(&global_drm_dev, 128, 128, 32, &o) != 0)
        return false;
    struct virtio_gpu_gem *vg = (struct virtio_gpu_gem *)o->driver_private;
    for (uint32_t i = 0; i < damages; i++) {
        struct virtio_gpu_rect r = {(i * 7U) % 120U, (i * 11U) % 120U, 8, 8};
        uint64_t off = (uint64_t)r.y * vg->pitch + (uint64_t)r.x * 4ULL;
        if (!present_batch(0, vg->resource_id, &r, off, false, &r)) {
            drm_gem_object_free(&global_drm_dev, o);
            return false;
        }
    }
    drm_gem_object_free(&global_drm_dev, o);
    struct virtq_stats q;
    virtq_get_stats(&gpu.controlq, &q);
    if (gpu.resources_live != baseline || !virtq_is_idle(&gpu.controlq) ||
        gpu.stats.commands_submitted != gpu.stats.commands_completed ||
        gpu.stats.commands_timed_out || gpu.stats.invalid_responses || q.submitted != q.completed) {
        klog_puts("[VIRTIO-GPU] FAIL: Phase 4 final state mismatch\n");
        return false;
    }
    klog_puts("[VIRTIO-GPU] PASS: DRM GEM lifetime, mmap backing, clipped damage, idle queue\n");
    return true;
}

bool virtio_gpu_is_initialized(void) {
    return gpu.initialized;
}
void virtio_gpu_get_stats(struct virtio_gpu_stats *out) {
    if (out) {
        *out = gpu.stats;
        out->interrupt_mode = (uint64_t)gpu_irq_mode;
        out->frames_in_flight = async_in_flight;
        out->cursor_runtime_ms =
            cursor_phase7_started_ms ? lapic_timer_get_ms() - cursor_phase7_started_ms : 0;
    }
}
