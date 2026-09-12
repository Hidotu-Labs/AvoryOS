/* Phase 2 — page model + page allocator tests.
 *
 * Compiled with the real Linux headers; reports through the LinuxKPI log
 * bridge.  Runs in a kthread (see linuxkpi/src/boot_tests.c). */

#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

/* ── pfn/page/address round trips ───────────────────────────────────────── */

static bool test_page_roundtrip(void) {
  uint64_t total = asc_pmm_get_total_memory();
  uint64_t hhdm = asc_pmm_get_hhdm_offset();
  uint64_t last_pfn = total >> PAGE_SHIFT;
  unsigned long step = (last_pfn / 64) | 1;

  for (unsigned long pfn = 0; pfn < last_pfn; pfn += step) {
    struct page *page = pfn_to_page(pfn);
    phys_addr_t phys;
    void *addr;

    if (!page)
      return false;
    if (page_to_pfn(page) != pfn)
      return false;
    if (compound_head(page) != page || PageTail(page))
      return false;

    phys = page_to_phys(page);
    if (phys != ((phys_addr_t)pfn << PAGE_SHIFT))
      return false;

    addr = page_address(page);
    if ((uint64_t)addr != (uint64_t)phys + hhdm)
      return false;

    /* virt_to_page(page_address(p)) must return the same descriptor. */
    if (virt_to_page(addr) != page)
      return false;
  }
  return true;
}

/* ── order-N allocation, contents, teardown ─────────────────────────────── */

static bool test_page_orders(void) {
  /* Compound bookkeeping only exists when __GFP_COMP requested it (upstream
   * semantics; TTM's pool deliberately allocates high-order pages without the
   * flag and treats every page independently). */
  for (unsigned int order = 0; order <= 6; order++) {
    unsigned long count = 1UL << order;

    for (int iter = 0; iter < 32; iter++) {
      struct page *page =
          alloc_pages(GFP_KERNEL | __GFP_ZERO | __GFP_COMP, order);
      unsigned char *p;

      if (!page)
        return false;

      p = page_address(page);
      if (!p)
        return false;

      if (order > 0) {
        if (!PageHead(page) || PageTail(page))
          return false;
        if (page->compound_order != order)
          return false;
        for (unsigned long i = 1; i < count; i++) {
          if (!PageTail(&page[i]))
            return false;
          if (compound_head(&page[i]) != page)
            return false;
        }
      }
      if (page_ref_count(page) != 1)
        return false;

      if (p[0] != 0)
        return false;
      p[0] = 0xA5;
      p[(count << PAGE_SHIFT) - 1] = 0x5A;
      if (p[0] != 0xA5 || p[(count << PAGE_SHIFT) - 1] != 0x5A)
        return false;

      __free_pages(page, order);
    }
  }

  /* Without __GFP_COMP every page is independent: no head/tail flags, the
   * block head resolves to itself, and all pages are addressable. */
  for (unsigned int order = 1; order <= 4; order++) {
    unsigned long count = 1UL << order;
    struct page *page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
    unsigned char *p;

    if (!page)
      return false;
    if (PageHead(page) || PageTail(page) || page->compound_order != 0)
      return false;
    for (unsigned long i = 1; i < count; i++) {
      if (PageHead(&page[i]) || PageTail(&page[i]))
        return false;
      if (compound_head(&page[i]) != &page[i])
        return false;
    }

    p = page_address(page);
    p[0] = 0x11;
    p[(count << PAGE_SHIFT) - 1] = 0x22;
    if (p[0] != 0x11 || p[(count << PAGE_SHIFT) - 1] != 0x22)
      return false;

    __free_pages(page, order);
  }
  return true;
}

/* ── refcount torture ───────────────────────────────────────────────────── */

static bool test_page_refcount(void) {
  struct page *page = alloc_page(GFP_KERNEL);
  unsigned long before = asc_pmm_get_free_pages();

  if (!page)
    return false;

  for (int i = 0; i < 1000; i++)
    get_page(page);
  if (page_ref_count(page) != 1001)
    return false;

  /* A refcounted page must survive the intermediate puts. */
  for (int i = 0; i < 1000; i++)
    put_page(page);

  if (atomic_read(&page->_refcount) != 1)
    return false;

  put_page(page);

  /* The block must be back in the allocator. */
  {
    unsigned long after = asc_pmm_get_free_pages();
    if (after != before)
      return false;
  }
  return true;
}

/* ── allocation-size and OOM paths ──────────────────────────────────────── */

static bool test_page_exact(void) {
  void *buf = alloc_pages_exact(3 * PAGE_SIZE + 17, GFP_KERNEL);
  unsigned char *p = buf;

  if (!buf)
    return false;
  p[0] = 1;
  p[3 * PAGE_SIZE + 16] = 2;
  if (p[0] != 1 || p[3 * PAGE_SIZE + 16] != 2)
    return false;
  free_pages_exact(buf, 3 * PAGE_SIZE + 17);

  /* GFP_DMA32 must return physical memory below 4 GB. */
  {
    struct page *page = alloc_pages(GFP_KERNEL | GFP_DMA32, 2);
    if (!page)
      return false;
    if (page_to_phys(page) + (4 * PAGE_SIZE) > 0x100000000ULL)
      return false;
    __free_pages(page, 2);
  }
  return true;
}

/* ── split_page turns a compound block into individual pages ────────────── */

static bool test_page_split(void) {
  struct page *page = alloc_pages(GFP_KERNEL, 3); /* 8 pages */
  unsigned long free_before;
  unsigned long free_after;

  if (!page)
    return false;
  free_before = asc_pmm_get_free_pages();

  split_page(page, 3);
  for (int i = 0; i < 8; i++) {
    if (PageHead(&page[i]) || PageTail(&page[i]))
      return false;
    if (page_ref_count(&page[i]) != 1)
      return false;
  }

  /* Free each split page individually. */
  for (int i = 0; i < 8; i++)
    __free_page(&page[i]);

  free_after = asc_pmm_get_free_pages();
  return free_after == free_before + 8;
}

/* ── aggregate free-page invariant across many alloc/free cycles ────────── */

static bool test_page_leak_invariant(void) {
  unsigned long before = asc_pmm_get_free_pages();

  for (int i = 0; i < 256; i++) {
    struct page *a = alloc_pages(GFP_KERNEL, i % 4);
    struct page *b = alloc_pages(GFP_KERNEL | __GFP_ZERO, 1);

    if (!a || !b)
      return false;
    __free_pages(a, i % 4);
    __free_pages(b, 1);
  }

  return asc_pmm_get_free_pages() == before;
}

/* ── aggregator ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase2_page(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"page pfn/address roundtrip", test_page_roundtrip},
      {"page order alloc/free", test_page_orders},
      {"page refcount", test_page_refcount},
      {"alloc_pages_exact/DMA32", test_page_exact},
      {"split_page", test_page_split},
      {"page free-count invariant", test_page_leak_invariant},
  };

  klog_puts("[LINUXKPI] Phase 2 page self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s wrong result\n", tests[i].name);
  }
}
