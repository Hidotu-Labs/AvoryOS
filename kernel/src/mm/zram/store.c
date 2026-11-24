#include "drivers/storage/block.h"
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

/* Disk-backed swap partition (slot IDs have DISK_SWAP_BIT set). */
#define DISK_SWAP_BIT 0x80000000U
#define DISK_SWAP_MAX_PAGES 1048576U /* up to 4 GiB per swap partition */
static struct block_device *disk_dev;
static uint32_t disk_pages;
static uint32_t disk_used;
static uint32_t disk_spp; /* sectors per 4 KiB page */
static uint32_t disk_hint;
static uint32_t *disk_refs;
static uint64_t *disk_bitmap;
static spinlock_t disk_lock = SPINLOCK_INIT;

static uint32_t disk_store_page(const void *page) {
  if (!disk_dev || !disk_refs || !disk_bitmap || !disk_pages)
    return 0;
  spinlock_acquire(&disk_lock);
  uint32_t words = (disk_pages + 64) / 64;
  uint32_t found = 0;
  for (uint32_t pass = 0; pass < words && !found; pass++) {
    uint32_t w = (disk_hint + pass) % words;
    uint64_t free_bits = ~disk_bitmap[w];
    if (!free_bits)
      continue;
    while (free_bits) {
      unsigned bit = (unsigned)__builtin_ctzll(free_bits);
      uint32_t idx = w * 64 + bit;
      if (idx >= 1 && idx <= disk_pages) {
        disk_bitmap[w] |= (1ull << bit);
        disk_hint = w;
        disk_used++;
        __atomic_store_n(&disk_refs[idx], 1, __ATOMIC_RELEASE);
        found = idx;
        break;
      }
      free_bits &= ~(1ull << bit);
    }
  }
  struct block_device *dev = disk_dev;
  uint32_t spp = disk_spp;
  spinlock_release(&disk_lock);

  if (!found)
    return 0;

  if (dev->write_sectors(dev, (uint64_t)found * spp, spp, page) != 0) {
    spinlock_acquire(&disk_lock);
    __atomic_store_n(&disk_refs[found], 0, __ATOMIC_RELEASE);
    disk_bitmap[found / 64] &= ~(1ull << (found % 64));
    if (disk_used > 0)
      disk_used--;
    spinlock_release(&disk_lock);
    return 0;
  }
  return DISK_SWAP_BIT | found;
}

int zram_swapon(const char *path, uint64_t flags) {
  (void)flags;
  if (!path || !path[0])
    return -2; /* ENOENT */
  if (strncmp(path, "/dev/", 5) != 0)
    return -2; /* ENOENT */

  struct block_device *dev = block_get_by_name(path + 5);
  if (!dev)
    return (strcmp(path, "/dev/null") == 0) ? -22 : -2; /* EINVAL or ENOENT */
  if (!dev->read_sectors || !dev->write_sectors || dev->sector_size == 0)
    return -22; /* EINVAL */
  if (4096 % dev->sector_size != 0)
    return -22; /* EINVAL */

  spinlock_acquire(&disk_lock);
  bool already = (disk_dev != NULL);
  spinlock_release(&disk_lock);
  if (already)
    return -16; /* EBUSY */

  uint32_t spp = 4096 / dev->sector_size;
  if (dev->total_sectors < (uint64_t)spp * 2)
    return -22; /* EINVAL */

  uint8_t *hdr = kmalloc(4096);
  if (!hdr)
    return -12; /* ENOMEM */
  if (dev->read_sectors(dev, 0, spp, hdr) != 0) {
    kfree(hdr);
    return -5; /* EIO */
  }

  /* Linux swap v1 header: "SWAPSPACE2" at 4086, version=1 at 1024, last_page at 1028 */
  if (memcmp(hdr + 4086, "SWAPSPACE2", 10) != 0) {
    kfree(hdr);
    return -22; /* EINVAL */
  }
  uint32_t version = 0, last_page = 0;
  memcpy(&version, hdr + 1024, 4);
  memcpy(&last_page, hdr + 1028, 4);
  kfree(hdr);

  if (version != 1 || last_page < 1)
    return -22; /* EINVAL */

  uint64_t max_dev_pages = (dev->total_sectors / spp) - 1;
  uint32_t pages = (last_page > max_dev_pages) ? (uint32_t)max_dev_pages : last_page;
  if (pages > DISK_SWAP_MAX_PAGES)
    pages = DISK_SWAP_MAX_PAGES;
  if (pages < 1)
    return -22;

  uint32_t *refs = kmalloc(((size_t)pages + 1) * sizeof(uint32_t));
  uint32_t words = (pages + 64) / 64;
  uint64_t *bmap = kmalloc((size_t)words * sizeof(uint64_t));
  if (!refs || !bmap) {
    if (refs) kfree(refs);
    if (bmap) kfree(bmap);
    return -12; /* ENOMEM */
  }
  memset(refs, 0, ((size_t)pages + 1) * sizeof(uint32_t));
  memset(bmap, 0, (size_t)words * sizeof(uint64_t));
  bmap[0] |= 1ull; /* slot 0 is reserved */

  spinlock_acquire(&disk_lock);
  if (disk_dev) {
    spinlock_release(&disk_lock);
    kfree(refs);
    kfree(bmap);
    return -16; /* EBUSY */
  }
  disk_refs = refs;
  disk_bitmap = bmap;
  disk_pages = pages;
  disk_used = 0;
  disk_spp = spp;
  disk_hint = 0;
  disk_dev = dev;
  spinlock_release(&disk_lock);
  return 0;
}

bool zram_get_disk_swap(char *name_out, size_t name_sz, uint64_t *total_kb,
                        uint64_t *used_kb) {
  spinlock_acquire(&disk_lock);
  if (!disk_dev) {
    spinlock_release(&disk_lock);
    return false;
  }
  if (name_out && name_sz > 0) {
    strncpy(name_out, disk_dev->name, name_sz - 1);
    name_out[name_sz - 1] = '\0';
  }
  if (total_kb)
    *total_kb = (uint64_t)disk_pages * 4ULL;
  if (used_kb)
    *used_kb = (uint64_t)disk_used * 4ULL;
  spinlock_release(&disk_lock);
  return true;
}

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
  if (!page)
    return 0;
  if (!slots)
    return disk_store_page(page);
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
      uint32_t dslot = disk_store_page(page);
      if (dslot)
        return dslot;
      __atomic_add_fetch(&rejected, 1, __ATOMIC_RELAXED);
      return 0;
    }
  }
  uint8_t compressed[2048];
  size_t n =
      same ? 0 : zram_lz4_compress(page, 4096, compressed, sizeof(compressed));
  if (!same && !n) {
    uint32_t dslot = disk_store_page(page);
    if (dslot)
      return dslot;
    __atomic_add_fetch(&rejected, 1, __ATOMIC_RELAXED);
    return 0;
  }
  spinlock_acquire(&lock);
  uint32_t id = free_head;
  if (!id) {
    stats.allocation_failures++;
    spinlock_release(&lock);
    return disk_store_page(page);
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
    return disk_store_page(page);
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
  if (id & DISK_SWAP_BIT) {
    uint32_t idx = id & ~DISK_SWAP_BIT;
    if (!disk_dev || !disk_refs || !idx || idx > disk_pages)
      return false;
    uint32_t r = __atomic_load_n(&disk_refs[idx], __ATOMIC_ACQUIRE);
    do {
      if (!r || r == UINT32_MAX)
        return false;
    } while (!__atomic_compare_exchange_n(&disk_refs[idx], &r, r + 1, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return true;
  }
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
  if (id & DISK_SWAP_BIT) {
    uint32_t idx = id & ~DISK_SWAP_BIT;
    if (!disk_dev || !disk_refs || !idx || idx > disk_pages)
      return;
    uint32_t r = __atomic_load_n(&disk_refs[idx], __ATOMIC_ACQUIRE);
    do {
      if (!r || r == UINT32_MAX)
        return;
    } while (!__atomic_compare_exchange_n(&disk_refs[idx], &r, r - 1, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    if (r - 1)
      return;
    spinlock_acquire(&disk_lock);
    disk_bitmap[idx / 64] &= ~(1ull << (idx % 64));
    if (disk_used > 0)
      disk_used--;
    spinlock_release(&disk_lock);
    return;
  }
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
  if (!page)
    return -1;
  if (id & DISK_SWAP_BIT) {
    uint32_t idx = id & ~DISK_SWAP_BIT;
    if (!disk_dev || !disk_refs || !idx || idx > disk_pages)
      return -1;
    if (!__atomic_load_n(&disk_refs[idx], __ATOMIC_ACQUIRE))
      return -1;
    return disk_dev->read_sectors(disk_dev, (uint64_t)idx * disk_spp, disk_spp, page);
  }
  if (!slots || !id || id > count)
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
