/* Native-backed slab/kmalloc implementation for the LinuxKPI slab API.
 * See linuxkpi/include/linux/slab.h for the contract. */

#include <linux/slab.h>

#include <linuxkpi/native_mm.h>

struct kmem_cache {
  void *native;
  const char *name;
  unsigned int size;
};

/* ── kmalloc family ─────────────────────────────────────────────────────── */

void *__kpi_kmalloc(size_t size, gfp_t flags) {
  if (size == 0)
    return ZERO_SIZE_PTR;

  void *p = asc_kmalloc(size);
  if (p && (flags & __GFP_ZERO))
    __builtin_memset(p, 0, size);
  return p;
}

void *__kpi_kcalloc(size_t num, size_t size, gfp_t flags) {
  size_t bytes;

  if (__builtin_mul_overflow(num, size, &bytes))
    return NULL;
  if (bytes == 0)
    return ZERO_SIZE_PTR;

  void *p = asc_kmalloc(bytes);
  if (p)
    __builtin_memset(p, 0, bytes);
  return p;
}

void *__kpi_krealloc(const void *ptr, size_t new_size, gfp_t flags) {
  if (!ptr) {
    if (new_size == 0)
      return ZERO_SIZE_PTR;
    return __kpi_kmalloc(new_size, flags);
  }
  if (ZERO_OR_NULL_PTR(ptr)) {
    if (new_size == 0)
      return ZERO_SIZE_PTR;
    return __kpi_kmalloc(new_size, flags);
  }
  if (new_size == 0) {
    __kpi_kfree(ptr);
    return ZERO_SIZE_PTR;
  }

  /* Note: the native realloc only grows and preserves contents; it never
   * shrinks.  __GFP_ZERO on growth is not honored yet (the old size is not
   * tracked), so callers that rely on zeroed expansion must kzalloc. */
  return asc_krealloc((void *)ptr, new_size);
}

void __kpi_kfree(const void *ptr) {
  if (ZERO_OR_NULL_PTR(ptr))
    return;
  asc_kfree((void *)ptr);
}

void __kpi_kfree_sensitive(const void *ptr) {
  if (ZERO_OR_NULL_PTR(ptr))
    return;
  /* Wipe the whole usable block, as upstream, then release it. */
  __builtin_memset((void *)ptr, 0, __kpi_ksize(ptr));
  __kpi_kfree(ptr);
}

size_t __kpi_ksize(const void *ptr) {
  return asc_heap_ksize(ptr);
}

void *__kpi_kmemdup(const void *src, size_t len, gfp_t flags) {
  void *p = __kpi_kmalloc(len, flags);
  if (p && p != ZERO_SIZE_PTR)
    __builtin_memcpy(p, src, len);
  return p;
}

/* Upstream declares this in <linux/string.h>; it is a real exported symbol
 * here so both declarations agree. */
void *kmemdup(const void *src, size_t len, gfp_t flags) {
  return __kpi_kmemdup(src, len, flags);
}

/* ── kmem_cache ─────────────────────────────────────────────────────────── */

struct kmem_cache *__kpi_kmem_cache_create(const char *name, unsigned int size,
                                           unsigned int align,
                                           slab_flags_t flags,
                                           void (*ctor)(void *)) {
  (void)flags;

  struct kmem_cache *c = asc_kmalloc(sizeof(*c));
  if (!c)
    return NULL;

  c->native = asc_kmem_cache_create(name, size, align ? align : 8, ctor, NULL);
  if (!c->native) {
    asc_kfree(c);
    return NULL;
  }

  c->name = name;
  c->size = size;
  return c;
}

struct kmem_cache *__kpi_kmem_cache_create_usercopy(
    const char *name, unsigned int size, unsigned int align, slab_flags_t flags,
    unsigned int useroffset, unsigned int usersize, void (*ctor)(void *)) {
  (void)useroffset;
  (void)usersize;
  return __kpi_kmem_cache_create(name, size, align, flags, ctor);
}

void __kpi_kmem_cache_destroy(struct kmem_cache *s) {
  if (!s)
    return;
  asc_kmem_cache_destroy(s->native);
  asc_kfree(s);
}

int __kpi_kmem_cache_shrink(struct kmem_cache *s) {
  if (!s)
    return 0;
  asc_kmem_cache_shrink(s->native);
  return 0;
}

void *__kpi_kmem_cache_alloc(struct kmem_cache *s, gfp_t flags) {
  if (!s)
    return NULL;
  void *obj = asc_kmem_cache_alloc(s->native);
  if (obj && (flags & __GFP_ZERO))
    __builtin_memset(obj, 0, s->size);
  return obj;
}

void *__kpi_kmem_cache_alloc_node(struct kmem_cache *s, gfp_t flags, int node) {
  (void)node;
  return __kpi_kmem_cache_alloc(s, flags);
}

void *__kpi_kmem_cache_zalloc(struct kmem_cache *s, gfp_t flags) {
  (void)flags;
  if (!s)
    return NULL;
  void *obj = asc_kmem_cache_alloc(s->native);
  if (obj)
    __builtin_memset(obj, 0, s->size);
  return obj;
}

void __kpi_kmem_cache_free(struct kmem_cache *s, void *obj) {
  if (!s || !obj)
    return;
  asc_kmem_cache_free(s->native, obj);
}
