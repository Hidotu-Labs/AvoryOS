#include "drm.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include <linuxkpi/native_prime.h>
#include <linuxkpi/gpu_diag.h>

static uint64_t prime_native_page(void *opaque, unsigned int page) {
  struct drm_gem_object *obj = opaque;
  return obj->get_page_phys ? obj->get_page_phys(obj, page)
                            : obj->phys_addr + (uint64_t)page * 4096;
}

static void prime_native_release(void *opaque) {
  struct drm_gem_object *obj = opaque;
  if (ascentdrm_gem_put_last(obj))
    ascentdrm_gem_object_free(obj->dev, obj);
}

static uint64_t prime_import_page(struct drm_gem_object *obj, uint32_t page) {
#ifdef LINUXKPI_LINUX_IMPORTED
  return linuxkpi_prime_page_phys(obj->prime_mapping, page);
#else
  return 0;
#endif
}

#ifdef LINUXKPI_LINUX_IMPORTED
/* Caller holds dev->lock. A dying object must not be resurrected. */
static struct drm_gem_object *prime_find_import_get(struct drm_device *dev,
                                                   void *buffer) {
  struct drm_gem_object *obj;
  list_for_each_entry(obj, &dev->gem_objects, list) {
    if (obj->prime_buffer != buffer)
      continue;
    int refs = __atomic_load_n(&obj->refcount, __ATOMIC_RELAXED);
    while (refs > 0) {
      if (__atomic_compare_exchange_n(&obj->refcount, &refs, refs + 1, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return obj;
    }
  }
  return NULL;
}
#endif

void ascentdrm_gem_release_storage(struct drm_gem_object *obj) {
#ifdef LINUXKPI_LINUX_IMPORTED
  if (obj->prime_buffer) {
    linuxkpi_prime_unmap(obj->prime_mapping);
    linuxkpi_prime_put(obj->prime_buffer);
    kfree(obj);
    return;
  }
#endif
#ifdef LINUXKPI_LINUX_IMPORTED
  linuxkpi_prime_reservation_free(obj->prime_reservation);
#endif
  pmm_free_blocks((void *)obj->phys_addr, obj->size / 4096);
  kfree(obj);
}

int ascentdrm_prime_export(struct drm_gem_object *obj, unsigned int flags) {
#ifdef LINUXKPI_LINUX_IMPORTED
  if (obj->prime_buffer)
    return linuxkpi_prime_fd(obj->prime_buffer, flags);
  if (obj->cache_mode != DRM_GEM_CACHE_WB)
    return -95; /* EOPNOTSUPP: don't export native MMIO scanout. */
  if (!__atomic_load_n(&obj->prime_reservation, __ATOMIC_ACQUIRE)) {
    void *resv = linuxkpi_prime_reservation_new();
    void *expected = NULL;
    if (!resv)
      return -12;
    if (!__atomic_compare_exchange_n(&obj->prime_reservation, &expected, resv,
                                    false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
      linuxkpi_prime_reservation_free(resv);
  }
  ascentdrm_gem_get(obj);
  return linuxkpi_prime_export(obj, obj->size, obj->prime_reservation, prime_native_page,
                              prime_native_release, flags);
#else
  return -95;
#endif
}

int ascentdrm_prime_import(struct drm_file *file, int fd, uint32_t *handle) {
#ifdef LINUXKPI_LINUX_IMPORTED
  struct drm_device *dev = file->dev;
  struct drm_gem_object *obj, *existing = NULL;
  void *buffer, *mapping, *address;
  size_t size;
  int ret = linuxkpi_prime_get(fd, &buffer, &size);
  if (ret) {
    gpu_diag_prime_error(GPU_PRIME_GET_FD, file, (uint32_t)fd, 0, ret);
    return ret;
  }
  obj = linuxkpi_prime_native_object(buffer);
  if (obj && obj->dev == dev) {
    *handle = ascentdrm_file_gem_register(file, obj);
    if (!*handle)
      gpu_diag_prime_error(GPU_PRIME_HANDLE, file, (uint32_t)fd, size, -12);
    linuxkpi_prime_put(buffer);
    return *handle ? 0 : -12;
  }
  if (!size || size > UINT32_MAX || (size & 4095)) {
    gpu_diag_prime_error(GPU_PRIME_SIZE, file, (uint32_t)fd, size, -22);
    linuxkpi_prime_put(buffer);
    return -22;
  }
  /* Find an existing import without attaching/mapping the same buffer twice.
   * A zero-ref object is awaiting deletion and must not be resurrected. */
  spinlock_acquire(&dev->lock);
  existing = prime_find_import_get(dev, buffer);
  spinlock_release(&dev->lock);
  if (existing) {
    *handle = ascentdrm_file_gem_register(file, existing);
    if (!*handle)
      gpu_diag_prime_error(GPU_PRIME_HANDLE, file, (uint32_t)fd, size, -12);
    prime_native_release(existing);
    linuxkpi_prime_put(buffer);
    return *handle ? 0 : -12;
  }
  ret = linuxkpi_prime_map(buffer, &mapping, &address);
  if (ret) {
    linuxkpi_prime_put(buffer);
    return ret;
  }
  obj = kcalloc(1, sizeof(*obj));
  if (!obj) {
    gpu_diag_prime_error(GPU_PRIME_OBJECT_ALLOC, file, (uint32_t)fd, size, -12);
    linuxkpi_prime_unmap(mapping);
    linuxkpi_prime_put(buffer);
    return -12;
  }
  obj->dev = dev;
  obj->size = size;
  obj->virt_addr = address;
  obj->cache_mode = DRM_GEM_CACHE_WB;
  obj->prime_buffer = buffer;
  obj->prime_mapping = mapping;
  gpu_diag_record(GPU_DIAG_IMPORT,obj,(uintptr_t)buffer,(uintptr_t)mapping,
                  (uintptr_t)address,size,0);
  obj->get_page_phys = prime_import_page;
  obj->refcount = 1;
  spinlock_acquire(&dev->lock);
  /* Attaching can sleep. Another importer may have published this buffer
   * while we were mapping it, so check again before assigning a handle. */
  existing = prime_find_import_get(dev, buffer);
  if (existing) {
    spinlock_release(&dev->lock);
    ascentdrm_gem_release_storage(obj);
    *handle = ascentdrm_file_gem_register(file, existing);
    if (!*handle)
      gpu_diag_prime_error(GPU_PRIME_HANDLE, file, (uint32_t)fd, size, -12);
    prime_native_release(existing);
    return *handle ? 0 : -12;
  }
  obj->handle = dev->next_gem_handle++;
  list_add_tail(&obj->list, &dev->gem_objects);
  spinlock_release(&dev->lock);
  *handle = ascentdrm_file_gem_register(file, obj);
  if (!*handle)
    gpu_diag_prime_error(GPU_PRIME_HANDLE, file, (uint32_t)fd, size, -12);
  prime_native_release(obj);
  return *handle ? 0 : -12;
#else
  return -95;
#endif
}

int ascentdrm_gem_begin_cpu(struct drm_gem_object *obj) {
#ifdef LINUXKPI_LINUX_IMPORTED
  if (obj->prime_buffer)
    return linuxkpi_prime_begin_cpu(obj->prime_buffer, obj->prime_mapping);
  void *resv = __atomic_load_n(&obj->prime_reservation, __ATOMIC_ACQUIRE);
  if (resv)
    return linuxkpi_prime_reservation_wait(resv);
#endif
  return 0;
}

void ascentdrm_gem_end_cpu(struct drm_gem_object *obj) {
#ifdef LINUXKPI_LINUX_IMPORTED
  if (obj->prime_buffer)
    linuxkpi_prime_end_cpu(obj->prime_buffer, obj->prime_mapping);
#endif
}
