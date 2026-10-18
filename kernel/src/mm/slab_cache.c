//  Named Object Slab Cache Allocator — Implementation
//
//  Each kmem_cache manages slabs (single 4KB pages) containing fixed-size
//  objects. Objects within a slab are tracked by a 128-bit bitmap. Slabs
//  transition between three lists:
//
//     free ──▶ partial ──▶ full
//           ◀──         ◀──
//
//  On alloc: pick from partial (O(1) bitmap scan), fallback to free, then
//            allocate a new slab page from PMM.
//  On free:  clear bit, move slab full→partial or partial→free.

#include "slab_cache.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "pmm.h"
#include "vmm.h"

#define SLAB_PAGE_MAGIC 0xCA04E51A8CA04EULL
#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

#define BITMAP_SET(bmp, i) ((bmp)[(i) / 32] |= (1U << ((i) % 32)))
#define BITMAP_CLEAR(bmp, i) ((bmp)[(i) / 32] &= ~(1U << ((i) % 32)))
#define BITMAP_TEST(bmp, i) (((bmp)[(i) / 32] & (1U << ((i) % 32))) != 0)

// Global State

static spinlock_t slab_global_lock = SPINLOCK_INIT;

static kmem_cache_t cache_pool[SLAB_CACHE_MAX_CACHES];
static bool cache_used[SLAB_CACHE_MAX_CACHES];
static int cache_count = 0;

// Pre-built Kernel Object Caches

kmem_cache_t *thread_cache = NULL;
kmem_cache_t *vfs_node_cache = NULL;
kmem_cache_t *vma_cache = NULL;

// Unlink a slab from a doubly-linked list
static void slab_unlink(struct slab_page **head, struct slab_page *s) {
  if (s->prev)
    s->prev->next = s->next;
  else
    *head = s->next;
  if (s->next)
    s->next->prev = s->prev;
  s->next = NULL;
  s->prev = NULL;
}

// Push a slab to the front of a list
static void slab_push(struct slab_page **head, struct slab_page *s) {
  s->next = *head;
  s->prev = NULL;
  if (*head)
    (*head)->prev = s;
  *head = s;
}

// Allocate & initialize a new slab page for a cache
static struct slab_page *slab_page_alloc(kmem_cache_t *cache) {
  void *frame = pmm_alloc_page();
  if (!frame)
    return NULL;

  struct slab_page *s = (struct slab_page *)PHYS_TO_VIRT(frame);
  memset(s, 0, PAGE_SIZE);

  s->magic = SLAB_PAGE_MAGIC;
  s->cache = cache;
  s->total_count = cache->objs_per_slab;
  if (s->total_count > 128)
    s->total_count = 128;
  s->free_count = s->total_count;
  // bitmap is already zero (memset) → all objects free

  cache->total_slabs++;
  return s;
}

// Public API

void slab_cache_init(void) {
  memset(cache_pool, 0, sizeof(cache_pool));
  memset(cache_used, 0, sizeof(cache_used));
  cache_count = 0;

  // Forward-declare sizes — we need the actual struct sizes.
  // We use extern sizeof helpers that each subsystem provides,
  // but for bootstrap we use conservative estimates that get
  // overridden when the subsystem calls kmem_cache_create.

  klog_puts("[SLAB] Named object slab cache subsystem initialized.\n");
}

kmem_cache_t *kmem_cache_create(const char *name, size_t obj_size,
                                size_t alignment, void (*ctor)(void *),
                                void (*dtor)(void *)) {
  if (!name || obj_size == 0)
    return NULL;
  if (alignment == 0)
    alignment = 8;
  // Round obj_size up to alignment
  size_t aligned_size = (obj_size + alignment - 1) & ~(alignment - 1);
  // Minimum 16 bytes per object
  if (aligned_size < 16)
    aligned_size = 16;

  spinlock_acquire(&slab_global_lock);

  // Find a free slot
  int slot = -1;
  for (int i = 0; i < SLAB_CACHE_MAX_CACHES; i++) {
    if (!cache_used[i]) {
      slot = i;
      break;
    }
  }

  if (slot == -1) {
    spinlock_release(&slab_global_lock);
    klog_puts("[SLAB] ERROR: Cache registry full!\n");
    return NULL;
  }

  kmem_cache_t *c = &cache_pool[slot];
  memset(c, 0, sizeof(*c));

  // Copy name
  int i = 0;
  while (name[i] && i < SLAB_CACHE_NAME_MAX - 1) {
    c->name[i] = name[i];
    i++;
  }
  c->name[i] = '\0';

  c->obj_size = obj_size;
  c->obj_aligned = aligned_size;
  c->alignment = alignment;
  c->ctor = ctor;
  c->dtor = dtor;

  // Calculate objects per slab
  size_t usable = PAGE_SIZE - sizeof(struct slab_page);
  c->objs_per_slab = usable / aligned_size;
  if (c->objs_per_slab > 128)
    c->objs_per_slab = 128;
  if (c->objs_per_slab == 0) {
    spinlock_release(&slab_global_lock);
    klog_puts("[SLAB] ERROR: Object too large for single-page slab: ");
    klog_puts(name);
    klog_puts("\n");
    return NULL;
  }

  cache_used[slot] = true;
  cache_count++;

  spinlock_release(&slab_global_lock);

  klog_puts("[SLAB] Created cache '");
  klog_puts(c->name);
  klog_puts("': obj_size=");
  klog_uint64(obj_size);
  klog_puts(" aligned=");
  klog_uint64(aligned_size);
  klog_puts(" per_slab=");
  klog_uint64(c->objs_per_slab);
  klog_puts("\n");

  return c;
}

void kmem_cache_destroy(kmem_cache_t *cache) {
  if (!cache)
    return;

  spinlock_acquire(&slab_global_lock);

  if (cache->active_objects > 0) {
    klog_puts("[SLAB] WARNING: Destroying cache '");
    klog_puts(cache->name);
    klog_puts("' with ");
    klog_uint64(cache->active_objects);
    klog_puts(" active objects!\n");
  }

  // Find slot and mark as unused
  for (int i = 0; i < SLAB_CACHE_MAX_CACHES; i++) {
    if (&cache_pool[i] == cache && cache_used[i]) {
      cache_used[i] = false;
      cache_count--;
      break;
    }
  }

  spinlock_release(&slab_global_lock);
}

void *kmem_cache_alloc(kmem_cache_t *cache) {
  if (!cache)
    return NULL;

  void *ret_ctor_arg = NULL;
  void (*ctor)(void *) = NULL;

  // Bounded retry: first PMM failure shrinks empty slabs elsewhere, then
  // tries exactly once more. Never loops (shrink makes no progress when
  // every cache is fully used).
  for (int attempt = 0; attempt < 2; attempt++) {
    spinlock_acquire(&slab_global_lock);

    // 1. Scan all partial slabs for a free slot. The old head-only lookup
    //    returned NULL when the head was full/corrupt even though another
    //    partial slab still had space.
    struct slab_page *s = NULL;
    int free_idx = -1;
    for (struct slab_page *cand = cache->slabs_partial; cand;
         cand = cand->next) {
      free_idx = -1;
      for (int i = 0; i < (int)cand->total_count; i++) {
        if (!BITMAP_TEST(cand->bitmap, i)) {
          free_idx = i;
          break;
        }
      }
      if (free_idx != -1) {
        s = cand;
        break;
      }
      // Full or corrupt header: try the next partial slab.
    }

    // 2. No space in partials: reuse one slab from the free list, else
    //    allocate a brand new slab page.
    if (!s) {
      if (cache->slabs_free) {
        s = cache->slabs_free;
        slab_unlink(&cache->slabs_free, s);
      } else {
        // 3. Allocate a brand new slab page
        s = slab_page_alloc(cache);
        if (!s) {
          spinlock_release(&slab_global_lock);
          if (attempt == 0) {
            kmem_cache_shrink_all();
            continue;
          }
          return NULL;
        }
      }
      // Move to partial
      slab_push(&cache->slabs_partial, s);

      free_idx = -1;
      for (int i = 0; i < (int)s->total_count; i++) {
        if (!BITMAP_TEST(s->bitmap, i)) {
          free_idx = i;
          break;
        }
      }
      if (free_idx == -1) {
        // Corrupt reused/new slab: park it back on the free list so a
        // later shrink can reclaim it instead of handing out a duplicate.
        slab_unlink(&cache->slabs_partial, s);
        slab_push(&cache->slabs_free, s);
        spinlock_release(&slab_global_lock);
        return NULL;
      }
    }

    BITMAP_SET(s->bitmap, free_idx);
    if (s->free_count > 0)
      s->free_count--;

    // If slab is now full, move from partial to full
    if (s->free_count == 0) {
      slab_unlink(&cache->slabs_partial, s);
      slab_push(&cache->slabs_full, s);
    }

    cache->total_allocs++;
    cache->active_objects++;

    uint8_t *obj_base = (uint8_t *)s + sizeof(struct slab_page);
    ret_ctor_arg = obj_base + (free_idx * cache->obj_aligned);
    ctor = cache->ctor;

    spinlock_release(&slab_global_lock);
    break;
  }

  // Call constructor outside the lock, as before.
  if (ret_ctor_arg && ctor) {
    ctor(ret_ctor_arg);
  }

  return ret_ctor_arg;
}

void kmem_cache_free(kmem_cache_t *cache, void *obj) {
  if (!cache || !obj)
    return;

  spinlock_acquire(&slab_global_lock);

  // Determine which slab page this object belongs to
  uint64_t page_base = (uint64_t)obj & ~0xFFFULL;
  struct slab_page *s = (struct slab_page *)page_base;

  // Validate magic
  if (s->magic != SLAB_PAGE_MAGIC) {
    klog_puts("[SLAB] ERROR: kmem_cache_free invalid magic! ptr=");
    klog_uint64((uint64_t)obj);
    klog_puts("\n");
    spinlock_release(&slab_global_lock);
    return;
  }

  // Validate cache ownership
  if (s->cache != cache) {
    klog_puts("[SLAB] ERROR: Object freed to wrong cache!\n");
    spinlock_release(&slab_global_lock);
    return;
  }

  // Calculate object index
  uint64_t object_base = page_base + sizeof(struct slab_page);
  if ((uint64_t)obj < object_base) {
    klog_puts("[SLAB] ERROR: Object points inside slab metadata!\n");
    spinlock_release(&slab_global_lock);
    return;
  }
  uint64_t offset = (uint64_t)obj - object_base;
  if (offset % cache->obj_aligned != 0) {
    klog_puts("[SLAB] ERROR: Object is not aligned to its cache!\n");
    spinlock_release(&slab_global_lock);
    return;
  }
  uint32_t idx = offset / cache->obj_aligned;

  if (idx >= s->total_count) {
    klog_puts("[SLAB] ERROR: Object index out of range!\n");
    spinlock_release(&slab_global_lock);
    return;
  }

  // Double-free detection
  if (!BITMAP_TEST(s->bitmap, idx)) {
    klog_puts("[SLAB] WARNING: Double free detected in cache '");
    klog_puts(cache->name);
    klog_puts("'!\n");
    spinlock_release(&slab_global_lock);
    return;
  }

  // Call destructor before clearing
  if (cache->dtor) {
    cache->dtor(obj);
  }

  BITMAP_CLEAR(s->bitmap, idx);
  s->free_count++;
  cache->total_frees++;
  cache->active_objects--;

  // State transitions
  if (s->free_count == 1) {
    // Was full → now partial
    slab_unlink(&cache->slabs_full, s);
    slab_push(&cache->slabs_partial, s);
  }

  if (s->free_count == s->total_count) {
    // Completely empty → move to free list
    slab_unlink(&cache->slabs_partial, s);
    slab_push(&cache->slabs_free, s);
  }

  spinlock_release(&slab_global_lock);
}

void kmem_cache_shrink(kmem_cache_t *cache) {
  if (!cache)
    return;

  spinlock_acquire(&slab_global_lock);

  // Release all completely empty slabs
  // NOTE: We do NOT unmap slab pages because kernel heap page tables are
  // shared across all process PML4s. Unmapping would corrupt other address
  // spaces. We simply detach them from the free list and abandon the VA.
  // The physical frame is returned to PMM.
  while (cache->slabs_free) {
    struct slab_page *s = cache->slabs_free;
    slab_unlink(&cache->slabs_free, s);

    uint64_t *pml4 = vmm_get_active_pml4();
    uint64_t phys = vmm_virt_to_phys(pml4, (uint64_t)s);
    if (phys) {
      pmm_free_page((void *)(phys & 0x000FFFFFFFFFF000ULL));
    }
    cache->total_slabs--;
  }

  spinlock_release(&slab_global_lock);
}

// OOM reclaim: release empty slabs from every cache. Used as the single
// bounded retry step by kmem_cache_alloc() (and, via weak symbol, by the
// generic heap in heap.c). Caller must NOT hold slab_global_lock.
void kmem_cache_shrink_all(void) {
  spinlock_acquire(&slab_global_lock);
  for (int i = 0; i < SLAB_CACHE_MAX_CACHES; i++) {
    if (!cache_used[i])
      continue;
    kmem_cache_t *c = &cache_pool[i];
    while (c->slabs_free) {
      struct slab_page *s = c->slabs_free;
      slab_unlink(&c->slabs_free, s);
      uint64_t *pml4 = vmm_get_active_pml4();
      uint64_t phys = vmm_virt_to_phys(pml4, (uint64_t)s);
      if (phys)
        pmm_free_page((void *)(phys & 0x000FFFFFFFFFF000ULL));
      c->total_slabs--;
    }
  }
  spinlock_release(&slab_global_lock);
}

void kmem_cache_print_all(void) {
  spinlock_acquire(&slab_global_lock);

  klog_puts("\n═══ Kernel Object Slab Cache Registry ═══\n");
  klog_puts(
      "Name                  ObjSize  Active   Allocs   Frees    Slabs\n");
  klog_puts(
      "────────────────────  ───────  ───────  ───────  ───────  ─────\n");

  for (int i = 0; i < SLAB_CACHE_MAX_CACHES; i++) {
    if (!cache_used[i])
      continue;
    kmem_cache_t *c = &cache_pool[i];

    klog_puts(c->name);
    // Pad name to 22 chars
    int nlen = 0;
    while (c->name[nlen])
      nlen++;
    for (int p = nlen; p < 22; p++)
      klog_puts(" ");

    klog_uint64(c->obj_size);
    klog_puts("      ");
    klog_uint64(c->active_objects);
    klog_puts("      ");
    klog_uint64(c->total_allocs);
    klog_puts("      ");
    klog_uint64(c->total_frees);
    klog_puts("      ");
    klog_uint64(c->total_slabs);
    klog_puts("\n");
  }
  klog_puts("═══════════════════════════════════════════\n\n");

  spinlock_release(&slab_global_lock);
}

uint64_t kmem_cache_total_active(void) {
  uint64_t total = 0;
  spinlock_acquire(&slab_global_lock);
  for (int i = 0; i < SLAB_CACHE_MAX_CACHES; i++) {
    if (cache_used[i]) {
      total += cache_pool[i].active_objects;
    }
  }
  spinlock_release(&slab_global_lock);
  return total;
}

uint64_t kmem_cache_total_slabs(void) {
  uint64_t total = 0;
  spinlock_acquire(&slab_global_lock);
  for (int i = 0; i < SLAB_CACHE_MAX_CACHES; i++) {
    if (cache_used[i]) {
      total += cache_pool[i].total_slabs;
    }
  }
  spinlock_release(&slab_global_lock);
  return total;
}

uint64_t kmem_cache_free_slabs(void) {
  uint64_t total = 0;
  spinlock_acquire(&slab_global_lock);
  for (int i = 0; i < SLAB_CACHE_MAX_CACHES; i++) {
    if (cache_used[i]) {
      for (struct slab_page *s = cache_pool[i].slabs_free; s; s = s->next)
        total++;
    }
  }
  spinlock_release(&slab_global_lock);
  return total;
}

uint64_t slab_total_pages(void) {
  extern uint64_t heap_slab_total_slabs(void) __attribute__((weak));
  uint64_t total = kmem_cache_total_slabs();
  if (heap_slab_total_slabs)
    total += heap_slab_total_slabs();
  return total;
}

uint64_t slab_reclaimable_pages(void) {
  extern uint64_t heap_slab_free_slabs(void) __attribute__((weak));
  uint64_t total = kmem_cache_free_slabs();
  if (heap_slab_free_slabs)
    total += heap_slab_free_slabs();
  return total;
}
