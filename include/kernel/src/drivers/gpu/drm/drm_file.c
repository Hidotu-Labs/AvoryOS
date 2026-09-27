

#include "../../../console/klog.h"
#include "../../../fs/ramfs.h"
#include "../../../lib/string.h"
#include "../../../mm/heap.h"
#include "../../../mm/vmm.h"
#include "../../../sched/sched.h"
#include "drm.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"

/* ── Forward declarations ────────────────────────────────────────────────── */
extern struct drm_gem_object *drm_gem_object_create(struct drm_device *dev,
                                                    size_t size);
extern void drm_gem_object_free(struct drm_device *dev,
                                struct drm_gem_object *obj);
extern int alloc_fd(struct thread *t);

/* ── drm_file lifecycle ──────────────────────────────────────────────────── */

struct drm_file *drm_file_alloc(struct drm_device *dev) {
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
    klog_puts("[DRM] First client granted Master status\n");
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
    hw_fb->refcount++;
    spinlock_release(&file->lock);
  }

  klog_puts("[DRM] drm_file allocated, client count=");
  uint32_t n = 0;
  struct drm_file *f;
  list_for_each_entry(f, &dev->file_list, list) n++;
  klog_uint64(n);
  klog_puts("\n");

  return file;
}

void drm_file_free(struct drm_file *file) {
  if (!file)
    return;
  struct drm_device *dev = file->dev;

  /* Release all GEM handles owned by this client */
  spinlock_acquire(&file->lock);
  for (uint32_t i = 0; i < DRM_MAX_HANDLES_PER_FILE; i++) {
    if (file->handles[i]) {
      file->handles[i]->refcount--;
      if (file->handles[i]->refcount <= 0)
        drm_gem_object_free(dev, file->handles[i]);
      file->handles[i] = NULL;
    }
  }
  /* Release legacy hw FB reference */
  if (file->hw_fb_gem) {
    file->hw_fb_gem->refcount--;
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

  spinlock_acquire(&dev->lock);
  list_del(&file->list);
  spinlock_release(&dev->lock);

  klog_puts("[DRM] drm_file freed\n");
  kfree(file);
}

/* ── Per-client GEM handle table ─────────────────────────────────────────── */

/*
 * Register a global gem object into this client's handle table.
 * Returns the local handle (1-based), or 0 on failure.
 */
uint32_t drm_file_gem_register(struct drm_file *file,
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
      obj->refcount++;
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
struct drm_gem_object *drm_file_gem_lookup(struct drm_file *file,
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
 * Release a local handle.  Decrements gem refcount; frees if zero.
 */
void drm_file_gem_release(struct drm_file *file, uint32_t handle) {
  if (handle == 0 || handle >= DRM_MAX_HANDLES_PER_FILE)
    return;
  spinlock_acquire(&file->lock);
  struct drm_gem_object *obj = file->handles[handle];
  if (obj) {
    file->handles[handle] = NULL;
    obj->refcount--;
    if (obj->refcount <= 0) {
      spinlock_release(&file->lock);
      drm_gem_object_free(file->dev, obj);
      return;
    }
  }
  spinlock_release(&file->lock);
}

/* ── Per-client event delivery ───────────────────────────────────────────── */

void drm_file_send_event(struct drm_file *file, struct drm_event_vblank *ev,
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

/* ── PRIME VFS node (anonymous buffer fd) ────────────────────────────────── */

static uint32_t prime_read(struct vfs_node *node, uint32_t offset,
                           uint32_t length, uint8_t *buf) {
  /* Reading a prime fd copies raw pixel data — useful for testing */
  struct drm_gem_object *obj = (struct drm_gem_object *)node->device;
  if (!obj || !obj->virt_addr)
    return 0;
  if (offset >= obj->size)
    return 0;
  if (offset + length > obj->size)
    length = obj->size - offset;
  memcpy(buf, (uint8_t *)obj->virt_addr + offset, length);
  return length;
}

static uint64_t prime_mmap(struct vfs_node *node, uint64_t addr,
                           uint64_t length, uint64_t prot, uint64_t flags,
                           uint64_t offset) {
  (void)prot;
  (void)flags;
  struct drm_gem_object *obj = (struct drm_gem_object *)node->device;
  if (!obj || !addr || !length || (addr & 4095) || (offset & 4095) ||
      offset > obj->size || length > obj->size - offset)
    return (uint64_t)-1;

  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW;
  if (obj->cache_mode == DRM_GEM_CACHE_WC)
    page_flags |= PAGE_FLAG_PWT | PAGE_FLAG_PCD | PAGE_FLAG_PAT;

  uint64_t *pml4 = vmm_get_active_pml4();
  uint32_t pages = (uint32_t)((length + 4095) / 4096);
  for (uint32_t i = 0; i < pages; i++) {
    uint64_t object_offset = offset + (uint64_t)i * 4096;
    uint64_t phys = obj->get_page_phys
        ? obj->get_page_phys(obj, (uint32_t)(object_offset / 4096))
        : obj->phys_addr + object_offset;
    if (!phys || !vmm_map_page(pml4, addr + (uint64_t)i * 4096, phys,
                               page_flags))
      return (uint64_t)-1;
  }
  return addr;
}

static void prime_close(struct vfs_node *node) {
  /* When the prime fd is closed, drop the gem refcount */
  struct drm_gem_object *obj = (struct drm_gem_object *)node->device;
  if (obj) {
    obj->refcount--;
    if (obj->refcount <= 0 && obj->dev)
      drm_gem_object_free(obj->dev, obj);
  }
  klog_puts("[DRM] PRIME fd closed\n");
}

/*
 * drm_prime_export — create an anonymous fd for a gem object.
 * Returns the new fd number, or -1 on failure.
 */
int drm_prime_export(struct drm_gem_object *obj) {
  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return -1;

  vfs_node_t *prime_node = kmalloc(sizeof(vfs_node_t));
  if (!prime_node)
    return -1;

  vfs_node_init(prime_node);
  strcpy(prime_node->name, "prime_buf");
  prime_node->flags = FS_CHARDEV; /* non-persistent: freed on last close */
  prime_node->mask = 0600;
  prime_node->length =
      obj->size > UINT32_MAX ? UINT32_MAX : (uint32_t)obj->size;
  prime_node->device = obj;
  prime_node->read = prime_read;
  prime_node->mmap = prime_mmap;
  prime_node->close = prime_close;
  prime_node->refcount = 1;

  obj->refcount++; /* prime fd holds a reference */

  t->fds[fd] = prime_node;
  t->fd_offsets[fd] = 0;

  klog_puts("[DRM] PRIME exported gem handle=");
  klog_uint64(obj->handle);
  klog_puts(" as fd=");
  klog_uint64(fd);
  klog_puts("\n");

  return fd;
}

/*
 * drm_prime_import — given a prime fd, return the gem object it wraps.
 * Returns NULL if the fd is not a prime node.
 */
struct drm_gem_object *drm_prime_import(int prime_fd) {
  struct thread *t = sched_get_current();
  if (!t || prime_fd < 0 || prime_fd >= MAX_FDS)
    return NULL;

  vfs_node_t *node = t->fds[prime_fd];
  if (!node)
    return NULL;

  /* Identify prime nodes by their read function pointer */
  if (node->read != prime_read)
    return NULL;

  return (struct drm_gem_object *)node->device;
}