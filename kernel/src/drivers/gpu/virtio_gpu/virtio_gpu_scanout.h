#ifndef VIRTIO_GPU_SCANOUT_H
#define VIRTIO_GPU_SCANOUT_H
#include "mm/dma_alloc.h"
#include <stdbool.h>
#include <stdint.h>

/* All calls are made under the native task-context presentation gate.
 * The transport must retain request/response ownership after a timeout;
 * ready() is true only once those descriptors have returned. */
struct vgpu_scanout_ops {
    bool (*ready)(void);
    uint32_t (*resource_id)(void);
    bool (*create)(uint32_t id, uint32_t width, uint32_t height);
    bool (*attach)(uint32_t id, uint64_t physical, uint32_t bytes);
    bool (*destroy)(uint32_t id, bool counted);
    bool (*transfer)(uint32_t id, uint32_t width, uint32_t height);
    bool (*select)(uint32_t head, uint32_t id, uint32_t width, uint32_t height);
    bool (*flush)(uint32_t id, uint32_t width, uint32_t height);
};
struct vgpu_scanout_image {
    dma_buffer_t *storage;
    uint32_t resource_id;
    uint32_t width, height;
    bool counted, copied;
};
struct vgpu_scanout_surface {
    uint32_t width, height;
    struct vgpu_scanout_image images[2];
    struct vgpu_scanout_surface *next;
};
struct vgpu_scanout_head {
    struct vgpu_scanout_surface *current, *pending, *retired;
    uint32_t selected_id;
    bool selection_unknown;
    uint64_t published, failures;
};
struct vgpu_scanout_source {
    const void *pixels;
    uint64_t size, offset;
    uint32_t width, height, pitch, x, y, view_width, view_height;
};

bool vgpu_scanout_source_valid(const struct vgpu_scanout_source *source);
struct vgpu_scanout_image *vgpu_scanout_prepare(struct vgpu_scanout_head *head,
    const struct vgpu_scanout_ops *ops, uint32_t scanout, uint32_t width, uint32_t height);
/* Caller holds the source CPU reservation while copying, and releases it
 * before publish. Producer GEMs never provide host DMA backing. */
bool vgpu_scanout_copy(struct vgpu_scanout_image *image,
                       const struct vgpu_scanout_source *source);
bool vgpu_scanout_publish(struct vgpu_scanout_head *head,
    const struct vgpu_scanout_ops *ops, uint32_t scanout, struct vgpu_scanout_image *image);
bool vgpu_scanout_detach(struct vgpu_scanout_head *head,
                         const struct vgpu_scanout_ops *ops, uint32_t scanout);
void vgpu_scanout_collect(struct vgpu_scanout_head *head, const struct vgpu_scanout_ops *ops);
#endif
