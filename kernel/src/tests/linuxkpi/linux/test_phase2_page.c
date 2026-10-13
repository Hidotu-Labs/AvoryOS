/* Phase 2 — page model + page allocator tests.
 *
 * Compiled with the real Linux headers; reports through the LinuxKPI log
 * bridge.  Runs in a kthread (see linuxkpi/src/boot_tests.c). */

#include <linux/gfp.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/shmem_fs.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

/* ── pfn/page/address round trips ───────────────────────────────────────── */

static bool p2_page_roundtrip_pfn(unsigned long pfn, uint64_t hhdm) {
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
  return virt_to_page(addr) == page;
}

static bool test_page_roundtrip(void) {
  uint64_t total = asc_pmm_get_managed_memory_end();
  uint64_t hhdm = asc_pmm_get_hhdm_offset();
  unsigned long last_pfn = (unsigned long)(total >> PAGE_SHIFT);

  /* pfn_to_page() materializes the 128 MB section's struct page map on first
   * touch and the map is never freed.  The old sweep sampled every ~64 MB of
   * RAM and therefore built a map for every section, leaving the whole sparse
   * mem_map (~136 MiB with the old 128-byte descriptor) resident for the rest
   * of the boot.  A few samples are enough to exercise the sparse lookup: the
   * section arithmetic is identical for every section. */
  const unsigned long samples[] = {0, 1, last_pfn / 2, last_pfn - 2,
                                   last_pfn - 1};

  for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
    if (!p2_page_roundtrip_pfn(samples[i], hhdm))
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
  unsigned long before = asc_pmm_get_free_pages();
  struct page *page = alloc_page(GFP_KERNEL);

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

/* Exercise real PAT mappings and alias visibility, not just the descriptor. */
static bool test_page_cache_types(void) {
  struct page *head = alloc_pages(GFP_KERNEL | __GFP_ZERO, 1);
  if (!head)
    return false;
  struct page *pages[2] = {head, pfn_to_page(page_to_pfn(head) + 1)};
  bool ok = set_pages_array_wc(pages, 2) == 0;
  unsigned long wc = ASC_PAGE_PAT | ASC_PAGE_PCD | ASC_PAGE_PWT;
  for (unsigned int i = 0; i < 2; i++)
    ok = ok && linuxkpi_page_cache_flags(page_to_phys(pages[i])) == wc;
  unsigned char *alias = vmap(pages, 2, VM_MAP, PAGE_KERNEL);
  if (alias) {
    unsigned char *direct = page_address(head);
    direct[0] = 0xa5;
    direct[PAGE_SIZE] = 0x5a;
    __asm__ volatile("mfence" ::: "memory");
    ok = ok && alias[0] == 0xa5 && alias[PAGE_SIZE] == 0x5a;
    alias[1] = 0x33;
    __asm__ volatile("mfence" ::: "memory");
    ok = ok && direct[1] == 0x33;
    vunmap(alias);
  } else {
    ok = false;
  }
  ok = set_pages_array_uc(pages, 2) == 0 && ok;
  for (unsigned int i = 0; i < 2; i++)
    ok = ok && linuxkpi_page_cache_flags(page_to_phys(pages[i])) ==
                 (ASC_PAGE_PCD | ASC_PAGE_PWT);
  ok = set_pages_wb(head, 2) == 0 && ok;
  for (unsigned int i = 0; i < 2; i++)
    ok = ok && linuxkpi_page_cache_flags(page_to_phys(pages[i])) == 0;
  __free_pages(head, 1);
  return ok;
}

/* TTM puts each swap page after copying, then reads it again during swapin.
 * A truncate must drop only the cache reference, retaining a live reader. */
static bool test_shmem_page_lifetime(void) {
  struct file *file = shmem_file_setup("kpi-page-lifetime", PAGE_SIZE, 0);
  bool ok = true;

  if (IS_ERR(file))
    return false;
  for (unsigned int i = 0; i < 32 && ok; i++) {
    struct page *page = shmem_read_mapping_page_gfp(file->f_mapping, 0,
                                                   GFP_KERNEL);
    if (IS_ERR(page)) {
      ok = false;
      break;
    }
    ok = page_count(page) == 2;
    ((unsigned char *)page_address(page))[37] = (unsigned char)i;
    put_page(page);
    ok = ok && page_count(page) == 1;
    struct page *again = shmem_read_mapping_page_gfp(file->f_mapping, 0,
                                                    GFP_KERNEL);
    if (IS_ERR(again)) {
      ok = false;
      break;
    }
    ok = ok && again == page && page_count(again) == 2 &&
         ((unsigned char *)page_address(again))[37] == (unsigned char)i;
    shmem_truncate_range(file->f_inode, 0, -1);
    ok = ok && page_count(again) == 1 && !again->mapping &&
         ((unsigned char *)page_address(again))[37] == (unsigned char)i;
    put_page(again);
    ok = ok && page_count(again) == 0;
  }
  fput(file);
  return ok;
}

void linuxkpi_test_phase2_page(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"page pfn/address roundtrip", test_page_roundtrip},
      {"page order alloc/free", test_page_orders},
      {"page refcount", test_page_refcount},
      {"shmem reader/truncate lifetime", test_shmem_page_lifetime},
      {"alloc_pages_exact/DMA32", test_page_exact},
      {"split_page", test_page_split},
      {"page WC/UC/WB aliases", test_page_cache_types},
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
