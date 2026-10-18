#include "mm/heap_internal.h"
#include "console/console.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "hal/hal.h"

struct slab_cache heap_caches[CACHE_COUNT] = {
    {32,   SPINLOCK_INIT, NULL, NULL, NULL},
    {64,   SPINLOCK_INIT, NULL, NULL, NULL},
    {128,  SPINLOCK_INIT, NULL, NULL, NULL},
    {256,  SPINLOCK_INIT, NULL, NULL, NULL},
    {512,  SPINLOCK_INIT, NULL, NULL, NULL},
    {1024, SPINLOCK_INIT, NULL, NULL, NULL},
};

struct cpu_local_slab heap_cpu_slab_caches[MAX_CPUS][CACHE_COUNT];

void heap_slab_init(void) {
  for (size_t i = 0; i < CACHE_COUNT; i++) {
    spinlock_init(&heap_caches[i].lock);
  }
  memset(heap_cpu_slab_caches, 0, sizeof(heap_cpu_slab_caches));
}

static struct slab *allocate_new_slab(struct slab_cache *c) {
  void *frame = pmm_alloc_page();
  if (!frame)
    return NULL;

  uint64_t vaddr = (uint64_t)frame + pmm_get_hhdm_offset();
  struct slab *s = (struct slab *)vaddr;
  memset(s, 0, PAGE_SIZE);

  s->magic = SLAB_MAGIC;
  s->cache = c;
  s->total_count = (PAGE_SIZE - sizeof(struct slab)) / c->obj_size;
  if (s->total_count > 128)
    s->total_count = 128; // Limit by bitmap capacity
  s->free_count = s->total_count;

  return s;
}

// OOM reclaim: named object caches may hold completely empty slabs while
// this size class needs a fresh page. Provided by slab_cache.c.
extern void kmem_cache_shrink_all(void) __attribute__((weak));
// Optional file-cache hook: fs can provide a global "free up to target
// clean pages" helper. mm must not hard-depend on fs, so weak.
extern size_t mm_file_cache_reclaim(size_t target) __attribute__((weak));
extern size_t thread_stack_cache_reclaim(size_t max_reclaim) __attribute__((weak));
extern size_t ext2_buffer_cache_shrink(size_t max_buffers) __attribute__((weak));
extern size_t pty_shrink_unused_buffers(void) __attribute__((weak));

// Return every completely empty generic slab to PMM. Caller must NOT hold
// any heap_caches[].lock (we take each one in turn). Returns pages freed.
static size_t heap_shrink_empty(void) {
  size_t freed = 0;
  uint64_t hhdm = pmm_get_hhdm_offset();
  for (size_t i = 0; i < CACHE_COUNT; i++) {
    struct slab_cache *c = &heap_caches[i];
    spinlock_acquire(&c->lock);
    while (c->free) {
      struct slab *s = c->free;
      c->free = s->next;
      if (c->free)
        c->free->prev = NULL;
      uint64_t phys = (uint64_t)s - hhdm;
      pmm_free_page((void *)phys);
      freed++;
    }
    spinlock_release(&c->lock);
  }
  return freed;
}

// Single bounded reclaim pass run after a first PMM failure, before one
// retry. Never loops: caller retries exactly once.
void heap_oom_reclaim(void) {
  if (kmem_cache_shrink_all)
    kmem_cache_shrink_all();
  heap_shrink_empty();
  if (thread_stack_cache_reclaim)
    thread_stack_cache_reclaim(16);
  if (ext2_buffer_cache_shrink)
    ext2_buffer_cache_shrink(32);
  if (pty_shrink_unused_buffers)
    pty_shrink_unused_buffers();
  if (mm_file_cache_reclaim)
    mm_file_cache_reclaim(32);
}

// Allocate a single object from a slab cache (caller must hold c->lock)
static void *slab_alloc_from_cache_locked(struct slab_cache *c) {
  for (struct slab *s = c->partial; s; s = s->next) {
    int free_idx = -1;
    for (int i = 0; i < (int)s->total_count; i++) {
      if (!BITMAP_TEST(s->bitmap, i)) {
        free_idx = i;
        break;
      }
    }
    if (free_idx == -1)
      continue;

    BITMAP_SET(s->bitmap, free_idx);
    if (s->free_count > 0)
      s->free_count--;

    if (s->free_count == 0) {
      if (s->prev)
        s->prev->next = s->next;
      else
        c->partial = s->next;
      if (s->next)
        s->next->prev = s->prev;

      s->next = c->full;
      s->prev = NULL;
      if (c->full)
        c->full->prev = s;
      c->full = s;
    }

    uint8_t *obj_base = (uint8_t *)s + sizeof(struct slab);
    return (void *)(obj_base + (free_idx * c->obj_size));
  }

  struct slab *s = NULL;
  if (c->free) {
    s = c->free;
    c->free = s->next;
    if (c->free)
      c->free->prev = NULL;
    s->next = NULL;
    s->prev = NULL;
  } else {
    s = allocate_new_slab(c);
    if (!s)
      return NULL;
  }

  s->next = c->partial;
  s->prev = NULL;
  if (c->partial)
    c->partial->prev = s;
  c->partial = s;

  int free_idx = -1;
  for (int i = 0; i < (int)s->total_count; i++) {
    if (!BITMAP_TEST(s->bitmap, i)) {
      free_idx = i;
      break;
    }
  }

  if (free_idx == -1) {
    if (s->prev)
      s->prev->next = s->next;
    else
      c->partial = s->next;
    if (s->next)
      s->next->prev = s->prev;
    s->next = c->free;
    s->prev = NULL;
    if (c->free)
      c->free->prev = s;
    c->free = s;
    return NULL;
  }

  BITMAP_SET(s->bitmap, free_idx);
  if (s->free_count > 0)
    s->free_count--;

  if (s->free_count == 0) {
    if (s->prev)
      s->prev->next = s->next;
    else
      c->partial = s->next;
    if (s->next)
      s->next->prev = s->prev;

    s->next = c->full;
    s->prev = NULL;
    if (c->full)
      c->full->prev = s;
    c->full = s;
  }

  uint8_t *obj_base = (uint8_t *)s + sizeof(struct slab);
  return (void *)(obj_base + (free_idx * c->obj_size));
}

static void slab_free_to_cache_locked(struct slab_cache *c, struct slab *s, void *ptr) {
  uint64_t page_base = (uint64_t)s;
  uint64_t object_base = page_base + sizeof(struct slab);
  if ((uint64_t)ptr < object_base) {
    console_puts("[WARN] kfree: pointer inside slab metadata (wild free)!\n");
    return;
  }
  uint64_t offset = (uint64_t)ptr - object_base;
  if (offset % c->obj_size != 0) {
    console_puts("[WARN] kfree: pointer is not object-aligned (wild free)!\n");
    return;
  }
  uint32_t idx = offset / c->obj_size;
  if (idx >= s->total_count) {
    console_puts("[WARN] kfree: object index out of range (wild free)!\n");
    return;
  }

  if (!BITMAP_TEST(s->bitmap, idx)) {
    heap_trace_warn_double("inside Slab!", ptr);
    return;
  }

  BITMAP_CLEAR(s->bitmap, idx);
  s->free_count++;

  if (s->free_count == 1) {
    if (s->prev)
      s->prev->next = s->next;
    else
      c->full = s->next;
    if (s->next)
      s->next->prev = s->prev;

    s->next = c->partial;
    s->prev = NULL;
    if (c->partial)
      c->partial->prev = s;
    c->partial = s;
  }

  if (s->free_count == s->total_count) {
    if (s->prev)
      s->prev->next = s->next;
    else
      c->partial = s->next;
    if (s->next)
      s->next->prev = s->prev;

    s->next = c->free;
    s->prev = NULL;
    if (c->free)
      c->free->prev = s;
    c->free = s;
  }
}

void *heap_slab_alloc(size_t size) {
  int cache_idx = -1;
  for (size_t i = 0; i < CACHE_COUNT; i++) {
    if (size <= heap_caches[i].obj_size) {
      cache_idx = (int)i;
      break;
    }
  }

  if (cache_idx < 0)
    return NULL;

  struct slab_cache *c = &heap_caches[cache_idx];

  // Fast-path: Per-CPU local freelist
  if (heap_initialized) {
    hal_irq_state_t flags = hal_irq_save();
    struct cpu_info *cpu = cpu_get_current();
    if (cpu && cpu->cpu_id < MAX_CPUS) {
      struct cpu_local_slab *local = &heap_cpu_slab_caches[cpu->cpu_id][cache_idx];
      if (local->count > 0) {
        void *obj = local->entries[--local->count];
        hal_irq_restore(flags);
        return obj;
      }
    }
    hal_irq_restore(flags);
  }

  // Slow-path: Fine-grained per-cache lock
  spinlock_acquire(&c->lock);
  void *ptr = slab_alloc_from_cache_locked(c);
  spinlock_release(&c->lock);
  if (!ptr) {
    heap_oom_reclaim();
    spinlock_acquire(&c->lock);
    ptr = slab_alloc_from_cache_locked(c);
    spinlock_release(&c->lock);
    if (!ptr)
      return NULL;
  }

  spinlock_acquire(&c->lock);
  if (ptr && heap_initialized) {
    hal_irq_state_t flags = hal_irq_save();
    struct cpu_info *cpu = cpu_get_current();
    if (cpu && cpu->cpu_id < MAX_CPUS) {
      struct cpu_local_slab *local = &heap_cpu_slab_caches[cpu->cpu_id][cache_idx];
      for (int b = 0; b < 4 && local->count < LOCAL_CACHE_CAPACITY; b++) {
        void *batch_obj = slab_alloc_from_cache_locked(c);
        if (!batch_obj)
          break;
        local->entries[local->count++] = batch_obj;
      }
    }
    hal_irq_restore(flags);
  }
  spinlock_release(&c->lock);

  return ptr;
}

bool heap_slab_free(void *ptr, uint64_t page_base) {
  struct slab *s = (struct slab *)page_base;
  struct slab_cache *c = s->cache;
  int cache_idx = (int)(c - heap_caches);

  if (heap_initialized && cache_idx >= 0 && cache_idx < (int)CACHE_COUNT) {
    hal_irq_state_t flags = hal_irq_save();
    struct cpu_info *cpu = cpu_get_current();
    if (cpu && cpu->cpu_id < MAX_CPUS) {
      struct cpu_local_slab *local = &heap_cpu_slab_caches[cpu->cpu_id][cache_idx];
      if (local->count < LOCAL_CACHE_CAPACITY) {
        for (uint16_t i = 0; i < local->count; i++) {
          if (local->entries[i] == ptr) {
            heap_trace_warn_double("in per-CPU freelist!", ptr);
            hal_irq_restore(flags);
            return true;
          }
        }
        local->entries[local->count++] = ptr;
        hal_irq_restore(flags);
        return true;
      }
    }
    hal_irq_restore(flags);
  }

  spinlock_acquire(&c->lock);
  slab_free_to_cache_locked(c, s, ptr);
  spinlock_release(&c->lock);
  return true;
}

size_t heap_slab_ksize(const void *ptr, uint64_t page_base) {
  struct slab *s = (struct slab *)page_base;
  uint64_t object_base = page_base + sizeof(struct slab);
  if ((uint64_t)ptr < object_base)
    return 0;
  uint64_t offset = (uint64_t)ptr - object_base;
  if (offset % s->cache->obj_size != 0)
    return 0;
  uint32_t idx = offset / s->cache->obj_size;
  if (idx >= s->total_count)
    return 0;
  return s->cache->obj_size;
}

static void heap_u64_to_str(uint64_t val, char *buf) {
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char temp[32];
  int i = 0;
  while (val > 0) {
    temp[i++] = (val % 10) + '0';
    val /= 10;
  }
  int j = 0;
  while (i > 0) {
    buf[j++] = temp[--i];
  }
  buf[j] = '\0';
}

void heap_slab_get_info(char *buf) {
  char num_buf[32];
  strcat(buf, "Slab Statistics:\n");
  strcat(buf, "  Size | Total Slabs | Total Objs | Free Objs\n");
  strcat(buf, "-------|-------------|------------|-----------\n");

  for (size_t i = 0; i < CACHE_COUNT; i++) {
    struct slab_cache *c = &heap_caches[i];
    spinlock_acquire(&c->lock);
    uint32_t total_slabs = 0;
    uint32_t total_objs = 0;
    uint32_t free_objs = 0;

    for (struct slab *s = c->partial; s; s = s->next) {
      total_slabs++;
      total_objs += s->total_count;
      free_objs += s->free_count;
    }
    for (struct slab *s = c->full; s; s = s->next) {
      total_slabs++;
      total_objs += s->total_count;
      free_objs += s->free_count;
    }
    for (struct slab *s = c->free; s; s = s->next) {
      total_slabs++;
      total_objs += s->total_count;
      free_objs += s->free_count;
    }

    strcat(buf, "  ");
    heap_u64_to_str(c->obj_size, num_buf);
    strcat(buf, num_buf);
    int pad = 5 - (int)strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(total_slabs, num_buf);
    strcat(buf, num_buf);
    pad = 12 - (int)strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(total_objs, num_buf);
    strcat(buf, num_buf);
    pad = 11 - (int)strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(free_objs, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");
    spinlock_release(&c->lock);
  }
}

uint64_t heap_slab_total_slabs(void) {
  uint64_t total = 0;
  for (size_t i = 0; i < CACHE_COUNT; i++) {
    struct slab_cache *c = &heap_caches[i];
    spinlock_acquire(&c->lock);
    for (struct slab *s = c->partial; s; s = s->next) total++;
    for (struct slab *s = c->full; s; s = s->next) total++;
    for (struct slab *s = c->free; s; s = s->next) total++;
    spinlock_release(&c->lock);
  }
  return total;
}

uint64_t heap_slab_free_slabs(void) {
  uint64_t total = 0;
  for (size_t i = 0; i < CACHE_COUNT; i++) {
    struct slab_cache *c = &heap_caches[i];
    spinlock_acquire(&c->lock);
    for (struct slab *s = c->free; s; s = s->next) total++;
    spinlock_release(&c->lock);
  }
  return total;
}
