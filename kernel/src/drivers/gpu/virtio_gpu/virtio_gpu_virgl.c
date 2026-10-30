/* Legacy virgl DRM ABI. Render batches retain DMA and BO references until
 * their host fence completes; explicit sync-file exports remain synchronous.
 * Blob resources, Venus and context-init are deliberately not advertised. */
#include "virtio_gpu.h"
#include "drivers/gpu/drm/drm.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "lib/string.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "console/klog.h"
#include <linuxkpi/native_vfs.h>

#define MAX_CAP_SIZE (64U * 1024U - 24U)
#define MAX_SUBMIT (1024U * 1024U)
#define MAX_RESOURCE (256U * 1024U * 1024U)
struct vg_map { uint64_t offset; uint32_t handle, pad; };
struct vg_param { uint64_t param, value; };
struct vg_caps { uint32_t id, version; uint64_t addr; uint32_t size, pad; };
struct vg_create {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};
struct vg_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct vg_transfer {
    uint32_t handle;
    struct virtio_gpu_box box;
    uint32_t level, offset, stride, layer_stride;
};
struct vg_wait { uint32_t handle, flags; };
struct vg_exec {
    uint32_t flags, size;
    uint64_t command, handles;
    uint32_t count;
    int32_t fence_fd;
    uint32_t ring_idx, syncobj_stride, num_in, num_out;
    uint64_t in_syncobjs, out_syncobjs;
};
_Static_assert(sizeof(struct vg_create) == 56, "virtgpu create UAPI");
_Static_assert(sizeof(struct vg_transfer) == 44, "virtgpu transfer UAPI");
_Static_assert(sizeof(struct vg_exec) == 64, "virtgpu exec UAPI");
struct virgl_attachment { uint32_t context; struct virgl_attachment *next; };
struct virgl_resource {
    uint32_t id, width, height, depth, stride, target;
    struct virgl_attachment *attachments;
};
static struct { uint32_t version, size; } caps[3];
static bool enabled;
static uint32_t next_context, ioctl_owner;
extern int linuxkpi_virgl_fence_wait(int fd);
extern int linuxkpi_virgl_fence_export(void);
extern void *linuxkpi_virgl_fence_create(void);
extern void linuxkpi_virgl_fence_get(void *);
extern void linuxkpi_virgl_fence_put(void *);
extern void linuxkpi_virgl_fence_complete(void *, int);
extern int linuxkpi_virgl_fence_export_pending(void *);

static bool user(uint64_t addr, size_t size) {
    return addr && size && vmm_is_user_addr_range_valid(addr, size);
}
static struct virtio_gpu_ctrl_hdr header(uint32_t type, uint32_t ctx) {
    return (struct virtio_gpu_ctrl_hdr){.type = type, .ctx_id = ctx};
}
static int command(const void *req, uint32_t len) {
    struct virtio_gpu_ctrl_hdr reply;
    uint32_t type;
    return virtio_gpu_control(req, len, &reply, sizeof(reply), &type) &&
        type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -5;
}
static void put(struct drm_gem_object *obj) {
    if (ascentdrm_gem_put_last(obj))
        ascentdrm_gem_object_free(obj->dev, obj);
}
/* Rendering handles are file-local. Do not use the legacy global GEM lookup
 * fallback, which would permit attaching another client's resource. */
static struct drm_gem_object *lookup(struct drm_file *file, uint32_t handle) {
    if (!handle || handle >= DRM_MAX_HANDLES_PER_FILE) return NULL;
    spinlock_acquire(&file->lock);
    struct drm_gem_object *obj = file->handles[handle];
    if (obj) ascentdrm_gem_get(obj);
    spinlock_release(&file->lock);
    return obj;
}
static struct virgl_resource *resource(struct drm_gem_object *obj) {
    return obj ? obj->virgl_resource : NULL;
}
uint32_t virtio_gpu_virgl_resource_id(struct drm_gem_object *obj) {
    struct virgl_resource *res = resource(obj);
    return res ? res->id : 0;
}
static int context(struct drm_file *file) {
    if (file->virgl_context) return 0;
    struct virtio_gpu_ctx_create req = {0};
    uint32_t id = __atomic_add_fetch(&next_context, 1, __ATOMIC_RELAXED);
    if (!id) return -12;
    req.hdr = header(VIRTIO_GPU_CMD_CTX_CREATE, id);
    memcpy(req.name, "Ascension", 9);
    req.nlen = 9;
    int ret = command(&req, sizeof(req));
    if (!ret) file->virgl_context = id;
    return ret;
}
static int attach(struct drm_file *file, struct virgl_resource *res) {
    for (struct virgl_attachment *p = res->attachments; p; p = p->next)
        if (p->context == file->virgl_context) return 0;
    struct virgl_attachment *entry = kmalloc(sizeof(*entry));
    if (!entry) return -12;
    struct virtio_gpu_ctx_resource req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, file->virgl_context);
    req.resource_id = res->id;
    int ret = command(&req, sizeof(req));
    if (ret) { kfree(entry); return ret; }
    *entry = (struct virgl_attachment){file->virgl_context, res->attachments};
    res->attachments = entry;
    return 0;
}
static void free_resource(struct drm_device *dev, struct drm_gem_object *obj) {
    (void)dev;
    struct virgl_resource *res = resource(obj);
    struct virtio_gpu_resource_unref req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
    req.resource_id = res->id;
    /* A timeout is not DMA completion. Keep backing pinned on error, since
     * the host may still access it. This deliberately leaks on device failure. */
    if (command(&req, sizeof(req))) return;
    while (res->attachments) {
        struct virgl_attachment *entry = res->attachments;
        res->attachments = entry->next;
        kfree(entry);
    }
    kfree(res);
    ascentdrm_gem_release_storage(obj);
}
void virtio_gpu_virgl_init(uint64_t features, uint32_t count) {
    if (!(features & VIRTIO_GPU_F_VIRGL)) return;
    for (uint32_t i = 0; i < count && i < 64; i++) {
        struct virtio_gpu_get_capset_info req = {0};
        struct virtio_gpu_resp_capset_info reply = {0};
        uint32_t type;
        req.hdr = header(VIRTIO_GPU_CMD_GET_CAPSET_INFO, 0);
        req.index = i;
        if (!virtio_gpu_control(&req, sizeof(req), &reply, sizeof(reply), &type) ||
            type != VIRTIO_GPU_RESP_OK_CAPSET_INFO) continue;
        if ((reply.id == 1 || reply.id == 2) && reply.size && reply.size <= MAX_CAP_SIZE) {
            caps[reply.id].version = reply.version;
            caps[reply.id].size = reply.size;
        }
    }
    enabled = caps[1].size != 0;
    if (enabled) klog_puts(virtio_gpu_render_queued() ?
        "[VIRTIO-GPU] Virgl rendering enabled (preallocated queued host fences)\n" :
        "[VIRTIO-GPU] Virgl rendering enabled (synchronous fallback)\n");
}
bool virtio_gpu_virgl_enabled(void) { return enabled; }
void virtio_gpu_virgl_close(struct drm_file *file) {
    if (!file->virgl_context) return;
    if (virtio_gpu_wait_idle(false)) return; /* Retain a timed-out host context. */
    struct virtio_gpu_ctrl_hdr req = header(VIRTIO_GPU_CMD_CTX_DESTROY, file->virgl_context);
    command(&req, sizeof(req));
    file->virgl_context = 0;
}
static int get_caps(struct vg_caps *p) {
    if (p->pad || p->id < 1 || p->id > 2 || !caps[p->id].size ||
        p->version > caps[p->id].version || !p->size)
        return -22;
    uint32_t copy_size = p->size < caps[p->id].size ? p->size : caps[p->id].size;
    if (!user(p->addr, copy_size)) return -14;
    uint32_t len = sizeof(struct virtio_gpu_ctrl_hdr) + caps[p->id].size;
    void *reply = kmalloc(len);
    if (!reply) return -12;
    struct virtio_gpu_get_capset req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_GET_CAPSET, 0);
    req.id = p->id; req.version = p->version;
    uint32_t type;
    int ret = -5;
    if (virtio_gpu_control(&req, sizeof(req), reply, len, &type) && type == VIRTIO_GPU_RESP_OK_CAPSET) {
        memcpy((void *)p->addr, (uint8_t *)reply + sizeof(req.hdr), copy_size);
        ret = 0;
    }
    kfree(reply);
    return ret;
}
static int create(struct drm_file *file, struct vg_create *p) {
    if (p->bo_handle || !p->size || p->size > MAX_RESOURCE || !p->width ||
        !p->height || !p->depth || !p->array_size || p->last_level > 15 ||
        p->flags & ~1U) return -22;
    int ret = context(file);
    if (ret) return ret;
    struct drm_gem_object *obj = ascentdrm_gem_object_create(file->dev, p->size);
    if (!obj) return -12;
    struct virgl_resource *res = kmalloc(sizeof(*res));
    if (!res) { put(obj); return -12; }
    *res = (struct virgl_resource){.id = virtio_gpu_resource_id(), .width = p->width,
        .height = p->height, .depth = p->depth, .stride = p->stride, .target = p->target};
    struct virtio_gpu_resource_create_3d req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, 0);
    req.resource_id = res->id; req.target = p->target; req.format = p->format;
    req.bind = p->bind; req.width = p->width; req.height = p->height;
    req.depth = p->depth; req.array_size = p->array_size; req.last_level = p->last_level;
    req.nr_samples = p->nr_samples; req.flags = p->flags;
    obj->virgl_resource = res;
    obj->free = free_resource;
    ret = command(&req, sizeof(req));
    if (ret) { put(obj); return ret; }
    struct virtio_gpu_resource_attach_backing backing = {0};
    backing.hdr = header(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, 0);
    backing.resource_id = res->id; backing.nr_entries = 1;
    backing.entry.addr = obj->phys_addr; backing.entry.length = obj->size;
    ret = command(&backing, sizeof(backing));
    if (!ret) ret = attach(file, res);
    if (!ret) {
        p->bo_handle = ascentdrm_file_gem_register(file, obj);
        if (!p->bo_handle) ret = -24;
        else { p->res_handle = res->id; p->size = obj->size; }
    }
    put(obj);
    return ret;
}
static int transfer(struct drm_file *file, struct vg_transfer *p, bool from) {
    struct drm_gem_object *obj = lookup(file, p->handle);
    struct virgl_resource *res = resource(obj);
    int ret = -22;
    if (!res) goto out;
    if (p->level > 15 || p->offset >= obj->size || !p->box.w || !p->box.h || !p->box.d)
        goto out;
    ret = context(file);
    if (ret) goto out;
    ret = attach(file, res);
    if (ret) goto out;
    ret = virtio_gpu_render_wait(&obj, 1, 0);
    if (ret) goto out;
    struct virtio_gpu_transfer_host_3d req = {0};
    req.hdr = header(from ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D :
        VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, file->virgl_context);
    req.box = p->box; req.offset = p->offset; req.resource_id = res->id;
    req.level = p->level; req.stride = p->stride; req.layer_stride = p->layer_stride;
    ret = command(&req, sizeof(req));
out:
    if (obj) put(obj);
    return ret;
}
int virtio_gpu_virgl_readback(struct drm_gem_object *obj) {
    struct virgl_resource *res = resource(obj);
    if (!res) return 0;
    int ret = virtio_gpu_render_wait(&obj, 1, 0);
    if (ret) return ret;
    struct virtio_gpu_transfer_host_3d req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, 0);
    req.resource_id = res->id;
    req.box = (struct virtio_gpu_box){0, 0, 0, res->width, res->height, 1};
    req.stride = res->stride;
    return command(&req, sizeof(req));
}
static int exec(struct drm_file *file, struct vg_exec *p, uint32_t abi_size) {
    if (p->flags & ~3U || !p->size || p->size > MAX_SUBMIT || (p->size & 3) ||
        p->count >= DRM_MAX_HANDLES_PER_FILE ||
        (abi_size > 32 && (p->ring_idx || p->num_in || p->num_out))) return -22;
    if (!user(p->command, p->size) || (p->count && !user(p->handles, p->count * 4))) return -14;
    int ret = context(file);
    if (ret) return ret;
#ifdef LINUXKPI_LINUX_IMPORTED
    if ((p->flags & 1) && (ret = linuxkpi_virgl_fence_wait(p->fence_fd))) return ret;
#else
    if (p->flags) return -95;
#endif
    struct drm_gem_object **objects = NULL;
    uint32_t *handles = NULL;
#ifdef LINUXKPI_LINUX_IMPORTED
    void *out_fence = NULL;
#endif
    if (p->count) {
        /* Snapshot handles and retain resources without a namespace-sized
         * stack allocation; a desktop submission can reference many BOs. */
        objects = kmalloc(p->count * (sizeof(*objects) + sizeof(*handles)));
        if (!objects) return -12;
        memset(objects, 0, p->count * sizeof(*objects));
        handles = (uint32_t *)(objects + p->count);
        memcpy(handles, (void *)p->handles, p->count * sizeof(*handles));
    }
    /* Pin file-local handles in bounded groups rather than masking/restoring
     * interrupts and acquiring the same namespace lock for every BO. Host
     * attachment and all waits remain outside the file lock. */
    for (uint32_t first = 0; first < p->count; first += 32) {
        uint32_t end = first + 32;
        if (end > p->count) end = p->count;
        spinlock_acquire(&file->lock);
        for (uint32_t i = first; i < end; i++) {
            uint32_t handle = handles[i];
            if (handle && handle < DRM_MAX_HANDLES_PER_FILE) {
                objects[i] = file->handles[handle];
                if (objects[i]) ascentdrm_gem_get(objects[i]);
            }
        }
        spinlock_release(&file->lock);
    }
    for (uint32_t i = 0; i < p->count; i++) {
        struct virgl_resource *res = resource(objects[i]);
        if (!res) { ret = -22; goto out; }
        if ((ret = attach(file, res))) goto out;
    }
    /* Shared textures used by another GL context need their producer fence;
     * batches within one context retain ordinary GL command ordering. */
    ret = virtio_gpu_render_wait(objects, p->count, file->virgl_context);
    if (ret) goto out;
    struct virtio_gpu_cmd_submit packet = {0};
    packet.hdr = header(VIRTIO_GPU_CMD_SUBMIT_3D, file->virgl_context);
    packet.size = p->size;
    if (virtio_gpu_render_queued()) {
        void (*notify)(void *, int) = NULL;
        void *notify_data = NULL;
#ifdef LINUXKPI_LINUX_IMPORTED
        if (p->flags & 2) {
            out_fence = linuxkpi_virgl_fence_create();
            if (!out_fence) { ret = -12; goto out; }
            linuxkpi_virgl_fence_get(out_fence); /* Independent job reference. */
            notify = linuxkpi_virgl_fence_complete;
            notify_data = out_fence;
        }
#endif
        /* Snapshot commands directly into the reserved job DMA. Avoid a large
         * temporary heap allocation and a second copy on every submission. */
        ret = virtio_gpu_render_submit_parts(&packet, sizeof(packet),
                       (void *)p->command, p->size,
                       objects, p->count, notify, notify_data) ? 0 : -5;
#ifdef LINUXKPI_LINUX_IMPORTED
        if (ret && out_fence) linuxkpi_virgl_fence_put(out_fence);
#endif
        if (!ret) objects = NULL; /* Completion owns the snapshot and BO refs. */
    } else {
        struct virtio_gpu_cmd_submit *req = kmalloc(sizeof(*req) + p->size);
        if (!req) { ret = -12; goto out; }
        *req = packet;
        memcpy(req + 1, (void *)p->command, p->size);
        ret = command(req, sizeof(*req) + p->size);
        kfree(req);
    }
#ifdef LINUXKPI_LINUX_IMPORTED
    if (!ret && (p->flags & 2)) {
        ret = out_fence ? linuxkpi_virgl_fence_export_pending(out_fence) :
                          linuxkpi_virgl_fence_export();
        if (ret >= 0) { p->fence_fd = ret; ret = 0; }
    }
#endif
out:
#ifdef LINUXKPI_LINUX_IMPORTED
    if (out_fence) linuxkpi_virgl_fence_put(out_fence);
#endif
    if (objects)
        for (uint32_t i = 0; i < p->count; i++) if (objects[i]) put(objects[i]);
    kfree(objects);
    return ret;
}
static int ioctl_locked(struct drm_file *file, uint32_t request, uint64_t arg) {
    uint32_t nr = request & 0xff, size = (request >> 16) & 0x3fff;
    uint32_t expected;
    switch (nr) {
    case 0x41: expected = sizeof(struct vg_map); break;
    case 0x42: expected = size; if (size != 32 && size != 40 && size != 64) return -22; break;
    case 0x43: expected = sizeof(struct vg_param); break;
    case 0x44: expected = sizeof(struct vg_create); break;
    case 0x45: expected = sizeof(struct vg_info); break;
    case 0x46: case 0x47: expected = sizeof(struct vg_transfer); break;
    case 0x48: expected = sizeof(struct vg_wait); break;
    case 0x49: expected = sizeof(struct vg_caps); break;
    default: return -25;
    }
    if (size != expected || (request & 0xc000ff00U) != 0xc0006400U) return -22;
    if (!user(arg, size)) return -14;
    /* Snapshot ioctl arguments once; never validate mutable user memory and
     * then use it again for allocation sizes or handle-array bounds. */
    union { struct vg_map map; struct vg_param param; struct vg_caps caps;
        struct vg_create create; struct vg_info info; struct vg_transfer transfer;
        struct vg_wait wait; struct vg_exec exec; } p = {0};
    memcpy(&p, (void *)arg, size);
    int ret = -22;
    if (nr == 0x43) {
        uint64_t value = 0;
        switch (p.param.param) {
        case 1: value = enabled; break;
        case 2: value = enabled; break;
        case 3: case 4: case 5: case 6: case 7: case 8: case 9: break;
        default: return -22;
        }
        /* GETPARAM.value is a userspace pointer, not an inline result. */
        if (!user(p.param.value, sizeof(value))) return -14;
        memcpy((void *)p.param.value, &value, sizeof(value));
        return 0;
    }
    if (!enabled) return -95;
    switch (nr) {
    case 0x42: ret = exec(file, &p.exec, size); break;
    case 0x44: ret = create(file, &p.create); break;
    case 0x46: case 0x47: ret = transfer(file, &p.transfer, nr == 0x46); break;
    case 0x49: return get_caps(&p.caps);
    default: {
        uint32_t handle = nr == 0x41 ? p.map.handle : nr == 0x45 ? p.info.bo_handle : p.wait.handle;
        struct drm_gem_object *obj = lookup(file, handle);
        if (!obj) return -2;
        struct virgl_resource *res = resource(obj);
        if (nr == 0x41 && !p.map.pad) {
            p.map.offset = ascentdrm_gem_mmap_offset(obj->phys_addr);
            ret = p.map.offset ? 0 : -75;
        }
        if (nr == 0x45 && res) {
            p.info.res_handle = res->id; p.info.size = obj->size; p.info.blob_mem = 0; ret = 0;
        }
        if (nr == 0x48 && !(p.wait.flags & ~1U))
            ret = virtio_gpu_render_wait_bo(obj, p.wait.flags & 1U);
        put(obj);
        break;
    }
    }
    if (!ret) memcpy((void *)arg, &p, size);
    return ret;
}
/* Context/resource operations may sleep for host fences. Contenders must
 * park too: repeatedly yielding keeps every blocked graphics client runnable
 * while video uploads and compositor submissions share this gate. */
static wait_queue_t ioctl_wait = {.lock = SPINLOCK_INIT};

static int ioctl_gate_acquire(void) {
    if (!__atomic_exchange_n(&ioctl_owner, 1, __ATOMIC_ACQUIRE))
        return 0;
    struct thread *self = sched_get_current();
    for (;;) {
        wait_queue_entry_t entry = {.thread = self};
        hal_irq_state_t flags = hal_irq_save();
        if (thread_has_pending_signal(self)) {
            hal_irq_restore(flags);
            return -4;
        }
        self->wakeup_ticks = 0;
        self->state = THREAD_BLOCKED;
        wait_queue_add_tail(&ioctl_wait, &entry);
        /* An unlock before registration has no waiter to wake. Claim again
         * after publication so that race cannot strand this client. */
        bool acquired = !__atomic_exchange_n(&ioctl_owner, 1, __ATOMIC_ACQUIRE);
        bool interrupted = !acquired && thread_has_pending_signal(self);
        if (!acquired && !interrupted) {
            hal_irq_restore(flags);
            sched_yield();
            flags = hal_irq_save();
        }
        wait_queue_remove(&ioctl_wait, &entry);
        self->wakeup_ticks = 0;
        self->state = THREAD_RUNNING;
        hal_irq_restore(flags);
        if (acquired)
            return 0;
        if (interrupted)
            return -4;
        if (!__atomic_exchange_n(&ioctl_owner, 1, __ATOMIC_ACQUIRE))
            return 0;
    }
}

static void ioctl_gate_release(void) {
    __atomic_store_n(&ioctl_owner, 0, __ATOMIC_RELEASE);
    wait_queue_wake_one(&ioctl_wait);
}

static int wait_ioctl(struct drm_file *file, uint32_t request, uint64_t arg) {
    uint32_t size = (request >> 16) & 0x3fff;
    if (size != sizeof(struct vg_wait) ||
        (request & 0xc000ff00U) != 0xc0006400U) return -22;
    if (!user(arg, size)) return -14;
    struct vg_wait p;
    memcpy(&p, (void *)arg, sizeof(p));
    if (p.flags & ~1U) return -22;
    /* Snapshot after earlier metadata/transfer operations have completed,
     * then release the gate before the potentially blocking fence wait. */
    int ret = ioctl_gate_acquire();
    if (ret) return ret;
    struct drm_gem_object *obj = lookup(file, p.handle);
    ioctl_gate_release();
    if (!obj) return -2;
    ret = virtio_gpu_render_wait_bo(obj, p.flags & 1U);
    put(obj);
    if (!ret) memcpy((void *)arg, &p, sizeof(p));
    return ret;
}

int virtio_gpu_virgl_ioctl(struct drm_file *file, uint32_t request, uint64_t arg) {
    /* WAIT snapshots a retained BO's fence prefix. Sleeping while owning
     * the global context/resource gate stalls submissions from every client,
     * including Xorg and the compositor. It needs no metadata mutation gate. */
    if ((request & 0xff) == 0x48) return wait_ioctl(file, request, arg);
    int ret = ioctl_gate_acquire();
    if (ret) return ret;
    ret = ioctl_locked(file, request, arg);
    ioctl_gate_release();
    return ret;
}
