/* Linux page model for AvoryOS.
 *
 * Implements the Linux page API described in <linux/mm.h> over the native
 * buddy allocator (kernel/src/mm/pmm.c) with a virtually contiguous mem_map:
 *
 *   - Descriptors live in a reserved VA arena based at vmemmap_base (the
 *     start of the VMAP window; linuxkpi_vmalloc_init() starts first-fit
 *     above it).  pfn_to_page() is pure arena arithmetic, and the
 *     descriptors of any block are adjacent, so upstream code that walks
 *     `page + n` or `++page` across a multi-page block stays correct.
 *   - The physical backing stays lazy and order-0: the first pfn_to_page()
 *     of a 64-descriptor chunk allocates one order-0 page and maps it into
 *     the chunk's arena slot.  That keeps the allocation budget the chunked
 *     layout was introduced for (the old layout demanded one order-9 (2 MB)
 *     buddy block per newly touched 128 MB section; under desktop
 *     fragmentation that allocation failed, made pfn_to_page() return NULL,
 *     and forced alloc_pages() to free the frame it had just obtained — the
 *     2026-10-16 TTM/PRIME -ENOMEM capture).  Contiguous VA without
 *     contiguous physical backing supplies both properties at once: no raw
 *     pointer walk can leave a block, and no metadata allocation is ever
 *     high-order.
 *   - `struct page::pfn` is stored in the descriptor, so page_to_pfn() needs
 *     no arena arithmetic.
 *   - `_refcount` is authoritative for pages handed to Linux code.  Pages
 *     allocated here are never touched by the native CoW/refcount paths, and
 *     the native PMM reference (1) is dropped exactly once, when the Linux
 *     refcount reaches zero.  Never mix the two accounting schemes on one
 *     page.
 *
 * Order-0 pages use the native per-CPU page cache inside pmm_alloc_page();
 * order-N allocations go straight to the buddy lists.
 */

#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/log2.h>
#include <linux/spinlock.h>

#include <linuxkpi/log.h>
#include <linuxkpi/gpu_diag.h>
#include <linuxkpi/native_mm.h>

extern unsigned long linuxkpi_shrink_reclaim(gfp_t gfp, unsigned long needed);

#define SECTION_SHIFT 27 /* 128 MB: descriptor-arena extent granularity */
#define PAGES_PER_SECTION (1UL << (SECTION_SHIFT - PAGE_SHIFT))
#define SECTIONS_FOR(phys) (((phys) >> SECTION_SHIFT) + 2)

/* Physical descriptor storage is chunked so every allocation is order-0: one
 * 4 KB buddy page holds CHUNK_PAGES descriptors (64 at 64 bytes each) and is
 * mapped at its arena slot, so descriptor arrays are contiguous in VA while
 * the backing pages stay order-0.  The static asserts keep a future
 * descriptor-size change from silently breaking the arena page math. */
#define CHUNK_PAGES (PAGE_SIZE / sizeof(struct page))
#define CHUNK_BYTES (CHUNK_PAGES * sizeof(struct page))
#define CHUNKS_PER_SECTION (PAGES_PER_SECTION / CHUNK_PAGES)
_Static_assert(PAGE_SIZE % sizeof(struct page) == 0,
               "descriptors must tile a page");
_Static_assert(PAGES_PER_SECTION % CHUNK_PAGES == 0,
               "descriptor chunks must tile a section");
_Static_assert(CHUNK_BYTES == PAGE_SIZE,
               "one descriptor chunk must be one arena page");

#define VMAP_WINDOW_BASE 0xFFFFC00000000000ULL
#define VMAP_WINDOW_END 0xFFFFE00000000000ULL

/* Symbols expected by imported x86 headers (asm/page_64.h).  The direct-map
 * base is the runtime HHDM offset; phys_base only matters for __pa() of kernel
 * text, which no LinuxKPI code uses today. */
unsigned long max_pfn;
unsigned long page_offset_base = 0xFFFF800000000000UL;
unsigned long phys_base;
unsigned long vmalloc_base = VMAP_WINDOW_BASE;
unsigned long vmemmap_base;          /* descriptor arena base (set at init) */
unsigned long linuxkpi_vmemmap_size; /* arena bytes; read by vmalloc.c */
unsigned long physmem_end;

/* One bit per descriptor chunk, set once the chunk page is mapped and its
 * descriptors are initialized; the pfn_to_page() fast path tests it without
 * the lock. */
static uint64_t *page_chunk_present;
static DEFINE_SPINLOCK(page_chunk_lock);

static bool page_model_ready;

/* ------------------------------------------------------------------------- */
/* mem_map                                                                    */
/* ------------------------------------------------------------------------- */

/* Descriptor address for a PFN: the arena is contiguous by construction, so
 * this arithmetic is the whole lookup. */
static struct page *page_arena(unsigned long pfn) {
  return (struct page *)(uintptr_t)(vmemmap_base +
                                    (uint64_t)pfn * sizeof(struct page));
}

/* Mapping state of one descriptor chunk: the fast path in pfn_to_page()
 * pairs its acquire load with the release OR in page_chunk_publish() after
 * the descriptors and the PTE are in place. */
static inline bool page_chunk_mapped(unsigned long chunk) {
  return (__atomic_load_n(&page_chunk_present[chunk / 64], __ATOMIC_ACQUIRE) &
          (1ULL << (chunk % 64))) != 0;
}

static inline void page_chunk_publish(unsigned long chunk) {
  __atomic_fetch_or(&page_chunk_present[chunk / 64], 1ULL << (chunk % 64),
                    __ATOMIC_RELEASE);
}

/* Chunk slow path: one order-0 page of zeroed, pre-initialized descriptors,
 * mapped at its arena slot.  Publishing the fully built chunk keeps the
 * lock-free fast path safe. */
static struct page *page_chunk_alloc(unsigned long chunk) {
  unsigned long flags;

  if (__builtin_expect(page_chunk_mapped(chunk), 1))
    return page_arena(chunk * CHUNK_PAGES);

  spin_lock_irqsave(&page_chunk_lock, flags);
  if (!page_chunk_mapped(chunk)) {
    uint64_t phys = (uint64_t)asc_pmm_alloc_pages(1);

    if (phys) {
      unsigned long first = chunk * CHUNK_PAGES;
      struct page *base = page_arena(first);

      /* Map first and initialize through the arena address: the page is
       * never written through another alias, and it stays unpublished until
       * the bitmap bit is set.  A never-mapped VA has no stale TLB entry,
       * so no flush is needed. */
      if (asc_vmm_map_page(asc_vmm_get_kernel_pml4(), (uint64_t)base, phys,
                           ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_NX)) {
        __builtin_memset(base, 0, CHUNK_BYTES);
        for (unsigned long i = 0; i < CHUNK_PAGES; i++) {
          base[i].pfn = (unsigned int)(first + i);
          atomic_set(&base[i]._refcount, 0);
          atomic_set(&base[i]._mapcount, -1);
        }
        page_chunk_publish(chunk);
      } else {
        asc_pmm_free_pages((void *)phys, 1);
      }
    }
  }
  spin_unlock_irqrestore(&page_chunk_lock, flags);
  return page_chunk_mapped(chunk) ? page_arena(chunk * CHUNK_PAGES) : NULL;
}

struct page *pfn_to_page(unsigned long pfn) {
  unsigned long chunk;

  if (!page_model_ready || pfn >= max_pfn)
    return NULL;

  chunk = pfn / CHUNK_PAGES;
  if (__builtin_expect(!page_chunk_mapped(chunk), 0)) {
    if (!page_chunk_alloc(chunk))
      return NULL;
  }
  return page_arena(pfn);
}

unsigned long page_to_pfn(const struct page *page) { return page->pfn; }

phys_addr_t page_to_phys(struct page *page) {
  /* Use the descriptor's own pfn: upstream page_to_phys() is
   * page_to_pfn(page) << PAGE_SHIFT and never walks a compound head.  Using
   * compound_head() here aliased every page of a non-compound high-order
   * allocation (TTM pool) onto its block head. */
  return (phys_addr_t)page->pfn << PAGE_SHIFT;
}

struct page *phys_to_page(phys_addr_t phys) {
  return pfn_to_page((unsigned long)phys >> PAGE_SHIFT);
}

void *page_address(const struct page *page) {
  return (void *)(((uint64_t)page->pfn << PAGE_SHIFT) +
                  asc_pmm_get_hhdm_offset());
}

bool is_vmalloc_addr(const void *x) {
  uint64_t v = (uint64_t)x;
  return v >= VMAP_WINDOW_BASE && v < VMAP_WINDOW_END;
}

/* Called by the virt_addr_valid() macro in asm/page.h. */
bool __virt_addr_valid(unsigned long kaddr) {
  uint64_t hhdm = asc_pmm_get_hhdm_offset();

  if (kaddr < hhdm)
    return false;
  return asc_pmm_is_managed(kaddr - hhdm);
}

/* ------------------------------------------------------------------------- */
/* Initialization                                                             */
/* ------------------------------------------------------------------------- */

void linuxkpi_page_init(void) {
  /* PFNs describe physical addresses, not the sum of RAM region sizes.
   * PCI holes can put valid RAM above total_ram (Q35 puts RAM above 4 GiB). */
  uint64_t total = asc_pmm_get_managed_memory_end();
  uint64_t hhdm = asc_pmm_get_hhdm_offset();
  unsigned long sections = SECTIONS_FOR(total);
  unsigned long chunks = sections * CHUNKS_PER_SECTION;
  size_t bytes = sections * PAGES_PER_SECTION * sizeof(struct page);
  size_t bits = (chunks + 7) / 8;
  size_t pages = (bits + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t phys;

  if (page_model_ready)
    return;

  /* The arena must fit the reserved VA region the vmap window starts with. */
  if (bytes > VMAP_WINDOW_END - VMAP_WINDOW_BASE)
    return;

  /* The present bitmap is the only eager metadata: one bit per 64
   * descriptors, so one order-0 page covers 8 GiB of RAM. */
  phys = (uint64_t)asc_pmm_alloc_pages(pages);
  if (!phys)
    return;

  page_chunk_present = (uint64_t *)(phys + hhdm);
  __builtin_memset(page_chunk_present, 0, pages * PAGE_SIZE);

  page_offset_base = hhdm;
  max_pfn = total >> PAGE_SHIFT;
  physmem_end = total;
  vmemmap_base = VMAP_WINDOW_BASE;
  linuxkpi_vmemmap_size = bytes;
  page_model_ready = true;
  klogf("[KPI] texture fixes active: WC-sfence/shrink-reclaim/ww-backoff/fault-seq\n");

  /* Chunk pages are allocated and mapped lazily by pfn_to_page() the first
   * time a page in that 64-descriptor chunk is handed out, so a 4 GiB
   * machine pays only for the PFNs that actually carry Linux-managed pages
   * (a fully touched 128 MB section costs 2 MB of descriptors, in order-0
   * pieces).  The eager alternative built all 34 sections (~68 MiB) at boot
   * even though the boot-time desktop touches a handful; see the "~150 MB
   * used after the KPI tests" report.  Driver tests that track PMM free-page
   * deltas warm up before sampling, which absorbs the one-time chunk
   * allocation of a newly touched range. */
}

/* ------------------------------------------------------------------------- */
/* Allocation                                                                 */
/* ------------------------------------------------------------------------- */

static void page_set_order(struct page *head, unsigned int order) {
  for (unsigned int i = 0; i < order; i++) {
    unsigned long nr = 1UL << i;
    for (unsigned long j = 0; j < nr; j++) {
      struct page *tail = head + (1UL << i) + j;
      tail->compound_head = head;
      __SetPageTail(tail);
      atomic_set(&tail->_refcount, 0);
      atomic_set(&tail->_mapcount, -1);
    }
  }

  __ClearPageTail(head);
  head->compound_head = head;
  head->compound_order = (unsigned char)order;
  if (order)
    __SetPageHead(head);
  else
    __ClearPageHead(head);
}

/* Non-compound high-order allocation (no __GFP_COMP): every page of the block
 * is an independent descriptor.  Upstream leaves the pages unmarked; clear the
 * compound state explicitly so a stale PG_head/PG_tail from the mem_map reuse
 * cannot make compound_head() alias pages. */
static void page_set_order_plain(struct page *head, unsigned int order) {
  unsigned long count = 1UL << order;

  for (unsigned long i = 0; i < count; i++) {
    struct page *p = head + i;

    __ClearPageTail(p);
    __ClearPageHead(p);
    p->compound_head = p;
    p->compound_order = 0;
    atomic_set(&p->_refcount, 1);
    atomic_set(&p->_mapcount, -1);
  }
}

struct page *alloc_pages(gfp_t gfp_mask, unsigned int order) {
  size_t count;
  uint64_t phys;
  struct page *page, *head;

  if (order > MAX_PAGE_ORDER)
    return NULL;

  count = 1UL << order;

  if (gfp_mask & __GFP_DMA32)
    phys = (uint64_t)asc_pmm_alloc_pages_range(count, 0, 0x100000000ULL);
  else
    phys = (uint64_t)asc_pmm_alloc_pages(count);

  if (!phys && (gfp_mask & __GFP_DIRECT_RECLAIM)) {
    /* Pressure relief before failing the allocation. Pools that can hold
     * pages a fresh allocation cannot see:
     *   - native frames parked in the PCP/pre-zeroed pools, which LinuxKPI
     *     allocations bypass (returning them lets free buddies merge into
     *     the contiguous run a high-order request needs),
     *   - shrinker-registered caches (TTM keeps freed GTT pages; the
     *     Minecraft texture-churn case from the original shrinker change),
     *   - clean file-cache pages: under a full desktop (Xorg+KDE+Firefox)
     *     the PMM can be empty while megabytes of droppable cache remain.
     * A failed allocation reaches userspace as -ENOMEM (amdgpu rejects the
     * command submission and Mesa aborts the client), so run one bounded
     * relief pass and retry once. The cheap frame drain runs even for
     * __GFP_NORETRY callers: TTM's high-order pool refills set
     * NORETRY|RETRY_MAYFAIL and walk down through lower orders, and merged
     * parked frames are what makes those retries succeed. The sleeping
     * shrinker/file-cache reclaim stays limited to callers that permit
     * reclaim, and callers that forbid it outright (GFP_ATOMIC/GFP_NOWAIT
     * clear __GFP_DIRECT_RECLAIM) get no relief at all. */
    asc_pmm_drain_cached_frames();
    if (!(gfp_mask & __GFP_NORETRY)) {
      unsigned long target = count < 64 ? 64 : count;
      if (target > 512)
        target = 512;
      linuxkpi_shrink_reclaim(gfp_mask, count);
      if (asc_mm_file_cache_reclaim)
        asc_mm_file_cache_reclaim(target);
    }
    if (gfp_mask & __GFP_DMA32)
      phys = (uint64_t)asc_pmm_alloc_pages_range(count, 0, 0x100000000ULL);
    else
      phys = (uint64_t)asc_pmm_alloc_pages(count);
  }
  if (!phys) {
    static unsigned long fails;
    unsigned long n = __atomic_add_fetch(&fails, 1, __ATOMIC_RELAXED);
    /* Rate-limit: first 8 failures carry order/gfp/free pages so the next
     * green-flash run correlates symptom to allocator pressure. */
    if (n <= 8) {
      klogf("[KPI-PAGE] alloc_pages order=%u count=%lu gfp=0x%x DMA32=%d "
            "failed (fail #%lu, free=%lu)\n",
            order, (unsigned long)count, (unsigned)gfp_mask,
            !!(gfp_mask & __GFP_DMA32), n,
            (unsigned long)asc_pmm_get_free_pages());
    }
    return NULL;
  }

  /* Raw descriptor walks below (and in callers such as TTM) require every
   * chunk of this block to be backed, not just the head's chunk.  Reserve
   * the whole range before changing any allocation metadata. */
  unsigned long first = phys >> PAGE_SHIFT;
  if (!page_model_ready || first >= max_pfn || count > max_pfn - first) {
    asc_pmm_free_pages((void *)phys, count);
    return NULL;
  }
  unsigned long last_chunk = (first + count - 1) / CHUNK_PAGES;
  for (unsigned long chunk = first / CHUNK_PAGES; chunk <= last_chunk; chunk++) {
    if (!page_chunk_alloc(chunk)) {
      /* Published chunks are shared, permanent mem_map storage.  Keep them
       * for later lookups, but return the entire data block on failure. */
      asc_pmm_free_pages((void *)phys, count);
      return NULL;
    }
  }

  page = pfn_to_page(first);
  if (!page) {
    asc_pmm_free_pages((void *)phys, count);
    return NULL;
  }

  head = page;
  if (gfp_mask & __GFP_COMP)
    page_set_order(head, order);
  else
    page_set_order_plain(head, order);
  atomic_set(&head->_refcount, 1);
  atomic_set(&head->_mapcount, -1);
  head->mapping = NULL;
  head->private = 0;
  head->index = 0;

  if (gfp_mask & __GFP_ZERO)
    __builtin_memset(page_address(head), 0, count << PAGE_SHIFT);

  if (gpu_diag_resource_enabled())
    gpu_diag_record(GPU_DIAG_PAGE_ALLOC, head, phys, count,
                    gfp_mask, atomic_read(&head->_refcount), 0);
  return head;
}

struct page *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order) {
  (void)nid;
  return alloc_pages(gfp_mask, order);
}

/* The final reference has already been consumed by the caller. */
static void page_release_block(struct page *head, unsigned int order) {
  void *phys;

  head->mapping = NULL;
  head->private = 0;
  __ClearPageHead(head);
  head->compound_order = 0;

  phys = (void *)((uint64_t)head->pfn << PAGE_SHIFT);
  if (gpu_diag_resource_enabled())
    gpu_diag_record(GPU_DIAG_PAGE_FREE, head, (uint64_t)phys,
                    1UL << order, head->cache_flags,
                    atomic_read(&head->_refcount), 0);
  asc_pmm_free_pages(phys, 1UL << order);
}

void __free_pages(struct page *page, unsigned int order) {
  struct page *head;

  if (!page)
    return;
  head = compound_head(page);
  if (atomic_dec_and_test(&head->_refcount))
    page_release_block(head, order);
}

void put_page(struct page *page) {
  struct page *head = compound_head(page);

  if (atomic_dec_and_test(&head->_refcount))
    page_release_block(head, compound_order(head));
}

void put_pages_list(struct list_head *pages) {
  struct page *page, *tmp;

  list_for_each_entry_safe(page, tmp, pages, lru) {
    list_del(&page->lru);
    put_page(page);
  }
}

void free_pages(unsigned long addr, unsigned int order) {
  if (addr == 0)
    return;
  /* __get_free_pages() returns the HHDM virtual address, not a PFN.
   * Shifting that address directly produces an out-of-range descriptor and
   * silently drops the free (notably KFD signal/CWSR backing allocations). */
  __free_pages(phys_to_page((phys_addr_t)(addr - asc_pmm_get_hhdm_offset())),
               order);
}

void split_page(struct page *page, unsigned int order) {
  struct page *head = compound_head(page);
  unsigned long count = 1UL << order;

  __ClearPageHead(head);
  head->compound_order = 0;

  for (unsigned long i = 0; i < count; i++) {
    struct page *p = head + i;
    __ClearPageTail(p);
    p->compound_head = p;
    atomic_set(&p->_refcount, 1);
    atomic_set(&p->_mapcount, -1);
  }
}

/* ------------------------------------------------------------------------- */
/* alloc_pages_exact (returns a virtually contiguous region)                  */
/* ------------------------------------------------------------------------- */

void *alloc_pages_exact(size_t size, gfp_t gfp_mask) {
  unsigned int order = get_order(size);
  struct page *page = alloc_pages(gfp_mask | __GFP_ZERO, order);

  if (!page)
    return NULL;
  return page_address(page);
}

void free_pages_exact(void *virt, size_t size) {
  unsigned int order = get_order(size);

  if (!virt)
    return;
  __free_pages(phys_to_page((phys_addr_t)((uint64_t)virt -
                                           asc_pmm_get_hhdm_offset())),
               order);
}
