#ifndef __AVORY_LINUXKPI_SLAB_H
#define __AVORY_LINUXKPI_SLAB_H

/* Native-backed Linux <linux/slab.h> overlay.
 *
 * Allocating through AvoryOS's heap/slab subsystem keeps one allocator in the
 * kernel and lets imported code use the exact Linux API.  The real upstream
 * header is not usable here because it builds on SLUB/SLAB internals and
 * kasan; this overlay provides the API surface imported code actually calls,
 * and grows as needed.  Implementations: linuxkpi/src/slab.c.
 */

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/compiler.h>

#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(x)                                                   \
  ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

/* Kmalloc alignment for DMA-safe buffers.  x86 caches are incoherent for DMA
 * below this anyway; matches upstream x86's 128 on most models. */
#ifndef ARCH_KMALLOC_MINALIGN
#define ARCH_KMALLOC_MINALIGN 8
#endif
#ifndef ARCH_DMA_MINALIGN
#define ARCH_DMA_MINALIGN 128
#endif

/* Slab flags used by imported code.  The native cache ignores them; the
 * values mirror upstream's so bit tests in callers behave. */
#define SLAB_HWCACHE_ALIGN ((slab_flags_t __force)0x00002000U)
#define SLAB_PANIC ((slab_flags_t __force)0x00040000U)
#define SLAB_TYPESAFE_BY_RCU ((slab_flags_t __force)0x00080000U)
#define SLAB_RECLAIM_ACCOUNT ((slab_flags_t __force)0x00020000U)
#define SLAB_ACCOUNT ((slab_flags_t __force)0x04000000U)

struct kmem_cache;
struct list_lru;

/* Out-of-line core (linuxkpi/src/slab.c). */
void *__kpi_kmalloc(size_t size, gfp_t flags);
void *__kpi_kcalloc(size_t num, size_t size, gfp_t flags);
void *__kpi_krealloc(const void *ptr, size_t new_size, gfp_t flags);
void __kpi_kfree(const void *ptr);
void __kpi_kfree_sensitive(const void *ptr);
size_t __kpi_ksize(const void *ptr);
void *__kpi_kmemdup(const void *src, size_t len, gfp_t flags);

struct kmem_cache *__kpi_kmem_cache_create(const char *name, unsigned int size,
                                           unsigned int align,
                                           slab_flags_t flags,
                                           void (*ctor)(void *));
struct kmem_cache *__kpi_kmem_cache_create_usercopy(
    const char *name, unsigned int size, unsigned int align, slab_flags_t flags,
    unsigned int useroffset, unsigned int usersize, void (*ctor)(void *));
void __kpi_kmem_cache_destroy(struct kmem_cache *s);
int __kpi_kmem_cache_shrink(struct kmem_cache *s);
void *__kpi_kmem_cache_alloc(struct kmem_cache *s, gfp_t flags);
void *__kpi_kmem_cache_alloc_node(struct kmem_cache *s, gfp_t flags, int node);
void *__kpi_kmem_cache_zalloc(struct kmem_cache *s, gfp_t flags);
void __kpi_kmem_cache_free(struct kmem_cache *s, void *obj);

/* ── kmalloc family ─────────────────────────────────────────────────────── */

static inline void *kmalloc(size_t size, gfp_t flags) {
  return __kpi_kmalloc(size, flags);
}

static inline void *kmalloc_node(size_t size, gfp_t flags, int node) {
  (void)node;
  return __kpi_kmalloc(size, flags);
}

static inline void *kmalloc_node_track_caller(size_t size, gfp_t flags,
                                              int node) {
  (void)node;
  return __kpi_kmalloc(size, flags);
}

static inline void *kzalloc(size_t size, gfp_t flags) {
  return __kpi_kmalloc(size, flags | __GFP_ZERO);
}

static inline void *kzalloc_node(size_t size, gfp_t flags, int node) {
  (void)node;
  return __kpi_kmalloc(size, flags | __GFP_ZERO);
}

static inline void *kcalloc(size_t num, size_t size, gfp_t flags) {
  return __kpi_kcalloc(num, size, flags);
}

static inline void *kcalloc_node(size_t num, size_t size, gfp_t flags,
                                 int node) {
  (void)node;
  return __kpi_kcalloc(num, size, flags);
}

static inline void *kmalloc_array(size_t num, size_t size, gfp_t flags) {
  size_t bytes;
  if (__builtin_mul_overflow(num, size, &bytes))
    return NULL;
  return __kpi_kmalloc(bytes, flags);
}

static inline void *kmalloc_array_node(size_t num, size_t size, gfp_t flags,
                                       int node) {
  (void)node;
  return kmalloc_array(num, size, flags);
}

static inline void *krealloc(const void *ptr, size_t new_size, gfp_t flags) {
  return __kpi_krealloc(ptr, new_size, flags);
}

static inline void *krealloc_array(const void *ptr, size_t num, size_t size,
                                   gfp_t flags) {
  size_t bytes;
  if (__builtin_mul_overflow(num, size, &bytes))
    return NULL;
  return __kpi_krealloc(ptr, bytes, flags);
}

static inline void kfree(const void *ptr) { __kpi_kfree(ptr); }
static inline void kfree_sensitive(const void *ptr) {
  __kpi_kfree_sensitive(ptr);
}
static inline size_t ksize(const void *ptr) { return __kpi_ksize(ptr); }

/* Declared here as well as in <linux/string.h> (which defines it upstream);
 * implemented in linuxkpi/src/slab.c. */
void *kmemdup(const void *src, size_t len, gfp_t flags);

/* Rounding is informational: the native heap already serves >= 64-byte
 * granularity for small objects and exact frames for large ones. */
static inline size_t kmalloc_size_roundup(size_t size) { return size; }

/* Direct __kmalloc-style entry points used by some callers. */
static inline void *__kmalloc(size_t size, gfp_t flags) {
  return __kpi_kmalloc(size, flags);
}
static inline void *__kmalloc_node(size_t size, gfp_t flags, int node) {
  (void)node;
  return __kpi_kmalloc(size, flags);
}

/* ── kmem_cache ─────────────────────────────────────────────────────────── */

static inline struct kmem_cache *kmem_cache_create(const char *name,
                                                   unsigned int size,
                                                   unsigned int align,
                                                   slab_flags_t flags,
                                                   void (*ctor)(void *)) {
  return __kpi_kmem_cache_create(name, size, align, flags, ctor);
}

static inline struct kmem_cache *kmem_cache_create_usercopy(
    const char *name, unsigned int size, unsigned int align, slab_flags_t flags,
    unsigned int useroffset, unsigned int usersize, void (*ctor)(void *)) {
  return __kpi_kmem_cache_create_usercopy(name, size, align, flags, useroffset,
                                          usersize, ctor);
}

static inline void kmem_cache_destroy(struct kmem_cache *s) {
  __kpi_kmem_cache_destroy(s);
}

static inline int kmem_cache_shrink(struct kmem_cache *s) {
  return __kpi_kmem_cache_shrink(s);
}

static inline void *kmem_cache_alloc(struct kmem_cache *s, gfp_t flags) {
  return __kpi_kmem_cache_alloc(s, flags);
}

static inline void *kmem_cache_alloc_node(struct kmem_cache *s, gfp_t flags,
                                          int node) {
  return __kpi_kmem_cache_alloc_node(s, flags, node);
}

static inline void *kmem_cache_alloc_lru(struct kmem_cache *s,
                                         struct list_lru *lru, gfp_t flags) {
  (void)lru;
  return __kpi_kmem_cache_alloc(s, flags);
}

static inline void *kmem_cache_zalloc(struct kmem_cache *s, gfp_t flags) {
  return __kpi_kmem_cache_zalloc(s, flags);
}

static inline void kmem_cache_free(struct kmem_cache *s, void *obj) {
  __kpi_kmem_cache_free(s, obj);
}

#define KMEM_CACHE(__struct, __flags)                                         \
  kmem_cache_create(#__struct, sizeof(struct __struct),                       \
                    __alignof__(struct __struct), (__flags), NULL)

#define KMEM_CACHE_USERCOPY(__struct, __flags, __field)                       \
  kmem_cache_create_usercopy(#__struct, sizeof(struct __struct),              \
                             __alignof__(struct __struct), (__flags),         \
                             offsetof(struct __struct, __field),              \
                             sizeof_field(struct __struct, __field), NULL)

/* ── vmalloc family (implemented in linuxkpi/src/vmalloc.c) ────────────────
 *
 * kvmalloc tries the heap first and falls back to vmalloc for large or
 * fragmented requests, matching Linux semantics; kvfree detects which
 * allocator served the pointer. */
void *kvmalloc_node(size_t size, gfp_t flags, int node);
void *kvmalloc(size_t size, gfp_t flags);
void *kvzalloc(size_t size, gfp_t flags);
void *kvmalloc_array(size_t num, size_t size, gfp_t flags);
void *kvcalloc(size_t num, size_t size, gfp_t flags);
void kvfree(const void *ptr);
void kvfree_sensitive(const void *ptr, size_t len);

#endif /* __AVORY_LINUXKPI_SLAB_H */
