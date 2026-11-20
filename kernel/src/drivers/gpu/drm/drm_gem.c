#include "drm.h"
#include "../../../mm/pmm.h"
#include "../../../mm/heap.h"
#include "../../../lib/string.h"
#include "../../../console/klog.h"
#include "../../../fs/vfs.h"

/* Same recovery heap_big_alloc() uses: shrink slab, buffer and file caches so
 * a large contiguous request has a chance after fragmentation. */
extern void heap_oom_reclaim(void);

uint64_t ascentdrm_live_gem_objects;
uint64_t ascentdrm_live_gem_bytes;
uint64_t ascentdrm_live_virgl_resources;

static uint64_t gem_sg_page_phys(struct drm_gem_object *obj, uint32_t page) {
    if (!obj || !obj->sg_pages || (size_t)page >= obj->size / 4096)
        return 0;
    return obj->sg_pages[page];
}

static struct drm_gem_object *gem_object_create_impl(struct drm_device *dev,
                                                     size_t size,
                                                     bool allow_sg) {
    struct drm_gem_object *obj = kmalloc(sizeof(struct drm_gem_object));
    if (!obj) return NULL;

    memset(obj, 0, sizeof(struct drm_gem_object));
    obj->dev = dev;

    // Round size up to page boundary
    size = (size + 0xFFF) & ~0xFFFULL;
    uint32_t pages = size / 4096;
    if (pages == 0) {
        kfree(obj);
        return NULL;
    }

    void *phys = pmm_alloc_blocks(pages);
    if (!phys) {
        heap_oom_reclaim();
        phys = pmm_alloc_blocks(pages);
    }
    /* Large buffers need one physically contiguous run when scatter-gather is
     * not allowed, so reclaim page/dentry caches before falling back or failing. */
    for (uint32_t attempt = 1; !phys && attempt <= (allow_sg ? 1 : 3); attempt++) {
        size_t target = (size_t)pages * attempt;
        mm_file_cache_reclaim(target);
        vfs_dentry_reclaim_pages(target);
        heap_oom_reclaim();
        phys = pmm_alloc_blocks(pages);
    }

    if (phys) {
        obj->size = size;
        obj->phys_addr = (uint64_t)phys;
        obj->virt_addr = (void *)((uint64_t)phys + pmm_get_hhdm_offset());
        memset(obj->virt_addr, 0, size);
    } else if (allow_sg && pages > 1) {
        uint64_t *sg = kmalloc((size_t)pages * sizeof(uint64_t));
        if (!sg) {
            kfree(obj);
            return NULL;
        }
        uint64_t hhdm = pmm_get_hhdm_offset();
        uint32_t allocated = 0;
        for (; allocated < pages; allocated++) {
            void *p = pmm_alloc_page();
            if (!p) {
                mm_file_cache_reclaim(pages - allocated);
                heap_oom_reclaim();
                p = pmm_alloc_page();
            }
            if (!p)
                break;
            sg[allocated] = (uint64_t)p;
            memset((void *)((uint64_t)p + hhdm), 0, 4096);
        }
        if (allocated < pages) {
            for (uint32_t i = 0; i < allocated; i++)
                pmm_free_page((void *)sg[i]);
            kfree(sg);
            klogf("[DRM] GEM alloc failed: %llu KiB (sg)\n",
                  (unsigned long long)(size >> 10));
            kfree(obj);
            return NULL;
        }
        obj->size = size;
        obj->sg_pages = sg;
        obj->phys_addr = sg[0];
        obj->virt_addr = NULL;
        obj->get_page_phys = gem_sg_page_phys;
    } else {
        klogf("[DRM] GEM alloc failed: %llu KiB contiguous\n",
              (unsigned long long)(size >> 10));
        kfree(obj);
        return NULL;
    }

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

struct drm_gem_object *ascentdrm_gem_object_create(struct drm_device *dev, size_t size) {
    return gem_object_create_impl(dev, size, false);
}

struct drm_gem_object *ascentdrm_gem_object_create_sg(struct drm_device *dev, size_t size) {
    return gem_object_create_impl(dev, size, true);
}

void ascentdrm_gem_object_free(struct drm_device *dev, struct drm_gem_object *obj) {
    if (!obj) return;
    spinlock_acquire(&dev->lock);
    list_del(&obj->list);
    spinlock_release(&dev->lock);
    if (obj->free) { obj->free(dev, obj); return; }
    ascentdrm_gem_release_storage(obj);
}

struct drm_gem_object *ascentdrm_gem_find_by_handle(struct drm_device *dev, uint32_t handle) {
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

/* Find by global handle and take a reference under dev->lock, so the object
 * cannot be freed between the lookup and the get. Returns NULL with no
 * reference taken when unknown. */
struct drm_gem_object *ascentdrm_gem_find_by_handle_get(struct drm_device *dev,
                                                  uint32_t handle) {
    spinlock_acquire(&dev->lock);
    struct drm_gem_object *obj;
    list_for_each_entry(obj, &dev->gem_objects, list) {
        if (obj->handle == handle) {
            __atomic_fetch_add(&obj->refcount, 1, __ATOMIC_RELAXED);
            spinlock_release(&dev->lock);
            return obj;
        }
    }
    spinlock_release(&dev->lock);
    return NULL;
}
