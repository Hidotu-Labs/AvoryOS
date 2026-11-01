#include "lib/string.h"
#include "lock/spinlock.h"
#include "lz4.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/zram.h"

struct packed_page {
  void *phys;
  struct packed_page *prev, *next;
  uint64_t free_mask;
  uint16_t size, bin;
};
/* 64-byte size classes, 64..2048 bytes: a compressed page is rounded up by
 * at most 63 bytes instead of up to 2x. */
#define PACKED_CLASSES 32
static struct packed_page *partial[PACKED_CLASSES];
#define PACKED_CHARGE (4096 + 64)      /* frame plus descriptor slab object */
struct slot {
  void *data;
  struct packed_page *pool;
  uint64_t same;
  uint32_t refs, next;
  uint16_t length, charge;
};
static struct slot *slots;
static uint32_t count, free_head;
static spinlock_t lock = SPINLOCK_INIT;
static struct zram_stats stats;
static uint64_t rejected;

bool zram_store_init(uint32_t capacity, uint64_t limit) {
  if (slots || !capacity || capacity > 524288 || limit < 4096)
    return false;
  size_t table_bytes =
      ((((size_t)capacity + 1) * sizeof(struct slot) + 64 + 4095) / 4096) *
      4096;
  if (table_bytes > limit)
    return false;
  struct slot *table = kmalloc(((size_t)capacity + 1) * sizeof(*table));
  if (!table)
    return false;
  memset(table, 0, ((size_t)capacity + 1) * sizeof(*table));
  for (uint32_t i = 1; i <= capacity; i++)
    table[i].next = i == capacity ? 0 : i + 1;
  slots = table;
  count = capacity;
  free_head = 1;
  stats.capacity_pages = capacity;
  stats.memory_limit = limit;
  stats.memory_bytes = table_bytes;
  stats.peak_bytes = stats.memory_bytes;
  stats.enabled = true;
  return true;
}
static void link_partial(struct packed_page *p) {
  p->prev = NULL;
  p->next = partial[p->bin];
  if (p->next)
    p->next->prev = p;
  partial[p->bin] = p;
}
static void unlink_partial(struct packed_page *p) {
  if (p->prev)
    p->prev->next = p->next;
  else
    partial[p->bin] = p->next;
  if (p->next)
    p->next->prev = p->prev;
  p->prev = p->next = NULL;
}
static void *packed_alloc(size_t n, struct packed_page **pool) {
  unsigned bin = n ? (unsigned)((n + 63) / 64) - 1 : 0;
  unsigned size = (bin + 1) * 64;
  spinlock_acquire(&lock);
  struct packed_page *p = partial[bin];
  if (p) {
    unsigned i = (unsigned)__builtin_ctzll(p->free_mask);
    p->free_mask &= ~(1ull << i);
    if (!p->free_mask)
      unlink_partial(p);
    *pool = p;
    void *data = (void *)((uint64_t)p->phys + pmm_get_hhdm_offset() + i * size);
    spinlock_release(&lock);
    return data;
  }
  if (stats.memory_bytes > stats.memory_limit ||
      PACKED_CHARGE > stats.memory_limit - stats.memory_bytes) {
    spinlock_release(&lock);
    return NULL;
  }
  stats.memory_bytes += PACKED_CHARGE; /* reserve before allocating off-lock */
  spinlock_release(&lock);
  p = kmalloc(sizeof(*p));
  void *phys = p ? pmm_alloc() : NULL;
  if (!phys) {
    if (p)
      kfree(p);
    spinlock_acquire(&lock);
    stats.memory_bytes -= PACKED_CHARGE;
    spinlock_release(&lock);
    return NULL;
  }
  memset(p, 0, sizeof(*p));
  p->phys = phys;
  p->size = size;
  p->bin = bin;
  unsigned objects = 4096 / size;
  p->free_mask = objects == 64 ? ~0ull : (1ull << objects) - 1;
  p->free_mask &= ~1ull;
  spinlock_acquire(&lock);
  link_partial(p);
  spinlock_release(&lock);
  *pool = p;
  return (void *)((uint64_t)phys + pmm_get_hhdm_offset());
}
uint32_t zram_store_page(const void *page) {
  if (!slots || !page)
    return 0;
  const uint8_t *bytes = page;
  uint64_t pattern;
  memcpy(&pattern, page, 8);
  bool same = true;
  for (unsigned i = 8; i < 4096; i += 8) {
    uint64_t value;
    memcpy(&value, bytes + i, 8);
    if (value != pattern) {
      same = false;
      break;
    }
  }
  /* Early exit for incompressible data: a 1 KiB probe that cannot reach 60%
   * rejects the page without compressing all 4 KiB. */
  if (!same) {
    uint8_t probe[640];
    if (!zram_lz4_compress(page, 1024, probe, sizeof(probe))) {
      __atomic_add_fetch(&rejected, 1, __ATOMIC_RELAXED);
      return 0;
    }
  }
  uint8_t compressed[2048];
  size_t n =
      same ? 0 : zram_lz4_compress(page, 4096, compressed, sizeof(compressed));
  if (!same && !n) {
    __atomic_add_fetch(&rejected, 1, __ATOMIC_RELAXED);
    return 0;
  }
  spinlock_acquire(&lock);
  uint32_t id = free_head;
  if (!id) {
    stats.allocation_failures++;
    spinlock_release(&lock);
    return 0;
  }
  free_head = slots[id].next;
  spinlock_release(&lock);
  struct packed_page *pool = NULL;
  void *data = n ? packed_alloc(n, &pool) : NULL;
  if (n && !data) {
    spinlock_acquire(&lock);
    stats.allocation_failures++;
    slots[id].next = free_head;
    free_head = id;
    spinlock_release(&lock);
    return 0;
  }
  if (n)
    memcpy(data, compressed, n);
  spinlock_acquire(&lock);
  slots[id].data = data;
  slots[id].pool = pool;
  slots[id].same = pattern;
  slots[id].length = n;
  /* Publish only initialized data: the release pairs with the acquire in
   * zram_slot_get() and zram_slot_read(). */
  __atomic_store_n(&slots[id].refs, 1, __ATOMIC_RELEASE);
  stats.stored_pages++;
  stats.compressed_bytes += n;
  if (same)
    stats.same_pages++;
  if (stats.memory_bytes > stats.peak_bytes)
    stats.peak_bytes = stats.memory_bytes;
  spinlock_release(&lock);
  return id;
}
/* Per-slot refcounts. A slot's data and metadata are written before its refs
 * are published, and never change while refs > 0, so readers holding a pin
 * need no lock. The global lock now guards only the free list, the packed
 * pools and statistics. A count can reach zero only in zram_slot_put(), and
 * after that no get can succeed, so the final put frees the slot exclusively. */
bool zram_slot_get(uint32_t id) {
  if (!slots || !id || id > count)
    return false;
  uint32_t r = __atomic_load_n(&slots[id].refs, __ATOMIC_ACQUIRE);
  do {
    if (!r || r == UINT32_MAX)
      return false;
  } while (!__atomic_compare_exchange_n(&slots[id].refs, &r, r + 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
  return true;
}
void zram_slot_put(uint32_t id) {
  if (!slots || !id || id > count)
    return;
  uint32_t r = __atomic_load_n(&slots[id].refs, __ATOMIC_ACQUIRE);
  do {
    if (!r || r == UINT32_MAX)
      return;
  } while (!__atomic_compare_exchange_n(&slots[id].refs, &r, r - 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
  if (r - 1)
    return;
  struct packed_page *release = NULL;
  spinlock_acquire(&lock);
  struct slot *s = &slots[id];
  struct packed_page *p = s->pool;
  if (p) {
    unsigned i =
        ((uint64_t)s->data - (uint64_t)p->phys - pmm_get_hhdm_offset()) /
        p->size;
    bool full = !p->free_mask;
    p->free_mask |= 1ull << i;
    if (full)
      link_partial(p);
    unsigned objects = 4096 / p->size;
    uint64_t all = objects == 64 ? ~0ull : (1ull << objects) - 1;
    if (p->free_mask == all) {
      unlink_partial(p);
      stats.memory_bytes -= PACKED_CHARGE;
      release = p;
    }
  }
  stats.compressed_bytes -= s->length;
  stats.stored_pages--;
  if (!s->length)
    stats.same_pages--;
  memset(s, 0, sizeof(*s));
  s->next = free_head;
  free_head = id;
  spinlock_release(&lock);
  if (release) {
    pmm_free_page(release->phys);
    kfree(release);
  }
}
int zram_slot_read(uint32_t id, void *page) {
  if (!slots || !id || id > count || !page)
    return -1;
  /* The caller holds a pin, so these fields are stable for the copy. */
  struct slot *s = &slots[id];
  if (!__atomic_load_n(&s->refs, __ATOMIC_ACQUIRE))
    return -1;
  uint64_t same = s->same;
  uint16_t length = s->length;
  void *data = s->data;
  if (!length) {
    for (unsigned i = 0; i < 4096; i += 8)
      memcpy((uint8_t *)page + i, &same, 8);
    return 0;
  }
  return zram_lz4_decompress(data, length, page, 4096);
}
void zram_store_stats(struct zram_stats *out) {
  spinlock_acquire(&lock);
  *out = stats;
  spinlock_release(&lock);
  out->rejected_pages = __atomic_load_n(&rejected, __ATOMIC_RELAXED);
}
