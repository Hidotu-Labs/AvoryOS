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
#include "mm/devframe.h"
#include "mm/tlb_shootdown.h"

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
struct vg_ctx_init { uint32_t num_params, pad; uint64_t params; };
_Static_assert(sizeof(struct vg_ctx_init) == 16, "virtgpu context-init UAPI");
struct vg_blob {
    uint32_t blob_mem, blob_flags, bo_handle, res_handle;
    uint64_t size;
    uint32_t pad, cmd_size;
    uint64_t cmd, blob_id;
};
_Static_assert(sizeof(struct vg_blob) == 48, "virtgpu blob UAPI");
_Static_assert(sizeof(struct vg_create) == 56, "virtgpu create UAPI");
_Static_assert(sizeof(struct vg_transfer) == 44, "virtgpu transfer UAPI");
_Static_assert(sizeof(struct vg_exec) == 64, "virtgpu exec UAPI");
struct virgl_attachment { uint32_t context; struct virgl_attachment *next; };
struct virgl_resource {
    uint32_t id, width, height, depth, stride, target;
    uint32_t format, bind, array_size, last_level, nr_samples, flags;
    struct drm_gem_object *obj;
    struct virgl_attachment *attachments;
    struct virgl_resource *prev, *next;
    uint32_t blob_mem, blob_flags; /* 0 for 3D resources */
    uint64_t shm_offset;           /* HOST3D: offset in the shm window */
    bool mapped;                   /* HOST3D: shm range placed and tracked */
    bool tracked;
};
struct virgl_ctx_entry {
    uint32_t id;
    uint32_t context_init;
    struct virgl_ctx_entry *next;
};
#define VIRTIO_GPU_MAX_CAPSETS 8
static struct { uint32_t version, size; } caps[VIRTIO_GPU_MAX_CAPSETS];
static bool enabled;
static uint32_t next_context, ioctl_owner;
static spinlock_t virgl_track_lock = SPINLOCK_INIT;
static struct virgl_ctx_entry *virgl_contexts_head;
static struct virgl_resource *virgl_resources_head;
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

static int virgl_attach_backing(uint32_t resource_id, struct drm_gem_object *obj) {
    if (!obj->sg_pages) {
        struct virtio_gpu_resource_attach_backing backing = {0};
        backing.hdr = header(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, 0);
        backing.resource_id = resource_id;
        backing.nr_entries = 1;
        backing.entry.addr = obj->phys_addr;
        backing.entry.length = obj->size;
        return command(&backing, sizeof(backing));
    }

    uint32_t pages = obj->size / 4096;
    if (pages == 0)
        return -22;

    uint32_t runs = 1;
    for (uint32_t i = 1; i < pages; i++) {
        if (obj->sg_pages[i] != obj->sg_pages[i - 1] + 4096)
            runs++;
    }

    size_t req_size = sizeof(struct virtio_gpu_resource_attach_backing) +
                      (size_t)(runs - 1) * sizeof(struct virtio_gpu_mem_entry);
    struct virtio_gpu_resource_attach_backing *backing = kmalloc(req_size);
    if (!backing)
        return -12;

    memset(backing, 0, sizeof(*backing));
    backing->hdr = header(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, 0);
    backing->resource_id = resource_id;
    backing->nr_entries = runs;

    struct virtio_gpu_mem_entry *entries = &backing->entry;
    uint32_t run_idx = 0;
    entries[0].addr = obj->sg_pages[0];
    entries[0].length = 4096;
    entries[0].padding = 0;
    for (uint32_t i = 1; i < pages; i++) {
        if (obj->sg_pages[i] == obj->sg_pages[i - 1] + 4096) {
            entries[run_idx].length += 4096;
        } else {
            run_idx++;
            entries[run_idx].addr = obj->sg_pages[i];
            entries[run_idx].length = 4096;
            entries[run_idx].padding = 0;
        }
    }

    int ret = command(backing, (uint32_t)req_size);
    kfree(backing);
    return ret;
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
    struct drm_gem_object *obj = ascentdrm_file_handle_peek(file, handle);
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
    struct virgl_ctx_entry *ctx_entry = kmalloc(sizeof(*ctx_entry));
    if (!ctx_entry) return -12;
    struct virtio_gpu_ctx_create req = {0};
    uint32_t id = __atomic_add_fetch(&next_context, 1, __ATOMIC_RELAXED);
    if (!id) { kfree(ctx_entry); return -12; }
    req.hdr = header(VIRTIO_GPU_CMD_CTX_CREATE, id);
    memcpy(req.name, "Ascension", 9);
    req.nlen = 9;
    /* Legacy virgl files keep context_init 0 exactly as before. */
    req.context_init = file->virgl_capset == VIRTIO_GPU_CAPSET_VENUS ?
                       VIRTIO_GPU_CAPSET_VENUS : 0;
    int ret = command(&req, sizeof(req));
    if (!ret) {
        file->virgl_context = id;
        ctx_entry->id = id;
        ctx_entry->context_init = req.context_init;
        spinlock_acquire(&virgl_track_lock);
        ctx_entry->next = virgl_contexts_head;
        virgl_contexts_head = ctx_entry;
        spinlock_release(&virgl_track_lock);
    } else {
        kfree(ctx_entry);
    }
    return ret;
}
static int attach(struct drm_file *file, struct virgl_resource *res) {
    spinlock_acquire(&virgl_track_lock);
    for (struct virgl_attachment *p = res->attachments; p; p = p->next)
        if (p->context == file->virgl_context) {
            spinlock_release(&virgl_track_lock);
            return 0;
        }
    spinlock_release(&virgl_track_lock);
    struct virgl_attachment *entry = kmalloc(sizeof(*entry));
    if (!entry) return -12;
    struct virtio_gpu_ctx_resource req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, file->virgl_context);
    req.resource_id = res->id;
    int ret = command(&req, sizeof(req));
    if (ret) { kfree(entry); return ret; }
    spinlock_acquire(&virgl_track_lock);
    for (struct virgl_attachment *p = res->attachments; p; p = p->next) {
        if (p->context == file->virgl_context) {
            spinlock_release(&virgl_track_lock);
            kfree(entry);
            return 0;
        }
    }
    *entry = (struct virgl_attachment){file->virgl_context, res->attachments};
    res->attachments = entry;
    spinlock_release(&virgl_track_lock);
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
    spinlock_acquire(&virgl_track_lock);
    if (res->tracked) {
        __atomic_sub_fetch(&ascentdrm_live_virgl_resources, 1, __ATOMIC_RELAXED);
        if (res->prev)
            res->prev->next = res->next;
        else if (virgl_resources_head == res)
            virgl_resources_head = res->next;
        if (res->next)
            res->next->prev = res->prev;
        res->prev = NULL;
        res->next = NULL;
        res->tracked = false;
    }
    struct virgl_attachment *attachments = res->attachments;
    res->attachments = NULL;
    spinlock_release(&virgl_track_lock);
    while (attachments) {
        struct virgl_attachment *entry = attachments;
        attachments = entry->next;
        kfree(entry);
    }
    kfree(res);
    ascentdrm_gem_release_storage(obj);
}
/* ---- Venus wire protocol (Phase 2) ----------------------------------------
 * Context-init (capset 4), host-side blobs and page-granular offsets in the
 * host-visible shm window. Nothing here is reachable from userspace yet; the
 * DRM uapi lands in Phase 3. Failure rules mirror free_resource(): a control
 * timeout does not prove the host dropped its mapping, so that range stays
 * reserved. Only an explicit host OK releases shm space. */
#define VENUS_SHM_PAGE 4096ULL
static spinlock_t venus_shm_lock = SPINLOCK_INIT;
static uint64_t *venus_shm_bits;   /* one bit per 4 KiB page; 1 = reserved */
static uint64_t venus_shm_pages;

static void venus_shm_init(uint64_t bytes) {
    uint64_t pages = bytes / VENUS_SHM_PAGE;
    if (!pages) return;
    spinlock_acquire(&venus_shm_lock);
    bool have = venus_shm_bits != NULL;
    spinlock_release(&venus_shm_lock);
    if (have) return;
    uint64_t words = (pages + 63) / 64;
    uint64_t *bits = kcalloc(words, sizeof(*bits));
    if (!bits) {
        klog_puts("[VENUS] shm allocator unavailable: bitmap allocation failed\n");
        return;
    }
    /* Bits past the window end are permanently reserved. */
    if (pages & 63) bits[words - 1] = ~0ULL << (pages & 63);
    spinlock_acquire(&venus_shm_lock);
    venus_shm_pages = pages;
    venus_shm_bits = bits;
    spinlock_release(&venus_shm_lock);
    klogf("[VENUS] shm allocator: %llu pages (%llu MiB)\n",
          (unsigned long long)pages, (unsigned long long)(bytes >> 20));
}

static bool venus_shm_alloc(uint64_t pages, uint64_t *offset) {
    bool ok = false;
    spinlock_acquire(&venus_shm_lock);
    if (venus_shm_bits && pages && pages <= venus_shm_pages) {
        uint64_t run = 0, start = 0;
        for (uint64_t p = 0; p < venus_shm_pages;) {
            if (!(p & 63) && venus_shm_bits[p / 64] == ~0ULL) {
                run = 0;
                p += 64;
                continue;
            }
            if (venus_shm_bits[p / 64] & (1ULL << (p % 64))) {
                run = 0;
            } else {
                if (!run) start = p;
                if (++run == pages) {
                    for (uint64_t q = start; q < start + pages; q++)
                        venus_shm_bits[q / 64] |= 1ULL << (q % 64);
                    *offset = start * VENUS_SHM_PAGE;
                    ok = true;
                    break;
                }
            }
            p++;
        }
    }
    spinlock_release(&venus_shm_lock);
    return ok;
}

static bool venus_shm_free(uint64_t offset, uint64_t pages) {
    bool ok = false;
    spinlock_acquire(&venus_shm_lock);
    uint64_t start = offset / VENUS_SHM_PAGE;
    if (venus_shm_bits && !(offset % VENUS_SHM_PAGE) && pages &&
        pages <= venus_shm_pages && start <= venus_shm_pages - pages) {
        ok = true;
        for (uint64_t q = start; q < start + pages && ok; q++)
            ok = (venus_shm_bits[q / 64] >> (q % 64)) & 1U;
        if (ok)
            for (uint64_t q = start; q < start + pages; q++)
                venus_shm_bits[q / 64] &= ~(1ULL << (q % 64));
    }
    spinlock_release(&venus_shm_lock);
    return ok;
}

static uint32_t venus_pages_for(uint64_t size) {
    if (!size || size > virtio_gpu_shm_size() || (size % VENUS_SHM_PAGE)) return 0;
    return (uint32_t)(size / VENUS_SHM_PAGE);
}

/* Page-run mem entries for a GEM object's backing, as the host command needs
 * them. Caller frees *out with kfree(). */
static int blob_entries(struct drm_gem_object *obj, struct virtio_gpu_mem_entry **out,
                        uint32_t *count) {
    if (!obj->sg_pages) {
        struct virtio_gpu_mem_entry *e = kmalloc(sizeof(*e));
        if (!e) return -12;
        *e = (struct virtio_gpu_mem_entry){.addr = obj->phys_addr, .length = (uint32_t)obj->size};
        *out = e;
        *count = 1;
        return 0;
    }
    uint32_t pages = obj->size / 4096;
    if (!pages) return -22;
    uint32_t runs = 1;
    for (uint32_t i = 1; i < pages; i++)
        if (obj->sg_pages[i] != obj->sg_pages[i - 1] + 4096) runs++;
    struct virtio_gpu_mem_entry *e = kmalloc(runs * sizeof(*e));
    if (!e) return -12;
    uint32_t r = 0;
    e[0] = (struct virtio_gpu_mem_entry){.addr = obj->sg_pages[0], .length = 4096};
    for (uint32_t i = 1; i < pages; i++) {
        if (obj->sg_pages[i] == obj->sg_pages[i - 1] + 4096) {
            e[r].length += 4096;
        } else {
            r++;
            e[r] = (struct virtio_gpu_mem_entry){.addr = obj->sg_pages[i], .length = 4096};
        }
    }
    *out = e;
    *count = runs;
    return 0;
}

int virtio_gpu_venus_ctx_create(uint32_t ctx_id) {
    if (!ctx_id || !caps[VIRTIO_GPU_CAPSET_VENUS].size) return -22;
    struct virtio_gpu_ctx_create req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_CTX_CREATE, ctx_id);
    req.context_init = VIRTIO_GPU_CAPSET_VENUS & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    memcpy(req.name, "venus", 5);
    req.nlen = 5;
    return command(&req, sizeof(req));
}

int virtio_gpu_venus_ctx_destroy(uint32_t ctx_id) {
    if (!ctx_id) return -22;
    struct virtio_gpu_ctrl_hdr req = header(VIRTIO_GPU_CMD_CTX_DESTROY, ctx_id);
    return command(&req, sizeof(req));
}

/* HOST3D/HOST3D_GUEST blobs are attached to ctx_id here; GUEST blobs need
 * entries (guest pages) and are attached the same way. On failure after the
 * create succeeded the caller must call virtio_gpu_blob_unref(). */
int virtio_gpu_blob_create(uint32_t ctx_id, uint32_t res_id, uint32_t blob_mem,
                           uint32_t blob_flags, uint64_t blob_id, uint64_t size,
                           const struct virtio_gpu_mem_entry *entries, uint32_t nr_entries) {
    if (!res_id || !size || (size % VENUS_SHM_PAGE) || nr_entries > 65536U ||
        (nr_entries && !entries) || (!nr_entries && entries))
        return -22;
    /* GUEST and HOST3D_GUEST are backed by guest pages; HOST3D is host-only. */
    bool guest_backed = blob_mem == VIRTIO_GPU_BLOB_MEM_GUEST ||
                        blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;
    if (blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST && blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D &&
        blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST)
        return -22;
    if (guest_backed != (nr_entries != 0)) return -22;
    if (blob_flags & ~7U) return -22;
    if (blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST && !ctx_id) return -22;

    size_t len = sizeof(struct virtio_gpu_resource_create_blob) +
                 (size_t)nr_entries * sizeof(struct virtio_gpu_mem_entry);
    uint8_t *buf = kmalloc(len);
    if (!buf) return -12;
    memset(buf, 0, len);
    struct virtio_gpu_resource_create_blob *req = (void *)buf;
    req->hdr = header(VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, ctx_id);
    req->resource_id = res_id;
    req->blob_mem = blob_mem;
    req->blob_flags = blob_flags;
    req->nr_entries = nr_entries;
    req->blob_id = blob_id;
    req->size = size;
    if (nr_entries)
        memcpy(buf + sizeof(*req), entries, (size_t)nr_entries * sizeof(*entries));
    int ret = command(buf, (uint32_t)len);
    kfree(buf);
    if (ret || !ctx_id) return ret;

    struct virtio_gpu_ctx_resource areq = {0};
    areq.hdr = header(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id);
    areq.resource_id = res_id;
    return command(&areq, sizeof(areq));
}

int virtio_gpu_blob_unref(uint32_t res_id) {
    if (!res_id) return -22;
    struct virtio_gpu_resource_unref req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
    req.resource_id = res_id;
    return command(&req, sizeof(req));
}

int virtio_gpu_blob_map(uint32_t res_id, uint64_t size, uint64_t *shm_offset, uint32_t *map_info) {
    uint32_t pages = venus_pages_for(size);
    if (!res_id || !pages || !shm_offset || !map_info) return -22;
    uint64_t off = 0;
    if (!venus_shm_alloc(pages, &off)) return -12;

    struct virtio_gpu_resource_map_blob req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, 0);
    req.resource_id = res_id;
    req.offset = off;
    struct virtio_gpu_resp_map_info resp = {0};
    uint32_t type = 0;
    if (!virtio_gpu_control(&req, sizeof(req), &resp, sizeof(resp), &type)) {
        /* No answer: the host may still map the window. Keep it reserved. */
        return -5;
    }
    if (type != VIRTIO_GPU_RESP_OK_MAP_INFO) {
        /* Explicit host refusal: nothing was mapped. */
        klogf("[VENUS] RESOURCE_MAP_BLOB refused: res=%u size=%llu shm_off=0x%llx type=%#x\n",
              res_id, (unsigned long long)size, (unsigned long long)off, type);
        venus_shm_free(off, pages);
        return -5;
    }
    if ((resp.map_info & VIRTIO_GPU_MAP_CACHE_MASK) > VIRTIO_GPU_MAP_CACHE_WC) {
        klogf("[VENUS] map_info %#x has an unknown cache type; unmapping\n", resp.map_info);
        if (!virtio_gpu_blob_unmap(res_id, size, off))
            return -5;
        return -95;
    }
    *shm_offset = off;
    *map_info = resp.map_info;
    return 0;
}

int virtio_gpu_blob_unmap(uint32_t res_id, uint64_t size, uint64_t shm_offset) {
    uint32_t pages = venus_pages_for(size);
    if (!res_id || !pages) return -22;
    struct virtio_gpu_resource_unmap_blob req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB, 0);
    req.resource_id = res_id;
    struct virtio_gpu_ctrl_hdr resp = {0};
    uint32_t type = 0;
    /* Any failure leaves the host mapping (and our window reservation) alone. */
    if (!virtio_gpu_control(&req, sizeof(req), &resp, sizeof(resp), &type) ||
        type != VIRTIO_GPU_RESP_OK_NODATA)
        return -5;
    return venus_shm_free(shm_offset, pages) ? 0 : -22;
}

bool virtio_gpu_venus_wire_selftest(void) {
    if (!caps[VIRTIO_GPU_CAPSET_VENUS].size || !virtio_gpu_blob_supported()) {
        klog_puts("[VENUS] wire self-test skipped: Venus capset or blob support absent\n");
        return false;
    }
    const uint64_t size = 64ULL * 1024;
    if (virtio_gpu_shm_size() < 2 * size) {
        klog_puts("[VENUS] wire self-test skipped: shm window too small\n");
        return false;
    }
    uint32_t ctx = __atomic_add_fetch(&next_context, 1, __ATOMIC_RELAXED);
    uint32_t res[2] = {virtio_gpu_resource_id(), virtio_gpu_resource_id()};
    uint64_t off[2] = {0, 0};
    uint32_t info[2] = {0, 0};
    uint32_t created = 0, mapped = 0;
    bool ctx_created = false, ok = false;
    const char *stage = NULL;

    if (!ctx || virtio_gpu_venus_ctx_create(ctx)) {
        stage = "context-init CTX_CREATE";
        goto cleanup;
    }
    ctx_created = true;
    for (uint32_t i = 0; i < 2; i++) {
        /* blob_id 0 + USE_MAPPABLE selects the host shm allocation in
         * vkr_context_create_resource(). A nonzero id names an existing
         * VkDeviceMemory and fails with ERR_UNSPEC when none exists. */
        if (virtio_gpu_blob_create(ctx, res[i], VIRTIO_GPU_BLOB_MEM_HOST3D,
                                   VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE, 0, size, NULL, 0)) {
            stage = "RESOURCE_CREATE_BLOB";
            goto cleanup;
        }
        created++;
    }
    for (uint32_t i = 0; i < 2; i++) {
        if (virtio_gpu_blob_map(res[i], size, &off[i], &info[i])) {
            stage = "RESOURCE_MAP_BLOB";
            goto cleanup;
        }
        mapped++;
    }
    if (off[0] + size > off[1] && off[1] + size > off[0]) {
        stage = "shm offsets overlap";
        goto cleanup;
    }
    if (off[0] + size > virtio_gpu_shm_size() || off[1] + size > virtio_gpu_shm_size()) {
        stage = "shm offset outside window";
        goto cleanup;
    }
    ok = true;

cleanup:;
    bool clean = true;
    for (uint32_t i = 0; i < 2; i++) {
        if (i < mapped && virtio_gpu_blob_unmap(res[i], size, off[i])) {
            clean = false;
            continue;
        }
        if (i < created && virtio_gpu_blob_unref(res[i]))
            clean = false;
    }
    if (ctx_created && clean && virtio_gpu_venus_ctx_destroy(ctx))
        clean = false;
    if (!clean) {
        ok = false;
        stage = stage ? stage : "cleanup (host state retained)";
    }
    if (ok) {
        klogf("[VENUS] PASS: CTX_CREATE capset=4, 2 HOST3D blobs mapped at shm 0x%llx/0x%llx "
              "map_info=0x%x/0x%x, unmapped and unreferenced\n",
              (unsigned long long)off[0], (unsigned long long)off[1], info[0], info[1]);
    } else {
        klogf("[VENUS] FAIL: %s\n", stage ? stage : "unknown");
    }
    return ok;
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
        if ((reply.id == VIRTIO_GPU_CAPSET_VIRGL ||
             reply.id == VIRTIO_GPU_CAPSET_VIRGL2 ||
             reply.id == VIRTIO_GPU_CAPSET_VENUS) &&
            reply.id < VIRTIO_GPU_MAX_CAPSETS &&
            reply.size && reply.size <= MAX_CAP_SIZE) {
            caps[reply.id].version = reply.version;
            caps[reply.id].size = reply.size;
        }
    }
    if (caps[VIRTIO_GPU_CAPSET_VENUS].size) {
        uint32_t vlen = sizeof(struct virtio_gpu_ctrl_hdr) + caps[VIRTIO_GPU_CAPSET_VENUS].size;
        uint8_t *vreply = kcalloc(1, vlen);
        bool fetched = false;
        uint32_t wire_ver = 0, vk_xml_ver = 0;
        if (vreply) {
            struct virtio_gpu_get_capset vreq = {0};
            uint32_t vtype = 0;
            vreq.hdr = header(VIRTIO_GPU_CMD_GET_CAPSET, 0);
            vreq.id = VIRTIO_GPU_CAPSET_VENUS;
            vreq.version = caps[VIRTIO_GPU_CAPSET_VENUS].version;
            if (virtio_gpu_control(&vreq, sizeof(vreq), vreply, vlen, &vtype) &&
                vtype == VIRTIO_GPU_RESP_OK_CAPSET) {
                fetched = true;
                if (caps[VIRTIO_GPU_CAPSET_VENUS].size >= 8) {
                    const uint32_t *words = (const uint32_t *)(vreply + sizeof(struct virtio_gpu_ctrl_hdr));
                    wire_ver = words[0];
                    vk_xml_ver = words[1];
                }
            }
            kfree(vreply);
        }
        klogf("[VIRTIO-GPU] Capset 4 (Venus): version=%u size=%u fetched=%u wire=%u vk_xml=0x%x\n",
              caps[VIRTIO_GPU_CAPSET_VENUS].version,
              caps[VIRTIO_GPU_CAPSET_VENUS].size,
              fetched ? 1U : 0U, wire_ver, vk_xml_ver);
    }
    if (caps[VIRTIO_GPU_CAPSET_VENUS].size && virtio_gpu_blob_supported())
        venus_shm_init(virtio_gpu_shm_size());
    enabled = caps[1].size != 0;
    if (enabled) klog_puts(virtio_gpu_render_queued() ?
        "[VIRTIO-GPU] Virgl rendering enabled (preallocated queued host fences)\n" :
        "[VIRTIO-GPU] Virgl rendering enabled (synchronous fallback)\n");
}
bool virtio_gpu_virgl_enabled(void) { return enabled; }
void virtio_gpu_virgl_close(struct drm_file *file) {
    if (!file->virgl_context) return;
    if (virtio_gpu_wait_idle(false)) return; /* Retain a timed-out host context. */
    uint32_t id = file->virgl_context;
    struct virtio_gpu_ctrl_hdr req = header(VIRTIO_GPU_CMD_CTX_DESTROY, id);
    command(&req, sizeof(req));
    spinlock_acquire(&virgl_track_lock);
    struct virgl_ctx_entry **link = &virgl_contexts_head;
    struct virgl_ctx_entry *victim = NULL;
    while (*link) {
        if ((*link)->id == id) {
            victim = *link;
            *link = victim->next;
            break;
        }
        link = &(*link)->next;
    }
    spinlock_release(&virgl_track_lock);
    kfree(victim);
    file->virgl_context = 0;
}

static bool virgl_ctx_in_list(const uint32_t *ctx_ids, uint32_t count,
                              uint32_t id) {
    for (uint32_t i = 0; i < count; i++) {
        if (ctx_ids[i] == id)
            return true;
    }
    return false;
}

int virtio_gpu_virgl_restore_after_reset(uint64_t features, uint32_t count) {
    virtio_gpu_virgl_init(features, count);
    if (!enabled)
        return 0;

    spinlock_acquire(&virgl_track_lock);
    uint32_t ctx_cap = 0, res_cap = 0, att_cap = 0;
    for (struct virgl_ctx_entry *c = virgl_contexts_head; c; c = c->next)
        ctx_cap++;
    for (struct virgl_resource *r = virgl_resources_head; r; r = r->next) {
        res_cap++;
        for (struct virgl_attachment *a = r->attachments; a; a = a->next)
            att_cap++;
    }
    spinlock_release(&virgl_track_lock);

    if (!ctx_cap && !res_cap)
        return 0;

    struct virgl_res_snap {
        uint32_t id, width, height, depth, target;
        uint32_t format, bind, array_size, last_level, nr_samples, flags;
        struct drm_gem_object *obj;
        uint32_t att_offset, att_count;
        uint32_t blob_mem, blob_flags;
    };

    uint32_t *ctx_ids = ctx_cap ? kmalloc(ctx_cap * sizeof(uint32_t)) : NULL;
    uint32_t *ctx_inits = ctx_cap ? kmalloc(ctx_cap * sizeof(uint32_t)) : NULL;
    struct virgl_res_snap *snaps =
        res_cap ? kmalloc(res_cap * sizeof(*snaps)) : NULL;
    uint32_t *all_atts = att_cap ? kmalloc(att_cap * sizeof(uint32_t)) : NULL;
    if ((ctx_cap && (!ctx_ids || !ctx_inits)) || (res_cap && !snaps) ||
        (att_cap && !all_atts)) {
        kfree(ctx_ids);
        kfree(ctx_inits);
        kfree(snaps);
        kfree(all_atts);
        return -12;
    }

    uint32_t n_ctx = 0, n_res = 0, n_att = 0;
    spinlock_acquire(&virgl_track_lock);
    for (struct virgl_ctx_entry *c = virgl_contexts_head;
         c && n_ctx < ctx_cap; c = c->next) {
        ctx_inits[n_ctx] = c->context_init;
        ctx_ids[n_ctx++] = c->id;
    }
    for (struct virgl_resource *r = virgl_resources_head;
         r && n_res < res_cap; r = r->next) {
        if (!r->obj)
            continue;
        int old_ref = __atomic_load_n(&r->obj->refcount, __ATOMIC_ACQUIRE);
        while (old_ref > 0 &&
               !__atomic_compare_exchange_n(&r->obj->refcount, &old_ref,
                                            old_ref + 1, true,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
        }
        if (old_ref <= 0)
            continue;
        struct virgl_res_snap *s = &snaps[n_res++];
        s->id = r->id;
        s->width = r->width;
        s->height = r->height;
        s->depth = r->depth;
        s->target = r->target;
        s->format = r->format;
        s->bind = r->bind;
        s->array_size = r->array_size;
        s->last_level = r->last_level;
        s->nr_samples = r->nr_samples;
        s->flags = r->flags;
        s->blob_mem = r->blob_mem;
        s->blob_flags = r->blob_flags;
        s->obj = r->obj;
        s->att_offset = n_att;
        s->att_count = 0;
        for (struct virgl_attachment *a = r->attachments;
             a && n_att < att_cap; a = a->next) {
            all_atts[n_att++] = a->context;
            s->att_count++;
        }
    }
    spinlock_release(&virgl_track_lock);

    int ret = 0;
    for (uint32_t i = 0; i < n_ctx; i++) {
        struct virtio_gpu_ctx_create req = {0};
        req.hdr = header(VIRTIO_GPU_CMD_CTX_CREATE, ctx_ids[i]);
        memcpy(req.name, "Ascension", 9);
        req.nlen = 9;
        req.context_init = ctx_inits[i];
        if (command(&req, sizeof(req)))
            ret = -5;
    }

    for (uint32_t i = 0; i < n_res; i++) {
        struct virgl_res_snap *s = &snaps[i];
        if (s->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D) {
            /* Host memory does not survive a device reset, so this blob cannot
             * be rebuilt. The recovery reports failure. */
            klogf("[VIRGL] S3 recovery: host blob %u not restored\n", s->id);
            ret = -5;
            put(s->obj);
            continue;
        }
        if (s->blob_mem) {
            /* Guest blobs are recreated from their pages, not as 3D resources. */
            struct virtio_gpu_mem_entry *ents = NULL;
            uint32_t nents = 0;
            int r = blob_entries(s->obj, &ents, &nents);
            if (!r)
                r = virtio_gpu_blob_create(0, s->id, s->blob_mem, s->blob_flags, 0,
                                           s->obj->size, ents, nents);
            kfree(ents);
            if (r) {
                ret = -5;
            } else {
                for (uint32_t a = 0; a < s->att_count; a++) {
                    uint32_t cid = all_atts[s->att_offset + a];
                    if (!virgl_ctx_in_list(ctx_ids, n_ctx, cid))
                        continue;
                    struct virtio_gpu_ctx_resource areq = {0};
                    areq.hdr = header(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, cid);
                    areq.resource_id = s->id;
                    if (command(&areq, sizeof(areq)))
                        ret = -5;
                }
            }
            put(s->obj);
            continue;
        }
        struct virtio_gpu_resource_create_3d req = {0};
        req.hdr = header(VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, 0);
        req.resource_id = s->id;
        req.target = s->target;
        req.format = s->format;
        req.bind = s->bind;
        req.width = s->width;
        req.height = s->height;
        req.depth = s->depth;
        req.array_size = s->array_size;
        req.last_level = s->last_level;
        req.nr_samples = s->nr_samples;
        req.flags = s->flags;
        if (command(&req, sizeof(req))) {
            ret = -5;
        } else {
            if (virgl_attach_backing(s->id, s->obj)) {
                ret = -5;
            } else {
                for (uint32_t a = 0; a < s->att_count; a++) {
                    uint32_t cid = all_atts[s->att_offset + a];
                    if (!virgl_ctx_in_list(ctx_ids, n_ctx, cid))
                        continue;
                    struct virtio_gpu_ctx_resource areq = {0};
                    areq.hdr = header(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, cid);
                    areq.resource_id = s->id;
                    if (command(&areq, sizeof(areq)))
                        ret = -5;
                }
            }
        }
        put(s->obj);
    }

    kfree(all_atts);
    kfree(snaps);
    kfree(ctx_inits);
    kfree(ctx_ids);
    klogf("[VIRGL] S3 recovery: restored %u context(s) and %u 3D resource(s) (ret=%d)\n",
          n_ctx, n_res, ret);
    return ret;
}
static int get_caps(struct vg_caps *p) {
    if (p->pad || p->id < 1 || p->id >= VIRTIO_GPU_MAX_CAPSETS || !caps[p->id].size ||
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
        p->flags & ~1U) {
        klogf("[VIRGL] create rejected: size=%u %ux%ux%u layers=%u last_level=%u flags=%#x bo=%u\n",
              p->size, p->width, p->height, p->depth, p->array_size,
              p->last_level, p->flags, p->bo_handle);
        return -22;
    }
    int ret = context(file);
    if (ret) { klogf("[VIRGL] ctx create failed: %d\n", ret); return ret; }
    struct drm_gem_object *obj = ascentdrm_gem_object_create_sg(file->dev, p->size);
    if (!obj) { klogf("[VIRGL] create: no memory for size=%u\n", p->size); return -12; }
    struct virgl_resource *res = kmalloc(sizeof(*res));
    if (!res) { put(obj); return -12; }
    *res = (struct virgl_resource){
        .id = virtio_gpu_resource_id(),
        .width = p->width,
        .height = p->height,
        .depth = p->depth,
        .stride = p->stride,
        .target = p->target,
        .format = p->format,
        .bind = p->bind,
        .array_size = p->array_size,
        .last_level = p->last_level,
        .nr_samples = p->nr_samples,
        .flags = p->flags,
        .obj = obj,
    };
    struct virtio_gpu_resource_create_3d req = {0};
    req.hdr = header(VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, 0);
    req.resource_id = res->id; req.target = p->target; req.format = p->format;
    req.bind = p->bind; req.width = p->width; req.height = p->height;
    req.depth = p->depth; req.array_size = p->array_size; req.last_level = p->last_level;
    req.nr_samples = p->nr_samples; req.flags = p->flags;
    obj->virgl_resource = res;
    obj->free = free_resource;
    ret = command(&req, sizeof(req));
    if (ret) {
        klogf("[VIRGL] RESOURCE_CREATE_3D rejected by host: id=%u target=%u fmt=%u bind=%#x %ux%ux%u\n",
              res->id, p->target, p->format, p->bind, p->width, p->height, p->depth);
        put(obj);
        return ret;
    }
    spinlock_acquire(&virgl_track_lock);
    res->prev = NULL;
    res->next = virgl_resources_head;
    if (virgl_resources_head)
        virgl_resources_head->prev = res;
    virgl_resources_head = res;
    res->tracked = true;
    __atomic_add_fetch(&ascentdrm_live_virgl_resources, 1, __ATOMIC_RELAXED);
    spinlock_release(&virgl_track_lock);
    ret = virgl_attach_backing(res->id, obj);
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
    /* Blob resources are not 3D resources; legacy transfers are refused. */
    if (!res || res->blob_mem) goto out;
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
/* ---- Host-visible (HOST3D) blobs ------------------------------------------
 * The host places the memory in the shm window at MAP time, so the GEM object
 * has no guest pages. Once mapped, its phys range is registered with the
 * device-frame tracker: each user PTE holds one object reference. Final
 * teardown (UNMAP_BLOB, UNREF, shm release) runs on the devframe worker after
 * the last reference is gone and the TLB has been flushed. */
static void device_blob_teardown(void *owner);

static void device_blob_get(void *owner) { ascentdrm_gem_get(owner); }
static bool device_blob_put(void *owner) { return ascentdrm_gem_put_last(owner); }
/* Tracker's final release: unlink the object as ascentdrm_gem_object_free()
 * does, then tear down. */
static void device_blob_release(void *owner) {
    struct drm_gem_object *obj = owner;
    spinlock_acquire(&obj->dev->lock);
    list_del(&obj->list);
    spinlock_release(&obj->dev->lock);
    device_blob_teardown(obj);
}
static const struct devframe_ops device_blob_ops = {
    .get = device_blob_get,
    .put = device_blob_put,
    .release = device_blob_release,
};

static struct drm_gem_object *device_blob_object(struct drm_device *dev, size_t size) {
    struct drm_gem_object *obj = kmalloc(sizeof(*obj));
    if (!obj) return NULL;
    memset(obj, 0, sizeof(*obj));
    obj->dev = dev;
    obj->size = size;
    obj->cache_mode = DRM_GEM_CACHE_WB;
    __atomic_store_n(&obj->refcount, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ascentdrm_live_gem_objects, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ascentdrm_live_gem_bytes, obj->size, __ATOMIC_RELAXED);
    spinlock_acquire(&dev->lock);
    obj->handle = dev->next_gem_handle++;
    list_add_tail(&obj->list, &dev->gem_objects);
    spinlock_release(&dev->lock);
    return obj;
}

/* An object that never reached a host resource: unlink and free it. */
static void device_blob_discard(struct drm_gem_object *obj) {
    spinlock_acquire(&obj->dev->lock);
    list_del(&obj->list);
    spinlock_release(&obj->dev->lock);
    __atomic_sub_fetch(&ascentdrm_live_gem_objects, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&ascentdrm_live_gem_bytes, obj->size, __ATOMIC_RELAXED);
    kfree(obj);
}

/* Worker context. The object is already unlinked from dev->gem_objects. */
static void device_blob_teardown(void *owner) {
    struct drm_gem_object *obj = owner;
    struct virgl_resource *res = resource(obj);
    /* Every PTE dropped its reference, but other CPUs may still cache the
     * translations, so the host range must not be recycled before this. */
    tlb_shootdown_all();
    if (res->mapped) {
        if (virtio_gpu_blob_unmap(res->id, obj->size, res->shm_offset)) {
            klogf("[VIRGL] device blob %u kept: host did not confirm UNMAP\n", res->id);
            return;
        }
        res->mapped = false;
        devframe_unregister(obj);
    }
    if (virtio_gpu_blob_unref(res->id)) {
        klogf("[VIRGL] device blob %u kept: host did not confirm UNREF\n", res->id);
        return;
    }
    spinlock_acquire(&virgl_track_lock);
    if (res->tracked) {
        __atomic_sub_fetch(&ascentdrm_live_virgl_resources, 1, __ATOMIC_RELAXED);
        if (res->prev)
            res->prev->next = res->next;
        else if (virgl_resources_head == res)
            virgl_resources_head = res->next;
        if (res->next)
            res->next->prev = res->prev;
        res->prev = NULL;
        res->next = NULL;
        res->tracked = false;
    }
    struct virgl_attachment *att = res->attachments;
    res->attachments = NULL;
    spinlock_release(&virgl_track_lock);
    while (att) {
        struct virgl_attachment *e = att;
        att = e->next;
        kfree(e);
    }
    kfree(res);
    __atomic_sub_fetch(&ascentdrm_live_gem_objects, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&ascentdrm_live_gem_bytes, obj->size, __ATOMIC_RELAXED);
    kfree(obj);
}

/* GEM free hook. Reached from handle close and other paths that may hold
 * locks, so the teardown (TLB flush, command queue) is queued, never inline. */
static void device_blob_free(struct drm_device *dev, struct drm_gem_object *obj) {
    (void)dev;
    if (!devframe_defer(obj, device_blob_teardown))
        klogf("[VIRGL] device blob %u leaked: teardown could not be queued\n",
              resource(obj)->id);
}

/* MAP for a HOST3D blob: place the memory in the shm window, expose the phys
 * range as the object's token, and register it with the device-frame tracker.
 * The cache mode follows the host's map_info. */
static int device_blob_map(struct drm_gem_object *obj, struct virgl_resource *res,
                           uint64_t *offset) {
    if (!res->mapped) {
        uint64_t shm_off = 0;
        uint32_t info = 0;
        int ret = virtio_gpu_blob_map(res->id, obj->size, &shm_off, &info);
        if (ret) return ret;
        enum drm_gem_cache_mode mode;
        switch (info & VIRTIO_GPU_MAP_CACHE_MASK) {
        case VIRTIO_GPU_MAP_CACHE_NONE:
        case VIRTIO_GPU_MAP_CACHE_CACHED:
            mode = DRM_GEM_CACHE_WB;
            break;
        case VIRTIO_GPU_MAP_CACHE_WC:
            mode = DRM_GEM_CACHE_WC;
            break;
        default:
            /* Uncached maps need a PAT mode that GEM mmap does not have. */
            virtio_gpu_blob_unmap(res->id, obj->size, shm_off);
            return -95;
        }
        uint64_t phys = virtio_gpu_shm_phys() + shm_off;
        obj->phys_addr = phys;
        obj->cache_mode = mode;
        res->shm_offset = shm_off;
        res->mapped = true;
        ret = devframe_register(phys, obj->size, obj, &device_blob_ops);
        if (ret) {
            res->mapped = false;
            virtio_gpu_blob_unmap(res->id, obj->size, shm_off);
            return ret;
        }
        obj->device_range = true;
    }
    *offset = ascentdrm_gem_mmap_offset(obj->phys_addr);
    return *offset ? 0 : -75;
}

/* Venus embeds a command stream in a blob create. It is submitted on the
 * file's context before the blob is created, the same order Linux uses. */
static int blob_submit_cmd(struct drm_file *file, struct vg_blob *p) {
    if (!p->cmd_size) return p->cmd ? -22 : 0;
    /* Same bound as EXECBUFFER: the 32-byte packet header still fits REQ_CAP. */
    if ((p->cmd_size & 3) || p->cmd_size > MAX_SUBMIT) return -22;
    if (!user(p->cmd, p->cmd_size)) return -14;
    int ret = context(file);
    if (ret) return ret;
    uint32_t len = sizeof(struct virtio_gpu_cmd_submit) + p->cmd_size;
    uint8_t *buf = kmalloc(len);
    if (!buf) return -12;
    memset(buf, 0, sizeof(struct virtio_gpu_cmd_submit));
    struct virtio_gpu_cmd_submit *packet = (struct virtio_gpu_cmd_submit *)buf;
    packet->hdr = header(VIRTIO_GPU_CMD_SUBMIT_3D, file->virgl_context);
    packet->size = p->cmd_size;
    memcpy(buf + sizeof(*packet), (void *)p->cmd, p->cmd_size);
    ret = command(buf, len);
    kfree(buf);
    return ret;
}

/* Records file's context on res without sending anything. */
static int device_attach_track(struct drm_file *file, struct virgl_resource *res) {
    struct virgl_attachment *entry = kmalloc(sizeof(*entry));
    if (!entry) return -12;
    spinlock_acquire(&virgl_track_lock);
    *entry = (struct virgl_attachment){file->virgl_context, res->attachments};
    res->attachments = entry;
    spinlock_release(&virgl_track_lock);
    return 0;
}

/* Dma-buf export places a mappable blob on demand, the same way MAP does.
 * A blob without USE_MAPPABLE (e.g. device-local WSI images) is host-only:
 * Linux never maps it, and the host may refuse (amdgpu VRAM without CPU
 * access fails with EPERM), so it is exported opaque instead. */
static int device_blob_prepare(struct drm_gem_object *obj) {
    struct virgl_resource *res = resource(obj);
    uint64_t offset = 0;
    if (!res || res->blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D)
        return 0;
    if (!(res->blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE)) {
        obj->device_opaque = true;
        return 0;
    }
    return device_blob_map(obj, res, &offset);
}

/* RESOURCE_CREATE_BLOB, HOST3D with blob_id 0 and USE_MAPPABLE: the host
 * allocates the memory at MAP time. Importing an existing VkDeviceMemory by
 * blob_id is not supported. */
static int blob_create_host(struct drm_file *file, struct vg_blob *p) {
    if (p->bo_handle || p->res_handle || !p->size || p->size > MAX_RESOURCE ||
        (p->size % 4096) || p->pad)
        return -22;
    /* blob_id != 0 imports an existing host VkDeviceMemory. SHAREABLE blobs
     * can be exported as dma-buf; CROSS_DEVICE is not built yet. */
    if (p->blob_flags & ~(VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE |
                          VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE))
        return -95;
    if (!virtio_gpu_blob_supported()) return -95;
    int ret = context(file);
    if (ret) return ret;
    struct drm_gem_object *obj = device_blob_object(file->dev, p->size);
    if (!obj) return -12;
    struct virgl_resource *res = kmalloc(sizeof(*res));
    if (!res) {
        device_blob_discard(obj);
        return -12;
    }
    *res = (struct virgl_resource){
        .id = virtio_gpu_resource_id(),
        .obj = obj,
        .blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D,
        .blob_flags = p->blob_flags,
    };
    /* HOST3D blobs must name their context; the host attaches it here. */
    ret = virtio_gpu_blob_create(file->virgl_context, res->id, res->blob_mem,
                                 res->blob_flags, p->blob_id, p->size, NULL, 0);
    if (ret) {
        klogf("[VIRGL] host blob create rejected: id=%u size=%llu ret=%d\n",
              res->id, (unsigned long long)p->size, ret);
        kfree(res);
        device_blob_discard(obj);
        return ret;
    }
    obj->virgl_resource = res;
    obj->free = device_blob_free;
    obj->prepare_export = device_blob_prepare;
    spinlock_acquire(&virgl_track_lock);
    res->prev = NULL;
    res->next = virgl_resources_head;
    if (virgl_resources_head)
        virgl_resources_head->prev = res;
    virgl_resources_head = res;
    res->tracked = true;
    __atomic_add_fetch(&ascentdrm_live_virgl_resources, 1, __ATOMIC_RELAXED);
    spinlock_release(&virgl_track_lock);
    /* The host attached the context above; only record it for S3 restore. */
    ret = device_attach_track(file, res);
    if (!ret) {
        p->bo_handle = ascentdrm_file_gem_register(file, obj);
        if (!p->bo_handle) ret = -24;
        else p->res_handle = res->id;
    }
    put(obj);
    return ret;
}

/* RESOURCE_CREATE_BLOB, guest-backed only: the pages come from a GEM object
 * and go into the host command. HOST3D blobs need host-visible memory mapped
 * into userspace (Phase 4), so they are refused until then. */
static int blob_create(struct drm_file *file, struct vg_blob *p) {
    int sub = blob_submit_cmd(file, p);
    if (sub) return sub;
    if (p->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D) return blob_create_host(file, p);
    if (p->blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST) return -95;
    /* Shareable and cross-device export are not built yet. */
    if (p->blob_flags & ~VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE) return -95;
    if (p->bo_handle || p->res_handle || !p->size || p->size > MAX_RESOURCE ||
        (p->size % 4096) || p->pad || p->blob_id)
        return -22;
    if (!virtio_gpu_blob_supported()) return -95;
    int ret = context(file);
    if (ret) return ret;
    struct drm_gem_object *obj = ascentdrm_gem_object_create_sg(file->dev, p->size);
    if (!obj) return -12;
    struct virgl_resource *res = kmalloc(sizeof(*res));
    if (!res) { put(obj); return -12; }
    *res = (struct virgl_resource){
        .id = virtio_gpu_resource_id(),
        .obj = obj,
        .blob_mem = p->blob_mem,
        .blob_flags = p->blob_flags,
    };
    obj->virgl_resource = res;
    obj->free = free_resource;
    struct virtio_gpu_mem_entry *ents = NULL;
    uint32_t nents = 0;
    ret = blob_entries(obj, &ents, &nents);
    if (!ret) {
        ret = virtio_gpu_blob_create(0, res->id, res->blob_mem, res->blob_flags, 0,
                                     p->size, ents, nents);
        kfree(ents);
    }
    if (ret) {
        klogf("[VIRGL] blob create rejected by host: id=%u size=%llu ret=%d\n",
              res->id, (unsigned long long)p->size, ret);
        put(obj);
        return ret;
    }
    spinlock_acquire(&virgl_track_lock);
    res->prev = NULL;
    res->next = virgl_resources_head;
    if (virgl_resources_head)
        virgl_resources_head->prev = res;
    virgl_resources_head = res;
    res->tracked = true;
    __atomic_add_fetch(&ascentdrm_live_virgl_resources, 1, __ATOMIC_RELAXED);
    spinlock_release(&virgl_track_lock);
    ret = attach(file, res);
    if (!ret) {
        p->bo_handle = ascentdrm_file_gem_register(file, obj);
        if (!p->bo_handle) ret = -24;
        else p->res_handle = res->id;
    }
    put(obj);
    return ret;
}

static int exec(struct drm_file *file, struct vg_exec *p, uint32_t abi_size) {
    /* Venus submits each VkQueue on its own ring. The host retires a ring's
     * fence only after that queue's GPU work completes; ring 0 (and a fence
     * without a ring) retires immediately on the host CPU timeline, which
     * let Venus present frames the GPU had not finished. */
    if (p->flags & ~7U || !p->size || p->size > MAX_SUBMIT || (p->size & 3) ||
        p->count >= DRM_MAX_HANDLES_PER_FILE ||
        (abi_size > 32 && (p->ring_idx >= 64 || p->num_in || p->num_out))) {
        klogf("[VIRGL] exec rejected: flags=%#x size=%u count=%u ring=%u in=%u out=%u abi=%u\n",
              p->flags, p->size, p->count, p->ring_idx, p->num_in, p->num_out, abi_size);
        return -22;
    }
    if (!user(p->command, p->size) || (p->count && !user(p->handles, p->count * 4))) return -14;
    int ret = context(file);
    if (ret) return ret;
#ifdef LINUXKPI_LINUX_IMPORTED
    if ((p->flags & 1) && (ret = linuxkpi_virgl_fence_wait(p->fence_fd))) return ret;
#else
    if (p->flags & 3U) return -95;
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
                objects[i] = ascentdrm_file_handle_peek(file, handle);
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
    /* VIRTGPU_EXECBUF_RING_IDX: fence on the submitting queue's timeline. */
    if ((p->flags & 4U) && abi_size > 32 &&
        file->virgl_capset == VIRTIO_GPU_CAPSET_VENUS) {
        packet.hdr.flags |= VIRTIO_GPU_FLAG_INFO_RING_IDX;
        packet.hdr.ring_idx = (uint8_t)p->ring_idx;
    }
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
/* Venus is advertised only when the host exposes capset 4. */
static bool venus_present(void) {
    return caps[VIRTIO_GPU_CAPSET_VENUS].size != 0;
}

/* GETPARAM SUPPORTED_CAPSET_IDs: bit (1 << id) per capset the host exposes. */
static uint64_t capset_mask(void) {
    uint64_t mask = 0;
    for (uint32_t id = 1; id < VIRTIO_GPU_MAX_CAPSETS; id++)
        if (caps[id].size)
            mask |= 1ULL << id;
    return mask;
}

#define VG_CTX_PARAM_CAPSET_ID       0x0001
#define VG_CTX_PARAM_NUM_RINGS       0x0002
#define VG_CTX_PARAM_POLL_RINGS_MASK 0x0003
#define VG_CTX_PARAM_DEBUG_NAME      0x0004

/* CONTEXT_INIT. Venus selects a host context of capset 4 for this file. The
 * virgl capsets are accepted and recorded but keep the legacy context, since
 * gallium's virgl winsys calls this on every start and fails if it errors.
 * Only one ring and no poll mask are supported. */
static int context_init(struct drm_file *file, struct vg_ctx_init *p) {
    if (p->pad || !p->num_params || p->num_params > 8) return -22;
    if (!user(p->params, p->num_params * sizeof(struct vg_param))) return -14;
    struct vg_param in[8];
    memcpy(in, (void *)p->params, p->num_params * sizeof(struct vg_param));
    bool have_capset = false;
    uint64_t capset = 0, rings = 1, poll = 0;
    for (uint32_t i = 0; i < p->num_params; i++) {
        switch (in[i].param) {
        case VG_CTX_PARAM_CAPSET_ID:
            if (have_capset) return -22;
            have_capset = true;
            capset = in[i].value;
            break;
        case VG_CTX_PARAM_NUM_RINGS: rings = in[i].value; break;
        case VG_CTX_PARAM_POLL_RINGS_MASK: poll = in[i].value; break;
        case VG_CTX_PARAM_DEBUG_NAME: break;
        default: return -22;
        }
    }
    if (!have_capset || capset >= VIRTIO_GPU_MAX_CAPSETS || !caps[capset].size)
        return -22;
    if (capset != VIRTIO_GPU_CAPSET_VENUS && capset != VIRTIO_GPU_CAPSET_VIRGL &&
        capset != VIRTIO_GPU_CAPSET_VIRGL2)
        return -22;
    /* Venus asks for 64 rings; EXECBUFFER tags each submission's fence with
     * its ring so the host retires it on that queue's timeline. */
    if (!rings || rings > 64 || poll) return -22;
    /* As in Linux, a second init or init after a context exists is EEXIST. */
    if (file->virgl_context || file->virgl_capset) return -17;
    file->virgl_capset = (uint32_t)capset;
    return 0;
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
    case 0x4b: expected = sizeof(struct vg_ctx_init); break;
    case 0x4a:
        /* Accept any struct size from our prefix up to the union size, so a
         * newer userspace header with trailing fields still gets through. */
        if (size < sizeof(struct vg_blob) || size > sizeof(struct vg_exec)) {
            klogf("[VIRGL] blob ioctl: unexpected struct size %u\n", size);
            return -22;
        }
        expected = size;
        break;
    default: return -25;
    }
    if (size != expected || (request & 0xc000ff00U) != 0xc0006400U) return -22;
    if (!user(arg, size)) return -14;
    /* Snapshot ioctl arguments once; never validate mutable user memory and
     * then use it again for allocation sizes or handle-array bounds. */
    union { struct vg_map map; struct vg_param param; struct vg_caps caps;
        struct vg_create create; struct vg_info info; struct vg_transfer transfer;
        struct vg_wait wait; struct vg_exec exec; struct vg_ctx_init ctx_init; struct vg_blob blob; } p = {0};
    memcpy(&p, (void *)arg, size);
    int ret = -22;
    if (nr == 0x43) {
        uint64_t value = 0;
        switch (p.param.param) {
        case 1: value = enabled; break;
        case 2: value = enabled; break;
        /* CONTEXT_INIT and the capset mask are advertised only with Venus. */
        case 6: value = venus_present() ? 1 : 0; break;
        case 7: value = capset_mask(); break;
        /* RESOURCE_BLOB and HOST_VISIBLE: available once Venus and the blob
         * feature with the shm window are present. CROSS_DEVICE stays 0. */
        case 3: case 4: value = venus_present() && virtio_gpu_blob_supported(); break;
        /* Venus aligns blob sizes to this value; blobs here are page-granular. */
        case 9: value = venus_present() ? 4096 : 0; break;
        case 5: case 8: break;
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
    /* Venus files have no legacy 3D resources; legacy create/transfer are refused. */
    case 0x44: ret = file->virgl_capset == VIRTIO_GPU_CAPSET_VENUS ? -22 : create(file, &p.create); break;
    case 0x46: case 0x47:
        ret = file->virgl_capset == VIRTIO_GPU_CAPSET_VENUS ? -22 :
              transfer(file, &p.transfer, nr == 0x46);
        break;
    case 0x49: return get_caps(&p.caps);
    case 0x4b: ret = context_init(file, &p.ctx_init); break;
    case 0x4a:
        ret = blob_create(file, &p.blob);
        if (ret)
            klogf("[VIRGL] blob create failed: ret=%d blob_mem=%u flags=%u size=%llu "
                  "cmd_size=%u blob_id=%llu\n", ret, p.blob.blob_mem, p.blob.blob_flags,
                  (unsigned long long)p.blob.size, p.blob.cmd_size,
                  (unsigned long long)p.blob.blob_id);
        break;
    default: {
        uint32_t handle = nr == 0x41 ? p.map.handle : nr == 0x45 ? p.info.bo_handle : p.wait.handle;
        struct drm_gem_object *obj = lookup(file, handle);
        if (!obj) return -2;
        struct virgl_resource *res = resource(obj);
        if (nr == 0x41 && !p.map.pad && res && res->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D) {
            /* As on Linux, only USE_MAPPABLE blobs have a CPU mapping. */
            ret = (res->blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE)
                      ? device_blob_map(obj, res, &p.map.offset) : -22;
        } else if (nr == 0x41 && !p.map.pad) {
            p.map.offset = ascentdrm_gem_mmap_offset(obj->phys_addr);
            ret = p.map.offset ? 0 : -75;
        }
        if (nr == 0x45 && res) {
            p.info.res_handle = res->id; p.info.size = obj->size; p.info.blob_mem = res->blob_mem; ret = 0;
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
    /* With Venus present, log every failing ioctl so an ICD init failure can be
     * traced from the serial log without a userspace debug build. */
    /* GET_CAPS -EINVAL is Mesa's vdrm probing capsets the host lacks (as on
     * Linux); it repeats on every Vulkan/GL start, so it is not logged. */
    if (ret && venus_present() && !((request & 0xff) == 0x49 && ret == -22))
        klogf("[VIRGL] venus-era ioctl 0x%02x failed: %d\n", request & 0xff, ret);
    return ret;
}
