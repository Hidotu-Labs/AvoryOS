/* Native-backed slab/kmalloc implementation for the LinuxKPI slab API.
 * See linuxkpi/include/linux/slab.h for the contract. */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/spinlock.h>

#include <linuxkpi/native_mm.h>

struct kmem_cache {
  void *native;
  const char *name;
  unsigned int size;
};

struct kpi_cache_object {
  struct kpi_cache_object *next;
  const void *object;
  void *native_cache;
  size_t size;
};

#define KPI_CACHE_OBJECT_BUCKETS 4096

static DEFINE_SPINLOCK(kpi_cache_objects_lock);
static struct kpi_cache_object *kpi_cache_objects[KPI_CACHE_OBJECT_BUCKETS];

static size_t kpi_cache_object_bucket(const void *object) {
  return ((uintptr_t)object >> 8) & (KPI_CACHE_OBJECT_BUCKETS - 1);
}

static bool kpi_cache_object_register(const void *object, void *native_cache,
                                      size_t size) {
  struct kpi_cache_object *entry = asc_kmalloc(sizeof(*entry));
  unsigned long flags;
  size_t bucket;

  if (!entry)
    return false;
  entry->object = object;
  entry->native_cache = native_cache;
  entry->size = size;
  bucket = kpi_cache_object_bucket(object);

  spin_lock_irqsave(&kpi_cache_objects_lock, flags);
  entry->next = kpi_cache_objects[bucket];
  kpi_cache_objects[bucket] = entry;
  spin_unlock_irqrestore(&kpi_cache_objects_lock, flags);
  return true;
}

static struct kpi_cache_object *kpi_cache_object_remove(const void *object,
                                                        void *native_cache) {
  struct kpi_cache_object **link;
  struct kpi_cache_object *entry = NULL;
  unsigned long flags;
  size_t bucket = kpi_cache_object_bucket(object);

  spin_lock_irqsave(&kpi_cache_objects_lock, flags);
  for (link = &kpi_cache_objects[bucket]; *link; link = &(*link)->next) {
    if ((*link)->object != object ||
        (native_cache && (*link)->native_cache != native_cache))
      continue;
    entry = *link;
    *link = entry->next;
    break;
  }
  spin_unlock_irqrestore(&kpi_cache_objects_lock, flags);
  return entry;
}

static size_t kpi_cache_object_size(const void *object) {
  struct kpi_cache_object *entry;
  unsigned long flags;
  size_t size = 0;
  size_t bucket = kpi_cache_object_bucket(object);

  spin_lock_irqsave(&kpi_cache_objects_lock, flags);
  for (entry = kpi_cache_objects[bucket]; entry; entry = entry->next) {
    if (entry->object == object) {
      size = entry->size;
      break;
    }
  }
  spin_unlock_irqrestore(&kpi_cache_objects_lock, flags);
  return size;
}

static void *kpi_kmem_cache_alloc_tracked(struct kmem_cache *cache) {
  void *object;

  if (!cache)
    return NULL;
  object = asc_kmem_cache_alloc(cache->native);
  if (object && !kpi_cache_object_register(object, cache->native, cache->size)) {
    asc_kmem_cache_free(cache->native, object);
    return NULL;
  }
  return object;
}

struct slab_sheaf {
  struct kmem_cache *cache;
  unsigned int count, capacity;
  void *objects[];
};

unsigned int kmem_cache_sheaf_size(struct slab_sheaf *sheaf) {
  return sheaf ? sheaf->count : 0;
}

void kmem_cache_return_sheaf(struct kmem_cache *cache, gfp_t gfp,
                            struct slab_sheaf *sheaf) {
  (void)cache;
  (void)gfp;
  if (!sheaf)
    return;
  while (sheaf->count)
    kmem_cache_free(sheaf->cache, sheaf->objects[--sheaf->count]);
  kfree(sheaf);
}

int kmem_cache_refill_sheaf(struct kmem_cache *cache, gfp_t gfp,
                           struct slab_sheaf **sheafp, unsigned int size) {
  struct slab_sheaf *old = *sheafp, *sheaf = old;
  size_t bytes;
  if (old && old->cache != cache)
    return -EINVAL;
  if (old && old->count >= size)
    return 0;
  if (!old || old->capacity < size) {
    if (__builtin_mul_overflow((size_t)size, sizeof(void *), &bytes) ||
        __builtin_add_overflow(bytes, sizeof(*sheaf), &bytes))
      return -ENOMEM;
    sheaf = kmalloc(bytes, gfp);
    if (!sheaf)
      return -ENOMEM;
    sheaf->cache = cache;
    sheaf->capacity = size;
    sheaf->count = old ? old->count : 0;
    if (old) {
      memcpy(sheaf->objects, old->objects, old->count * sizeof(void *));
      kfree(old);
    }
    *sheafp = sheaf;
  }
  while (sheaf->count < size) {
    void *object = kmem_cache_alloc(cache, gfp);
    if (!object)
      return -ENOMEM;
    sheaf->objects[sheaf->count++] = object;
  }
  return 0;
}

struct slab_sheaf *kmem_cache_prefill_sheaf(struct kmem_cache *cache,
                                          gfp_t gfp, unsigned int size) {
  struct slab_sheaf *sheaf = NULL;
  if (kmem_cache_refill_sheaf(cache, gfp, &sheaf, size)) {
    kmem_cache_return_sheaf(cache, gfp, sheaf);
    return NULL;
  }
  return sheaf;
}

void *kmem_cache_alloc_from_sheaf(struct kmem_cache *cache, gfp_t gfp,
                                 struct slab_sheaf *sheaf) {
  if (sheaf && sheaf->cache == cache && sheaf->count)
    return sheaf->objects[--sheaf->count];
  return kmem_cache_alloc(cache, gfp);
}

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
  struct kpi_cache_object *entry;

  if (ZERO_OR_NULL_PTR(ptr))
    return;
  entry = kpi_cache_object_remove(ptr, NULL);
  if (entry) {
    asc_kmem_cache_free(entry->native_cache, (void *)ptr);
    asc_kfree(entry);
    return;
  }
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
  size_t size;

  if (ZERO_OR_NULL_PTR(ptr))
    return 0;
  size = kpi_cache_object_size(ptr);
  if (size)
    return size;
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

void *kmemdup_noprof(const void *src, size_t len, gfp_t flags) {
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
  void *obj = kpi_kmem_cache_alloc_tracked(s);
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
  void *obj = kpi_kmem_cache_alloc_tracked(s);
  if (obj)
    __builtin_memset(obj, 0, s->size);
  return obj;
}

void __kpi_kmem_cache_free(struct kmem_cache *s, void *obj) {
  struct kpi_cache_object *entry;

  if (!s || !obj)
    return;
  entry = kpi_cache_object_remove(obj, s->native);
  asc_kmem_cache_free(s->native, obj);
  if (entry)
    asc_kfree(entry);
}

/* Bulk allocation is all-or-nothing, matching the Linux slab contract. */
int kmem_cache_alloc_bulk(struct kmem_cache *cache, gfp_t flags,
                           size_t count, void **objects) {
  size_t i;
  if (!cache || (!objects && count) || count > (size_t)__INT_MAX__)
    return 0;
  for (i = 0; i < count; i++) {
    objects[i] = __kpi_kmem_cache_alloc(cache, flags);
    if (!objects[i]) {
      while (i) {
        --i;
        __kpi_kmem_cache_free(cache, objects[i]);
        objects[i] = NULL;
      }
      return 0;
    }
  }
  return (int)count;
}
void kmem_cache_free_bulk(struct kmem_cache *cache, size_t count, void **objects) {
  for (size_t i = 0; i < count; i++) {
    if (cache)
      __kpi_kmem_cache_free(cache, objects[i]);
    else
      kfree(objects[i]);
  }
}

char *kstrndup(const char *src, size_t max, gfp_t flags) {
  if (!src)
    return NULL;
  size_t length = strnlen(src, max);
  if (length == (size_t)-1)
    return NULL;
  char *copy = kmalloc(length + 1, flags);
  if (copy) {
    memcpy(copy, src, length);
    copy[length] = '\0';
  }
  return copy;
}
