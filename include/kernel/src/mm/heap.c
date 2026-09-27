#include "mm/heap.h"
#include "console/console.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "smp/cpu.h"

#define SLAB_MAGIC 0x51ABCAFECAFE51ABULL
#define BIG_MAGIC 0xB16A110CB16A110CULL

#define BITMAP_SET(bmp, index) ((bmp)[(index) / 32] |= (1U << ((index) % 32)))
#define BITMAP_CLEAR(bmp, index) ((bmp)[(index) / 32] &= ~(1U << ((index) % 32)))
#define BITMAP_TEST(bmp, index)                                                \
  (((bmp)[(index) / 32] & (1U << ((index) % 32))) != 0)

// Dedicated lock for virtual memory heap space & large allocations
static spinlock_t vmem_lock = SPINLOCK_INIT;
bool heap_initialized = false;

struct slab_cache;

struct slab {
  uint64_t magic;
  struct slab *next;
  struct slab *prev;
  uint32_t free_count;
  uint32_t total_count;
  struct slab_cache *cache;
  uint32_t bitmap[4]; // 128 bits
} __attribute__((aligned(64)));

struct slab_cache {
  size_t obj_size;
  spinlock_t lock;      // Fine-grained lock per size class
  struct slab *partial;
  struct slab *full;
  struct slab *free;
};

struct big_alloc {
  uint64_t magic;
  size_t pages;
  struct big_alloc *next;
  struct big_alloc *prev;
  uint64_t padding[4];
} __attribute__((aligned(64)));

static struct big_alloc *big_alloc_head = NULL;

static struct slab_cache caches[] = {
    {32,   SPINLOCK_INIT, NULL, NULL, NULL},
    {64,   SPINLOCK_INIT, NULL, NULL, NULL},
    {128,  SPINLOCK_INIT, NULL, NULL, NULL},
    {256,  SPINLOCK_INIT, NULL, NULL, NULL},
    {512,  SPINLOCK_INIT, NULL, NULL, NULL},
    {1024, SPINLOCK_INIT, NULL, NULL, NULL},
};
#define CACHE_COUNT (sizeof(caches) / sizeof(caches[0]))

// Per-CPU local freelist (Magazine Cache)
#define LOCAL_CACHE_CAPACITY 16

struct cpu_local_slab {
  uint16_t count;
  void *entries[LOCAL_CACHE_CAPACITY];
};

static struct cpu_local_slab cpu_slab_caches[MAX_CPUS][CACHE_COUNT];

void heap_init(void) {
  for (size_t i = 0; i < CACHE_COUNT; i++) {
    spinlock_init(&caches[i].lock);
  }
  spinlock_init(&vmem_lock);
  memset(cpu_slab_caches, 0, sizeof(cpu_slab_caches));
  heap_initialized = true;
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

// Allocate a single object from a slab cache (caller must hold c->lock)
static void *slab_alloc_from_cache_locked(struct slab_cache *c) {
  struct slab *s = c->partial;

  // If no partial, try to get from free list, else allocate new slab
  if (!s) {
    if (c->free) {
      s = c->free;
      c->free = s->next;
      if (c->free)
        c->free->prev = NULL;
      s->next = NULL;
    } else {
      // Allocate new slab
      s = allocate_new_slab(c);
      if (!s)
        return NULL;
    }
    // Put it in partial list
    s->next = c->partial;
    s->prev = NULL;
    if (c->partial)
      c->partial->prev = s;
    c->partial = s;
  }

  // Find free index in partial slab
  int free_idx = -1;
  for (int i = 0; i < (int)s->total_count; i++) {
    if (!BITMAP_TEST(s->bitmap, i)) {
      free_idx = i;
      break;
    }
  }

  if (free_idx == -1)
    return NULL;

  BITMAP_SET(s->bitmap, free_idx);
  s->free_count--;

  // If slab is now full, move it from partial to full list
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

// Free a single object back into its slab (caller must hold c->lock)
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
    console_puts("[WARN] kfree: Double free intercepted inside Slab!\n");
    return;
  }

  BITMAP_CLEAR(s->bitmap, idx);
  s->free_count++;

  // If it was full, it is now partial
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

  // If it's completely empty, move to the free list for later reuse
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

void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;

  // 1. Can we fit it in a slab cache?
  int cache_idx = -1;
  for (size_t i = 0; i < CACHE_COUNT; i++) {
    if (size <= caches[i].obj_size) {
      cache_idx = (int)i;
      break;
    }
  }

  if (cache_idx >= 0) {
    struct slab_cache *c = &caches[cache_idx];

    // Fast-path: Per-CPU local freelist
    if (heap_initialized) {
      hal_irq_state_t flags = hal_irq_save();
      struct cpu_info *cpu = cpu_get_current();
      if (cpu && cpu->cpu_id < MAX_CPUS) {
        struct cpu_local_slab *local = &cpu_slab_caches[cpu->cpu_id][cache_idx];
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

    // If successful and local cache has room, pre-refill a small batch (up to 4)
    if (ptr && heap_initialized) {
      hal_irq_state_t flags = hal_irq_save();
      struct cpu_info *cpu = cpu_get_current();
      if (cpu && cpu->cpu_id < MAX_CPUS) {
        struct cpu_local_slab *local = &cpu_slab_caches[cpu->cpu_id][cache_idx];
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

  // 2. Large Allocation Path (> 1024 bytes)
  size_t total_size = size + sizeof(struct big_alloc);
  size_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

  void *blocks = pmm_alloc_pages(pages);
  if (!blocks)
    return NULL;

  uint64_t vaddr = (uint64_t)blocks + pmm_get_hhdm_offset();
  struct big_alloc *b = (struct big_alloc *)vaddr;
  b->magic = BIG_MAGIC;
  b->pages = pages;

  spinlock_acquire(&vmem_lock);
  b->next = big_alloc_head;
  b->prev = NULL;
  if (big_alloc_head)
    big_alloc_head->prev = b;
  big_alloc_head = b;
  spinlock_release(&vmem_lock);

  return (void *)((uint8_t *)b + sizeof(struct big_alloc));
}

void kfree(void *ptr) {
  if (!ptr)
    return;

  uint64_t page_base = (uint64_t)ptr & ~0xFFFULL;
  uint64_t magic_check = *(uint64_t *)page_base;

  // Is it a Slab chunk?
  if (magic_check == SLAB_MAGIC) {
    struct slab *s = (struct slab *)page_base;
    struct slab_cache *c = s->cache;
    int cache_idx = (int)(c - caches);

    // Fast-path: Per-CPU local freelist
    if (heap_initialized && cache_idx >= 0 && cache_idx < (int)CACHE_COUNT) {
      hal_irq_state_t flags = hal_irq_save();
      struct cpu_info *cpu = cpu_get_current();
      if (cpu && cpu->cpu_id < MAX_CPUS) {
        struct cpu_local_slab *local = &cpu_slab_caches[cpu->cpu_id][cache_idx];
        if (local->count < LOCAL_CACHE_CAPACITY) {
          /* The fast path deliberately leaves the slab bitmap bit set, so the
           * slow-path double-free check cannot see an object that is already
           * parked here.  A second free would otherwise queue the same object
           * twice and two later kmalloc()s would hand it out twice. */
          for (uint16_t i = 0; i < local->count; i++) {
            if (local->entries[i] == ptr) {
              console_puts("[WARN] kfree: double free intercepted in per-CPU "
                           "freelist!\n");
              hal_irq_restore(flags);
              return;
            }
          }
          local->entries[local->count++] = ptr;
          hal_irq_restore(flags);
          return;
        }
      }
      hal_irq_restore(flags);
    }

    // Slow-path: Fine-grained per-cache lock
    spinlock_acquire(&c->lock);
    slab_free_to_cache_locked(c, s, ptr);
    spinlock_release(&c->lock);
    return;
  }

  // Is it a Big Alloc chunk?
  if (magic_check == BIG_MAGIC) {
    struct big_alloc *b = (struct big_alloc *)page_base;

    spinlock_acquire(&vmem_lock);
    if (b->prev)
      b->prev->next = b->next;
    else
      big_alloc_head = b->next;
    if (b->next)
      b->next->prev = b->prev;

    size_t pages = b->pages;
    b->magic = 0;
    spinlock_release(&vmem_lock);

    uint64_t phys_addr = page_base - pmm_get_hhdm_offset();
    pmm_free_pages((void *)phys_addr, pages);
    return;
  }

  console_puts(
      "[WARN] kfree: Fatal validation failure. Invalid pointer space.\n");
  klog_puts("  ptr=");
  klog_uint64((uint64_t)ptr);
  klog_puts(" page_base=");
  klog_uint64(page_base);
  klog_puts(" magic=");
  klog_uint64(magic_check);
  klog_puts("\n");
}

void *kcalloc(size_t num, size_t size) {
  size_t total = num * size;
  void *ptr = kmalloc(total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
  if (!ptr)
    return kmalloc(new_size);
  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  uint64_t page_base = (uint64_t)ptr & ~0xFFFULL;
  uint64_t magic_check = *(uint64_t *)page_base;
  size_t old_size = 0;

  if (magic_check == SLAB_MAGIC) {
    struct slab *s = (struct slab *)page_base;
    old_size = s->cache->obj_size;
  } else if (magic_check == BIG_MAGIC) {
    struct big_alloc *b = (struct big_alloc *)page_base;
    old_size = (b->pages * PAGE_SIZE) - sizeof(struct big_alloc);
  } else {
    return NULL;
  }

  if (new_size <= old_size)
    return ptr;

  void *new_ptr = kmalloc(new_size);
  if (new_ptr) {
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
  }
  return new_ptr;
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

void heap_get_info(char *buf) {
  buf[0] = '\0';
  char num_buf[32];

  strcat(buf, "Slab Statistics:\n");
  strcat(buf, "  Size | Total Slabs | Total Objs | Free Objs\n");
  strcat(buf, "-------|-------------|------------|-----------\n");

  for (size_t i = 0; i < CACHE_COUNT; i++) {
    struct slab_cache *c = &caches[i];
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
    int pad = 5 - strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(total_slabs, num_buf);
    strcat(buf, num_buf);
    pad = 12 - strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(total_objs, num_buf);
    strcat(buf, num_buf);
    pad = 11 - strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(free_objs, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");
    spinlock_release(&c->lock);
  }

  spinlock_acquire(&vmem_lock);
  strcat(buf, "\nBig Allocations:\n");
  uint32_t big_count = 0;
  uint64_t big_pages = 0;
  for (struct big_alloc *b = big_alloc_head; b; b = b->next) {
    big_count++;
    big_pages += b->pages;
  }

  strcat(buf, "  Count: ");
  heap_u64_to_str(big_count, num_buf);
  strcat(buf, num_buf);
  strcat(buf, "\n");

  strcat(buf, "  Total Pages: ");
  heap_u64_to_str(big_pages, num_buf);
  strcat(buf, num_buf);
  strcat(buf, "\n");

  strcat(buf, "  Total Size: ");
  heap_u64_to_str(big_pages * PAGE_SIZE / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");
  spinlock_release(&vmem_lock);
}
