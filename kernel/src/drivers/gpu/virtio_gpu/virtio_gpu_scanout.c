/* Native 2D presentation engine. Scanouts, rather than producer GEMs, own
 * distinct front/back resources and coherent staging allocations. */
#include "drivers/gpu/virtio_gpu/virtio_gpu_scanout.h"
#include "mm/heap.h"
#include "lib/string.h"
#include <linuxkpi/gpu_diag.h>

bool vgpu_scanout_source_valid(const struct vgpu_scanout_source *s) {
    if (!s || !s->pixels || !s->width || !s->height || !s->view_width || !s->view_height ||
        s->view_width > 8192 || s->view_height > 8192 ||
        s->pitch < (uint64_t)s->width * 4 || (s->pitch & 3) ||
        s->x >= s->width || s->y >= s->height ||
        s->view_width > s->width - s->x || s->view_height > s->height - s->y ||
        s->offset > s->size)
        return false;
    uint64_t start = (uint64_t)s->y * s->pitch + (uint64_t)s->x * 4;
    uint64_t bytes = (uint64_t)(s->view_height - 1) * s->pitch + (uint64_t)s->view_width * 4;
    return start <= s->size - s->offset && bytes <= s->size - s->offset - start;
}

static bool surface_contains(const struct vgpu_scanout_surface *surface, uint32_t id) {
    return surface && id && (surface->images[0].resource_id == id ||
                            surface->images[1].resource_id == id);
}
static void retire(struct vgpu_scanout_head *head, struct vgpu_scanout_surface *surface) {
    if (surface) {
        surface->next = head->retired;
        head->retired = surface;
    }
}
void vgpu_scanout_collect(struct vgpu_scanout_head *head, const struct vgpu_scanout_ops *ops) {
    if (head->selection_unknown || !head->retired || !ops->ready())
        return;
    struct vgpu_scanout_surface **link = &head->retired;
    while (*link) {
        struct vgpu_scanout_surface *s = *link;
        if (surface_contains(s, head->selected_id)) {
            link = &s->next;
            continue;
        }
        for (unsigned i = 0; i < 2; i++) {
            struct vgpu_scanout_image *image = &s->images[i];
            /* UNREF acknowledgement (or a confirmed absent ID) releases all
             * host references, including attached backing. Never free DMA
             * merely because create/attach/transfer returned a timeout. */
            if (image->resource_id) {
                if (!ops->destroy(image->resource_id, image->counted))
                    return;
                image->resource_id = 0;
                image->counted = false;
            }
            if (image->storage) {
                dma_free(image->storage);
                image->storage = NULL;
            }
        }
        *link = s->next;
        kfree(s);
    }
}
static bool recover_selection(struct vgpu_scanout_head *head,
                               const struct vgpu_scanout_ops *ops, uint32_t scanout) {
    if (!ops->ready())
        return false;
    if (head->selection_unknown) {
        if (!ops->select(scanout, 0, 0, 0))
            return false;
        head->selected_id = 0;
        head->selection_unknown = false;
    }
    return true;
}
static struct vgpu_scanout_surface *surface_new(struct vgpu_scanout_head *head,
    const struct vgpu_scanout_ops *ops, uint32_t width, uint32_t height) {
    struct vgpu_scanout_surface *s = kcalloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->width = width;
    s->height = height;
    uint32_t bytes = width * height * 4;
    for (unsigned i = 0; i < 2; i++) {
        struct vgpu_scanout_image *image = &s->images[i];
        image->width = width;
        image->height = height;
        image->storage = dma_alloc(bytes, 0);
        if (!image->storage)
            goto fail;
        /* Assign cleanup ownership before submitting CREATE: even a failed
         * response can have created a host object, especially after timeout. */
        image->resource_id = ops->resource_id();
        if (!image->resource_id || !ops->create(image->resource_id, width, height))
            goto fail;
        image->counted = true;
        if (!ops->attach(image->resource_id, image->storage->phys, bytes))
            goto fail;
    }
    return s;
fail:
    retire(head, s);
    vgpu_scanout_collect(head, ops);
    return NULL;
}
struct vgpu_scanout_image *vgpu_scanout_prepare(struct vgpu_scanout_head *head,
    const struct vgpu_scanout_ops *ops, uint32_t scanout, uint32_t width, uint32_t height) {
    if (!width || !height || width > 8192 || height > 8192 ||
        !recover_selection(head, ops, scanout))
        return NULL;
    vgpu_scanout_collect(head, ops);
    /* A failed cleanup can still leave a host-owned command outstanding. */
    if (head->retired || !ops->ready())
        return NULL;
    struct vgpu_scanout_surface *s = head->current;
    if (!s || s->width != width || s->height != height) {
        s = head->pending;
        if (s && (s->width != width || s->height != height)) {
            retire(head, s);
            head->pending = NULL;
            vgpu_scanout_collect(head, ops);
            if (head->retired || !ops->ready())
                return NULL;
            s = NULL;
        }
        if (!s) {
            s = surface_new(head, ops, width, height);
            if (!s)
                return NULL;
            head->pending = s;
        }
    }
    struct vgpu_scanout_image *image = &s->images[s->images[0].resource_id == head->selected_id];
    image->copied = false;
    return image;
}
bool vgpu_scanout_copy(struct vgpu_scanout_image *image,
                       const struct vgpu_scanout_source *s) {
    if (!image || !image->storage || !vgpu_scanout_source_valid(s) ||
        image->width != s->view_width || image->height != s->view_height ||
        image->storage->size < (uint64_t)s->view_width * s->view_height * 4)
        return false;
    const uint8_t *src = (const uint8_t *)s->pixels + s->offset +
                         (uint64_t)s->y * s->pitch + (uint64_t)s->x * 4;
    uint8_t *dst = image->storage->virt;
    size_t row = (size_t)s->view_width * 4;
    bool check = gpu_diag_take_sample();
    uint64_t before = check ? gpu_diag_hash_rect(src,row,s->view_height,s->pitch) : 0;
    if (row == s->pitch)
        memcpy(dst, src, row * s->view_height);
    else
        for (uint32_t y = 0; y < s->view_height; y++)
            memcpy(dst + (size_t)y * row, src + (size_t)y * s->pitch, row);
    image->copied = true;
    gpu_diag_record(GPU_DIAG_COPY_LAYOUT,s->pixels,image->resource_id,
        ((uint64_t)s->view_width<<32)|s->view_height,s->pitch,s->offset,
        (long)(((uint64_t)s->x<<32)|s->y));
    if (check) {
        uint64_t shadow=gpu_diag_hash_rect(dst,row,s->view_height,row);
        uint64_t after=gpu_diag_hash_rect(src,row,s->view_height,s->pitch);
        long flags=before!=after ? 1 : (before!=shadow ? 2 : 0);
        gpu_diag_record(GPU_DIAG_COPY_CHECK,s->pixels,image->resource_id,before,shadow,after,flags);
    }
    return true;
}
bool vgpu_scanout_publish(struct vgpu_scanout_head *head,
    const struct vgpu_scanout_ops *ops, uint32_t scanout, struct vgpu_scanout_image *image) {
    struct vgpu_scanout_surface *s = NULL;
    if (head->pending && (image == &head->pending->images[0] || image == &head->pending->images[1]))
        s = head->pending;
    else if (head->current && (image == &head->current->images[0] || image == &head->current->images[1]))
        s = head->current;
    if (!s || !image->copied || !recover_selection(head, ops, scanout))
        goto fail;
    /* A failed flush can be retried without touching the visible image.
     * Every other publication fully uploads an inactive host image first. */
    if (head->selected_id != image->resource_id) {
        if (!ops->transfer(image->resource_id, s->width, s->height))
            goto fail;
        if (!ops->select(scanout, image->resource_id, s->width, s->height)) {
            head->selection_unknown = true;
            goto fail;
        }
        head->selected_id = image->resource_id;
        if (s == head->pending) {
            retire(head, head->current);
            head->current = s;
            head->pending = NULL;
        }
    }
    if (!ops->flush(image->resource_id, s->width, s->height))
        goto fail;
    head->published++;
    gpu_diag_record(GPU_DIAG_PRESENT,head,image->resource_id,scanout,head->published,0,0);
    vgpu_scanout_collect(head, ops);
    return true;
fail:
    head->failures++;
    gpu_diag_record(GPU_DIAG_PRESENT,head,image?image->resource_id:0,scanout,
                    head->published,head->selection_unknown,-1);
    return false;
}
bool vgpu_scanout_detach(struct vgpu_scanout_head *head,
                         const struct vgpu_scanout_ops *ops, uint32_t scanout) {
    if (!recover_selection(head, ops, scanout))
        return false;
    if (head->selected_id && !ops->select(scanout, 0, 0, 0)) {
        head->selection_unknown = true;
        return false;
    }
    head->selected_id = 0;
    retire(head, head->current);
    retire(head, head->pending);
    head->current = head->pending = NULL;
    vgpu_scanout_collect(head, ops);
    return !head->retired;
}
