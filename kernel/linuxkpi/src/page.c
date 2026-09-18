/* Linux page model for AvoryOS.
 *
 * Implements the Linux page API described in <linux/mm.h> over the native
 * buddy allocator (kernel/src/mm/pmm.c) with a sparse mem_map:
 *
 *   - Physical memory is divided into 128 MB sections.  Each section owns a
 *     lazily allocated array of `struct page` (2 MB for 32768 pages at 64
 *     bytes each, one order-9 buddy allocation).
 *   - `struct page::pfn` is stored in the descriptor, so page_to_pfn() needs
 *     no section arithmetic.
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

#include <linuxkpi/native_mm.h>

#define SECTION_SHIFT 27 /* 128 MB */
#define PAGES_PER_SECTION (1UL << (SECTION_SHIFT - PAGE_SHIFT))
#define SECTIONS_FOR(phys) (((phys) >> SECTION_SHIFT) + 2)

#define VMAP_WINDOW_BASE 0xFFFFC00000000000ULL
#define VMAP_WINDOW_END 0xFFFFE00000000000ULL

/* Symbols expected by imported x86 headers (asm/page_64.h).  The direct-map
 * base is the runtime HHDM offset; phys_base only matters for __pa() of kernel
 * text, which no LinuxKPI code uses today. */
unsigned long max_pfn;
unsigned long page_offset_base = 0xFFFF800000000000UL;
unsigned long phys_base;
unsigned long vmalloc_base = VMAP_WINDOW_BASE;
unsigned long vmemmap_base;
unsigned long physmem_end;

static struct page **mem_section;
static unsigned long mem_section_count;
static DEFINE_SPINLOCK(page_section_lock);

static bool page_model_ready;

/* ------------------------------------------------------------------------- */
/* mem_map                                                                    */
/* ------------------------------------------------------------------------- */

static struct page *page_section_alloc(unsigned long section) {
  unsigned long flags;
  struct page *map;

  spin_lock_irqsave(&page_section_lock, flags);
  map = mem_section[section];
  if (!map) {
    size_t bytes = PAGES_PER_SECTION * sizeof(struct page);
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = (uint64_t)asc_pmm_alloc_pages(pages);

    if (phys) {
      map = (struct page *)(phys + asc_pmm_get_hhdm_offset());
      __builtin_memset(map, 0, bytes);
      for (unsigned long i = 0; i < PAGES_PER_SECTION; i++) {
        map[i].pfn = section * PAGES_PER_SECTION + i;
        atomic_set(&map[i]._refcount, 0);
        atomic_set(&map[i]._mapcount, -1);
      }
      __atomic_store_n(&mem_section[section], map, __ATOMIC_RELEASE);
    }
  }
  spin_unlock_irqrestore(&page_section_lock, flags);
  return map;
}

struct page *pfn_to_page(unsigned long pfn) {
  unsigned long section;

  if (!page_model_ready || pfn >= max_pfn)
    return NULL;

  section = pfn >> (SECTION_SHIFT - PAGE_SHIFT);
  if (section >= mem_section_count)
    return NULL;

  struct page *map = __atomic_load_n(&mem_section[section], __ATOMIC_ACQUIRE);
  if (!map)
    map = page_section_alloc(section);
  if (!map)
    return NULL;

  return &map[pfn & (PAGES_PER_SECTION - 1)];
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
  uint64_t total = asc_pmm_get_total_memory();
  uint64_t hhdm = asc_pmm_get_hhdm_offset();
  unsigned long sections = SECTIONS_FOR(total);
  size_t bytes = sections * sizeof(struct page *);
  size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t phys;

  if (page_model_ready)
    return;

  phys = (uint64_t)asc_pmm_alloc_pages(pages);
  if (!phys)
    return;

  mem_section = (struct page **)(phys + hhdm);
  __builtin_memset(mem_section, 0, bytes);
  mem_section_count = sections;
  page_offset_base = hhdm;
  max_pfn = total >> PAGE_SHIFT;
  physmem_end = total;
  page_model_ready = true;

  /* Section maps are allocated lazily by pfn_to_page() the first time a page
   * in that section is handed out, so a 4 GiB machine pays only for the
   * sections that actually carry Linux-managed pages (a 128 MB section costs
   * 2 MB at 64 bytes per page).  The eager alternative built all 34 sections
   * (~68 MiB) at boot even though the boot-time desktop touches a handful;
   * see the "~150 MB used after the KPI tests" report.  Driver tests that
   * track PMM free-page deltas warm up before sampling, which absorbs the
   * one-time map allocation of a newly touched section. */
}

/* ------------------------------------------------------------------------- */
/* Allocation                                                                 */
/* ------------------------------------------------------------------------- */

static void page_set_order(struct page *head, unsigned int order) {
  struct page *p = head;

  for (unsigned int i = 0; i < order; i++) {
    unsigned long nr = 1UL << i;
    for (unsigned long j = 0; j < nr; j++) {
      struct page *tail = &p[(1UL << i) + j];
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
    struct page *p = &head[i];

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

  if (!phys)
    return NULL;

  page = pfn_to_page(phys >> PAGE_SHIFT);
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

  return head;
}

struct page *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order) {
  (void)nid;
  return alloc_pages(gfp_mask, order);
}

void __free_pages(struct page *page, unsigned int order) {
  struct page *head;
  void *phys;

  if (!page)
    return;

  head = compound_head(page);
  if (!atomic_dec_and_test(&head->_refcount))
    return; /* still referenced */

  head->mapping = NULL;
  head->private = 0;
  __ClearPageHead(head);
  head->compound_order = 0;

  phys = (void *)((uint64_t)head->pfn << PAGE_SHIFT);
  asc_pmm_free_pages(phys, 1UL << order);
}

void put_page(struct page *page) {
  struct page *head = compound_head(page);

  if (atomic_dec_and_test(&head->_refcount))
    __free_pages(head, compound_order(head));
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
  __free_pages(pfn_to_page(addr >> PAGE_SHIFT), order);
}

void split_page(struct page *page, unsigned int order) {
  struct page *head = compound_head(page);
  unsigned long count = 1UL << order;

  __ClearPageHead(head);
  head->compound_order = 0;

  for (unsigned long i = 0; i < count; i++) {
    struct page *p = &head[i];
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
