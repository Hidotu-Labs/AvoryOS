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
    image->pixels_valid = true;
    image->damage_tracked = false;
    image->host_pixels_valid = false;
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
void vgpu_scanout_invalidate(struct vgpu_scanout_head *head) {
    if (head->current)
        head->current->source_valid = false;
    if (head->pending)
        head->pending->source_valid = false;
}
static struct vgpu_scanout_surface *image_surface(struct vgpu_scanout_head *head,
                                                  struct vgpu_scanout_image *image) {
    if (head->pending && (image == &head->pending->images[0] || image == &head->pending->images[1]))
        return head->pending;
    if (head->current && (image == &head->current->images[0] || image == &head->current->images[1]))
        return head->current;
    return NULL;
}
static struct vgpu_scanout_damage damage_union(struct vgpu_scanout_damage previous,
                                                struct vgpu_scanout_damage next) {
    uint32_t right = previous.x + previous.width;
    uint32_t bottom = previous.y + previous.height;
    if (next.x + next.width > right)
        right = next.x + next.width;
    if (next.y + next.height > bottom)
        bottom = next.y + next.height;
    if (next.x < previous.x)
        previous.x = next.x;
    if (next.y < previous.y)
        previous.y = next.y;
    previous.width = right - previous.x;
    previous.height = bottom - previous.y;
    return previous;
}
static uint64_t scanout_load_pair(const uint8_t *pixels) {
    uint64_t value;
    __builtin_memcpy(&value, pixels, sizeof(value));
    return value;
}
static uint32_t scanout_load_pixel(const uint8_t *pixels) {
    uint32_t value;
    __builtin_memcpy(&value, pixels, sizeof(value));
    return value;
}
static struct vgpu_scanout_damage scanout_changed_rect(
    const struct vgpu_scanout_image *image, const struct vgpu_scanout_source *source,
    struct vgpu_scanout_damage region) {
    struct vgpu_scanout_damage changed = {0};
    const uint8_t *source_pixels = (const uint8_t *)source->pixels + source->offset +
        (uint64_t)(source->y + region.y) * source->pitch + (uint64_t)(source->x + region.x) * 4;
    const uint8_t *staged_pixels = (const uint8_t *)image->storage->virt +
        ((uint64_t)region.y * image->width + region.x) * 4;
    for (uint32_t row = 0; row < region.height; row++) {
        const uint8_t *incoming = source_pixels + (uint64_t)row * source->pitch;
        const uint8_t *staged = staged_pixels + (uint64_t)row * image->width * 4;
        uint32_t first = 0, last = region.width;
        while (first + 2 <= last &&
               scanout_load_pair(incoming + (size_t)first * 4) ==
               scanout_load_pair(staged + (size_t)first * 4))
            first += 2;
        if (first < last && scanout_load_pixel(incoming + (size_t)first * 4) ==
                            scanout_load_pixel(staged + (size_t)first * 4))
            first++;
        if (first == last)
            continue;
        while (last - first >= 2 &&
               scanout_load_pair(incoming + (size_t)(last - 2) * 4) ==
               scanout_load_pair(staged + (size_t)(last - 2) * 4))
            last -= 2;
        if (last > first && scanout_load_pixel(incoming + (size_t)(last - 1) * 4) ==
                            scanout_load_pixel(staged + (size_t)(last - 1) * 4))
            last--;
        struct vgpu_scanout_damage line = {region.x + first, region.y + row, last - first, 1};
        changed = changed.width ? damage_union(changed, line) : line;
    }
    return changed;
}
bool vgpu_scanout_copy_damage(struct vgpu_scanout_head *head,
    struct vgpu_scanout_image *image, const struct vgpu_scanout_source *source,
    uint64_t source_id, const struct vgpu_scanout_damage *damage) {
    struct vgpu_scanout_surface *surface = image_surface(head, image);
    if (!surface || !vgpu_scanout_source_valid(source) ||
        image->width != source->view_width || image->height != source->view_height ||
        !image->storage || image->storage->size < (uint64_t)image->width * image->height * 4)
        return false;
    const struct vgpu_scanout_source *previous = &surface->source;
    bool same = surface->source_valid && source_id && surface->source_id == source_id &&
        previous->pixels == source->pixels && previous->size == source->size &&
        previous->offset == source->offset && previous->pitch == source->pitch &&
        previous->width == source->width && previous->height == source->height &&
        previous->x == source->x && previous->y == source->y;
    bool partial = damage && damage->width && damage->height &&
        damage->x < source->view_width && damage->y < source->view_height &&
        damage->width <= source->view_width - damage->x &&
        damage->height <= source->view_height - damage->y;
    struct vgpu_scanout_damage changed = partial ? *damage :
        (struct vgpu_scanout_damage){0, 0, source->view_width, source->view_height};
    for (unsigned int index = 0; index < 2; index++) {
        struct vgpu_scanout_image *other = &surface->images[index];
        if (!same)
            other->pixels_valid = false;
        other->pending = other->pending_valid ? damage_union(other->pending, changed) : changed;
        other->pending_valid = true;
    }
    struct vgpu_scanout_damage upload = image->pixels_valid ? image->pending :
        (struct vgpu_scanout_damage){0, 0, source->view_width, source->view_height};
    image->compared_pixels = image->host_pixels_valid ? (uint64_t)upload.width * upload.height : 0;
    if (image->host_pixels_valid)
        upload = scanout_changed_rect(image, source, upload);
    if (upload.width && upload.height)
        image->host_pixels_valid = false;
    if (upload.width == source->view_width && upload.height == source->view_height) {
        if (!vgpu_scanout_copy(image, source))
            return false;
    } else if (upload.width && upload.height) {
        if (!image->storage || image->storage->size < (uint64_t)image->width * image->height * 4)
            return false;
        const uint8_t *src = (const uint8_t *)source->pixels + source->offset +
            (uint64_t)(source->y + upload.y) * source->pitch + (uint64_t)(source->x + upload.x) * 4;
        size_t pitch = (size_t)image->width * 4;
        uint8_t *dst = (uint8_t *)image->storage->virt + (size_t)upload.y * pitch + (size_t)upload.x * 4;
        bool check = gpu_diag_take_sample();
        size_t row_bytes = (size_t)upload.width * 4;
        uint64_t before = check ? gpu_diag_hash_rect(src, row_bytes, upload.height, source->pitch) : 0;
        for (uint32_t row = 0; row < upload.height; row++)
            memcpy(dst + (size_t)row * pitch, src + (size_t)row * source->pitch,
                   row_bytes);
        gpu_diag_record(GPU_DIAG_COPY_LAYOUT, source->pixels, image->resource_id,
            ((uint64_t)upload.width << 32) | upload.height, source->pitch, source->offset,
            (long)(((uint64_t)(source->x + upload.x) << 32) | (source->y + upload.y)));
        if (check) {
            uint64_t shadow = gpu_diag_hash_rect(dst, row_bytes, upload.height, pitch);
            uint64_t after = gpu_diag_hash_rect(src, row_bytes, upload.height, source->pitch);
            long flags = before != after ? 1 : (before != shadow ? 2 : 0);
            gpu_diag_record(GPU_DIAG_COPY_CHECK, source->pixels, image->resource_id,
                            before, shadow, after, flags);
        }
    }
    image->copied = true;
    image->pixels_valid = true;
    image->pending_valid = false;
    image->damage_tracked = true;
    image->upload = upload;
    surface->source = *source;
    surface->source_id = source_id;
    surface->source_valid = true;
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
    if (!image->damage_tracked)
        s->source_valid = false;
    if (ops->present_batch) {
        struct vgpu_scanout_damage upload = image->damage_tracked ? image->upload :
            (struct vgpu_scanout_damage){0, 0, s->width, s->height};
        bool select = head->selected_id != image->resource_id;
        bool transfer = select && (upload.width || !image->host_pixels_valid);
        int result = ops->present_batch(scanout, image->resource_id, s->width, s->height,
                                         &upload, transfer, select);
        if (result < 0) {
            image->host_pixels_valid = false;
            head->selection_unknown = true;
            goto fail;
        }
        if (result > 0) {
            image->host_pixels_valid = true;
            head->selected_id = image->resource_id;
            if (s == head->pending) {
                retire(head, head->current);
                head->current = s;
                head->pending = NULL;
            }
            goto published;
        }
    }
    /* A failed flush can be retried without touching the visible image.
     * Every other publication fully uploads an inactive host image first. */
    if (head->selected_id != image->resource_id) {
        bool transferred;
        if (image->damage_tracked && !image->upload.width && image->host_pixels_valid) {
            transferred = true;
        } else if (image->damage_tracked && ops->transfer_rect) {
            const struct vgpu_scanout_damage *upload = &image->upload;
            transferred = ops->transfer_rect(image->resource_id, upload->x, upload->y,
                upload->width, upload->height,
                ((uint64_t)upload->y * s->width + upload->x) * 4);
        } else {
            transferred = ops->transfer(image->resource_id, s->width, s->height);
        }
        if (!transferred)
            goto fail;
        image->host_pixels_valid = true;
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
    bool flushed;
    if (image->damage_tracked && image->upload.width && ops->flush_rect) {
        const struct vgpu_scanout_damage *upload = &image->upload;
        flushed = ops->flush_rect(image->resource_id, upload->x, upload->y,
                                  upload->width, upload->height);
    } else {
        flushed = ops->flush(image->resource_id, s->width, s->height);
    }
    if (!flushed)
        goto fail;
published:
    head->published++;
    gpu_diag_record(GPU_DIAG_PRESENT,head,image->resource_id,scanout,head->published,0,0);
    vgpu_scanout_collect(head, ops);
    return true;
fail:
    vgpu_scanout_invalidate(head);
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
