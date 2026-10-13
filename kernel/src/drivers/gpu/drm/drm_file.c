

#include "../../../console/console.h"
#include "../../../console/klog.h"
#include "../../../fb/framebuffer.h"
#include "../../../fs/ramfs.h"
#include "../../../lib/string.h"
#include "../../../mm/heap.h"
#include "../../../mm/vmm.h"
#include "../../../sched/sched.h"
#include "drm.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"

/* ── Forward declarations ────────────────────────────────────────────────── */
extern struct drm_gem_object *ascentdrm_gem_object_create(struct drm_device *dev,
                                                    size_t size);
extern void ascentdrm_gem_object_free(struct drm_device *dev,
                                struct drm_gem_object *obj);
extern int alloc_fd(struct thread *t);

/* ── drm_file lifecycle ──────────────────────────────────────────────────── */

struct drm_file *ascentdrm_file_alloc(struct drm_device *dev) {
  struct drm_file *file = kmalloc(sizeof(struct drm_file));
  if (!file)
    return NULL;

  memset(file, 0, sizeof(struct drm_file));
  file->dev = dev;
  file->next_handle = 1;
  spinlock_init(&file->lock);
  INIT_LIST_HEAD(&file->event_queue);
  wait_queue_init(&file->event_wq);

  spinlock_acquire(&dev->lock);
  if (list_empty(&dev->file_list)) {
    file->is_master = 1;
    klog_debug_puts("[DRM] First client granted Master status\n");
  }
  list_add_tail(&file->list, &dev->file_list);
  spinlock_release(&dev->lock);

  /*
   * Pre-register the hardware framebuffer GEM object (handle 0xF0B0) into
   * this client's handle table under the same handle number.  Legacy
   * userland (tglgears_drm, drm_bench, etc.) uses 0xF0B0 directly.
   */
  struct drm_gem_object *hw_fb = NULL;
  spinlock_acquire(&dev->lock);
  struct drm_gem_object *obj;
  list_for_each_entry(obj, &dev->gem_objects, list) {
    if (obj->handle == 0xF0B0) {
      hw_fb = obj;
      break;
    }
  }
  spinlock_release(&dev->lock);

  if (hw_fb) {
    /*
     * 0xF0B0 = 61616 which exceeds DRM_MAX_HANDLES_PER_FILE (256).
     * We can't slot it directly, so we register it at handle slot 1
     * and also keep a pointer in a dedicated hw_fb field so MAP_DUMB
     * can find it by the legacy handle number via the fallback path.
     */
    spinlock_acquire(&file->lock);
    file->hw_fb_gem = hw_fb;
    ascentdrm_gem_get(hw_fb);
    spinlock_release(&file->lock);
  }

  klog_debug_puts("[DRM] drm_file allocated, client count=");
  uint32_t n = 0;
  struct drm_file *f;
  list_for_each_entry(f, &dev->file_list, list) n++;
  klog_debug_uint64(n);
  klog_debug_puts("\n");

  return file;
}

void ascentdrm_file_free(struct drm_file *file) {
  if (!file)
    return;
  struct drm_device *dev = file->dev;

  /* Release all GEM handles owned by this client. Collect last-refs under
   * the lock and free them after unlock: the refcount itself is atomic, so
   * exactly one closer observes the 1->0 transition even when two files
   * share the object. */
  struct drm_gem_object *to_free[DRM_MAX_HANDLES_PER_FILE];
  uint32_t n_free = 0;
  spinlock_acquire(&file->lock);
  for (uint32_t i = 0; i < DRM_MAX_HANDLES_PER_FILE; i++) {
    if (file->handles[i]) {
      struct drm_gem_object *obj = file->handles[i];
      file->handles[i] = NULL;
      if (ascentdrm_gem_put_last(obj) && n_free < DRM_MAX_HANDLES_PER_FILE)
        to_free[n_free++] = obj;
    }
  }
  /* Release legacy hw FB reference */
  if (file->hw_fb_gem) {
    __atomic_fetch_sub(&file->hw_fb_gem->refcount, 1, __ATOMIC_ACQ_REL);
    /* Never free the hw FB object itself — it's the physical framebuffer */
    file->hw_fb_gem = NULL;
  }

  /* Drain pending events */
  struct drm_pending_event *e, *tmp;
  list_for_each_entry_safe(e, tmp, &file->event_queue, list) {
    list_del(&e->list);
    kfree(e);
  }
  spinlock_release(&file->lock);

  for (uint32_t i = 0; i < n_free; i++)
    ascentdrm_gem_object_free(dev, to_free[i]);

  spinlock_acquire(&dev->lock);
  list_del(&file->list);
  bool last_client = list_empty(&dev->file_list);
  spinlock_release(&dev->lock);

  /* Plymouth quit/kill while its splash resource is scanned out leaves the
   * TTY login running blind (serial shows `AvoryOS login`, QEMU black).
   * Always hand back on last close, not only when kd==GRAPHICS, since the
   * scanout can switch while kd stayed TEXT. Redraw first so GOP has the
   * login prompt, then re-present GOP. Transient closes while already TEXT
   * just re-present the same image. */
  /* Redraw text into the GOP fb first, then re-present it: restore copies
   * GOP -> native scanout, so order matters. */
  if (last_client) {
    ascentdrm_cursor_reset(dev);
    fb_set_kd_mode(0); /* KD_TEXT, redraws console */
    virtio_gpu_restore_console_scanout();
  }

  klog_debug_puts("[DRM] drm_file freed\n");
  kfree(file);
}

/* ── Per-client GEM handle table ─────────────────────────────────────────── */

/*
 * Register a global gem object into this client's handle table.
 * Returns the local handle (1-based), or 0 on failure.
 */
uint32_t ascentdrm_file_gem_register(struct drm_file *file,
                               struct drm_gem_object *obj) {
  spinlock_acquire(&file->lock);

  /* Linux GEM namespaces return the existing handle when the same dma-buf
   * is imported repeatedly into one DRM file. */
  for (uint32_t h = 1; h < DRM_MAX_HANDLES_PER_FILE; h++) {
    if (file->handles[h] == obj) {
      spinlock_release(&file->lock);
      return h;
    }
  }

  /* Reuse released slots instead of permanently exhausting the namespace. */
  uint32_t start = file->next_handle;
  if (start == 0 || start >= DRM_MAX_HANDLES_PER_FILE)
    start = 1;
  uint32_t h = start;
  do {
    if (!file->handles[h]) {
      file->handles[h] = obj;
      ascentdrm_gem_get(obj);
      file->next_handle = h + 1;
      if (file->next_handle >= DRM_MAX_HANDLES_PER_FILE)
        file->next_handle = 1;
      spinlock_release(&file->lock);
      return h;
    }
    h++;
    if (h >= DRM_MAX_HANDLES_PER_FILE)
      h = 1;
  } while (h != start);

  spinlock_release(&file->lock);
  return 0;
}

/*
 * Look up a gem object by handle.
 *
 * Priority:
 *  1. Per-client table (handles 1..DRM_MAX_HANDLES_PER_FILE-1)
 *  2. hw_fb_gem for the legacy hardware FB handle (0xF0B0)
 *  3. Global gem list fallback for any other out-of-range handle
 *     (keeps backward compat with code that uses raw global handles)
 */
struct drm_gem_object *ascentdrm_file_gem_lookup(struct drm_file *file,
                                           uint32_t handle) {
  /* Fast path: per-client table */
  if (handle > 0 && handle < DRM_MAX_HANDLES_PER_FILE) {
    spinlock_acquire(&file->lock);
    struct drm_gem_object *obj = file->handles[handle];
    spinlock_release(&file->lock);
    if (obj)
      return obj;
  }

  /* Legacy hardware FB handle */
  if (handle == 0xF0B0 && file->hw_fb_gem)
    return file->hw_fb_gem;

  /* Global fallback: search the device's gem list by raw handle number.
   * This covers legacy userland that bypasses the per-client table. */
  struct drm_device *dev = file->dev;
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

/*
 * Locked lookup+get: find the object and take a reference while still
 * holding file->lock, so a concurrent close/release cannot free it between
 * the lookup and the refcount increment. Returns NULL with no reference
 * taken when the handle is unknown.
 */
struct drm_gem_object *ascentdrm_file_gem_lookup_get(struct drm_file *file,
                                               uint32_t handle) {
  if (handle > 0 && handle < DRM_MAX_HANDLES_PER_FILE) {
    spinlock_acquire(&file->lock);
    struct drm_gem_object *obj = file->handles[handle];
    if (obj)
      ascentdrm_gem_get(obj);
    spinlock_release(&file->lock);
    if (obj)
      return obj;
  }
  if (handle == 0xF0B0) {
    spinlock_acquire(&file->lock);
    struct drm_gem_object *obj = file->hw_fb_gem;
    if (obj)
      ascentdrm_gem_get(obj);
    spinlock_release(&file->lock);
    if (obj)
      return obj;
  }
  return ascentdrm_file_gem_lookup(file, handle);
}

/*
 * Release a local handle.  Decrements gem refcount; frees if zero.
 */
void ascentdrm_file_gem_release(struct drm_file *file, uint32_t handle) {
  if (handle == 0 || handle >= DRM_MAX_HANDLES_PER_FILE)
    return;
  spinlock_acquire(&file->lock);
  struct drm_gem_object *obj = file->handles[handle];
  if (obj) {
    file->handles[handle] = NULL;
    spinlock_release(&file->lock);
    if (ascentdrm_gem_put_last(obj))
      ascentdrm_gem_object_free(file->dev, obj);
    return;
  }
  spinlock_release(&file->lock);
}

/* ── Per-client event delivery ───────────────────────────────────────────── */

void ascentdrm_file_send_event(struct drm_file *file, struct drm_event_vblank *ev,
                         struct vfs_node *node) {
  struct drm_pending_event *e = kmalloc(sizeof(struct drm_pending_event));
  if (!e)
    return;

  memset(e, 0, sizeof(struct drm_pending_event));
  e->event = *ev;

  spinlock_acquire(&file->lock);
  list_add_tail(&e->list, &file->event_queue);
  spinlock_release(&file->lock);

  wait_queue_wake_all(&file->event_wq);

  extern void epoll_notify_event(struct vfs_node * node, uint32_t events);
  if (node)
    epoll_notify_event(node, 0x0001 /* POLLIN */);
}
