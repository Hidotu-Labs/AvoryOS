#include "mm/heap.h"
#include "mm/heap_internal.h"
#include "console/klog.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "sched/sched.h"

bool heap_initialized = false;

void heap_init(void) {
  heap_slab_init();
  heap_big_init();
  heap_initialized = true;
}

void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;

  void *ptr = NULL;
  if (size <= 1024) {
    ptr = heap_slab_alloc_at(size, __builtin_return_address(0));
  }
  if (!ptr) {
    ptr = heap_big_alloc(size);
  }
  if (ptr) {
    struct thread *cur = sched_get_current();
    if (cur)
      cur->kheap_alloc_bytes += size;
  }
  return ptr;
}

/* VMAP window (linuxkpi/src/vmalloc.c). Pointers in it come from vmalloc and
 * are never HHDM heap blocks, so they must be checked before the heap probe. */
#define HEAP_VMAP_BASE 0xFFFFC00000000000ULL
#define HEAP_VMAP_END 0xFFFFE00000000000ULL

extern void *vmalloc(unsigned long size);
extern void vfree(const void *addr);

void *kmalloc_fallback(size_t size) {
  void *ptr = kmalloc(size);
  if (ptr || size == 0)
    return ptr;
  return vmalloc((unsigned long)size);
}

void kfree(void *ptr) {
  if (!ptr)
    return;

  void *caller = __builtin_return_address(0);
  heap_trace_record(ptr, caller);

  if ((uint64_t)(uintptr_t)ptr >= HEAP_VMAP_BASE &&
      (uint64_t)(uintptr_t)ptr < HEAP_VMAP_END) {
    vfree(ptr);
    return;
  }

  /* Heap allocations live in managed HHDM RAM. Reject wild pointers before
   * probing their page header, which must never be an arbitrary ring-0 read. */
  if (!pmm_kernel_ptr_is_managed(ptr)) {
    klogf("[ERROR] kfree: rejected pointer outside managed heap space ptr=0x%016llX caller=0x%016llX\n",
          (unsigned long long)(uintptr_t)ptr,
          (unsigned long long)(uintptr_t)caller);
    return;
  }

  uint64_t page_base = (uint64_t)ptr & ~0xFFFULL;
  uint64_t magic_check = *(uint64_t *)page_base;

  if (magic_check == SLAB_MAGIC) {
    heap_slab_free(ptr, page_base);
    return;
  }

  if (magic_check == BIG_MAGIC) {
    heap_big_free(ptr, page_base);
    return;
  }

  klogf("[ERROR] kfree: invalid allocation ptr=0x%016llX page=0x%016llX magic=0x%016llX caller=0x%016llX\n",
        (unsigned long long)(uintptr_t)ptr,
        (unsigned long long)page_base,
        (unsigned long long)magic_check,
        (unsigned long long)(uintptr_t)caller);
}

void *kcalloc(size_t num, size_t size) {
  if (size != 0 && num > SIZE_MAX / size)
    return NULL;
  size_t total = num * size;
  void *ptr = kmalloc(total);
  if (ptr)
    memset(ptr, 0, total);
  return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
  if (!ptr)
    return kmalloc(new_size);
  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  size_t old_size = heap_ksize(ptr);
  if (old_size == 0)
    return NULL;

  if (new_size <= old_size)
    return ptr;

  void *new_ptr = kmalloc(new_size);
  if (new_ptr) {
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
  }
  return new_ptr;
}

size_t heap_ksize(const void *ptr) {
  if (!ptr)
    return 0;

  if (!pmm_kernel_ptr_is_managed(ptr))
    return 0;

  uint64_t page_base = (uint64_t)ptr & ~0xFFFULL;
  uint64_t magic_check = *(uint64_t *)page_base;

  if (magic_check == SLAB_MAGIC)
    return heap_slab_ksize(ptr, page_base);

  if (magic_check == BIG_MAGIC)
    return heap_big_ksize(ptr, page_base);

  return 0;
}

void heap_get_info(char *buf) {
  buf[0] = '\0';
  heap_slab_get_info(buf);
  heap_big_get_info(buf);
}
