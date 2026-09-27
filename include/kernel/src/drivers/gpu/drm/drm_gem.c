#include "drm.h"
#include "../../../mm/pmm.h"
#include "../../../mm/heap.h"
#include "../../../lib/string.h"

struct drm_gem_object *drm_gem_object_create(struct drm_device *dev, size_t size) {
    struct drm_gem_object *obj = kmalloc(sizeof(struct drm_gem_object));
    if (!obj) return NULL;

    memset(obj, 0, sizeof(struct drm_gem_object));
    obj->dev = dev;
    
    // Round size up to page boundary
    size = (size + 0xFFF) & ~0xFFFULL;
    uint32_t pages = size / 4096;

    void *phys = pmm_alloc_blocks(pages);
    if (!phys) {
        kfree(obj);
        return NULL;
    }

    obj->size = size;
    obj->phys_addr = (uint64_t)phys;
    obj->virt_addr = (void *)((uint64_t)phys + pmm_get_hhdm_offset());
    obj->cache_mode = DRM_GEM_CACHE_WB;
    obj->refcount = 1;

    spinlock_acquire(&dev->lock);
    obj->handle = dev->next_gem_handle++;
    list_add_tail(&obj->list, &dev->gem_objects);
    spinlock_release(&dev->lock);

    return obj;
}

void drm_gem_object_free(struct drm_device *dev, struct drm_gem_object *obj) {
    if (!obj) return;
    spinlock_acquire(&dev->lock);
    list_del(&obj->list);
    spinlock_release(&dev->lock);
    if (obj->free) { obj->free(dev, obj); return; }
    pmm_free_blocks((void *)obj->phys_addr, obj->size / 4096);
    kfree(obj);
}

struct drm_gem_object *drm_gem_find_by_handle(struct drm_device *dev, uint32_t handle) {
    spinlock_acquire(&dev->lock);
    struct drm_gem_object *obj;
    list_for_each_entry(obj, &dev->gem_objects, list) {
        if (obj->handle == handle) {
            spinlock_release(&dev->lock);
            return obj;
        }
    }
    spinlock_release(&dev->lock);
    return NULL;
}
