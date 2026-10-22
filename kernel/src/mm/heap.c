#include "mm/heap.h"
#include "mm/heap_internal.h"
#include "console/console.h"
#include "console/klog.h"
#include "lib/string.h"

bool heap_initialized = false;

void heap_init(void) {
  heap_slab_init();
  heap_big_init();
  heap_initialized = true;
}

void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;

  if (size <= 1024) {
    void *ptr = heap_slab_alloc(size);
    if (ptr)
      return ptr;
  }

  return heap_big_alloc(size);
}

void kfree(void *ptr) {
  if (!ptr)
    return;

  heap_trace_record(ptr, __builtin_return_address(0));

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

  console_puts("[WARN] kfree: Fatal validation failure. Invalid pointer space.\n");
  klog_puts("  ptr=");
  klog_uint64((uint64_t)ptr);
  klog_puts(" page_base=");
  klog_uint64(page_base);
  klog_puts(" magic=");
  klog_uint64(magic_check);
  klog_puts(" caller=");
  klog_hex64((uint64_t)__builtin_return_address(0));
  klog_puts("\n");
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
