/* Small <linux/mm.h> helpers imported code needs beyond the page model in
 * page.c: the sysinfo totals used by TTM's accounting, the page-dirty/
 * accessed helpers used by shmem-backed ttm_tt, and the single init_mm
 * instance that <linux/pgtable.h> macros (pgd_offset_k) reference.
 *
 * AvoryOS has no page reclaim, writeback or page-migration machinery, so
 * set_page_dirty() only maintains the PG_dirty bit the page cache tests and
 * mark_page_accessed() is deliberately inert. */

#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/string.h>
#include <linux/sysinfo.h>
#include <linux/types.h>

#include <linuxkpi/native_mm.h>

/* pgtable.h's pgd_offset_k() expands to pgd_offset(&init_mm, ...); nothing in
 * the imported set walks a real Linux mm, so a zeroed instance is enough. */
struct mm_struct init_mm;

bool set_page_dirty(struct page *page) {
  if (!page)
    return false;
  /* Returns true only when this call transitions the page to dirty, like
   * upstream's TestSetPageDirty contract. */
  return !test_and_set_bit(PG_dirty, &page->flags);
}

void mark_page_accessed(struct page *page) { (void)page; }

void si_meminfo(struct sysinfo *val) {
  if (!val)
    return;
  memset(val, 0, sizeof(*val));
  val->totalram = (unsigned long)(asc_pmm_get_total_memory() / PAGE_SIZE);
  val->freeram = (unsigned long)asc_pmm_get_free_pages_total();
  val->mem_unit = PAGE_SIZE;
}
