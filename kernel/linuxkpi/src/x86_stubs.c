/* x86 arch entry points that imported Linux code reaches through the stock
 * x86 headers, but whose upstream implementations live in files AvoryOS does
 * not build (arch/x86/mm/pat/memtype.c, arch/x86/lib/clear_page_64.S).
 *
 * AvoryOS maps all RAM through the HHDM as write-back and never programs PAT
 * or MTRRs:
 *   - cachemode2protval() returns the x86 PAT bits for the requested mode so
 *     the pgprot_t values imported code builds are well formed, but nothing
 *     consumes them for CPU mappings (kmap is an HHDM lookup).
 *   - set_pages_*() cache-mode flips are inert; a page's effective caching is
 *     always write-back.  TTM uses them around its pool pages; the VRAM BAR
 *     itself is mapped separately through ioremap().
 *   - clear_page()'s alternatives target memset over the HHDM.
 *
 * Recorded in docs/linuxkpi-gaps.md (Phase 4). */

#include <asm/page.h>
#include <asm/pgtable_types.h>
#include <asm/set_memory.h>
#include <linux/string.h>
#include <linux/types.h>

unsigned long cachemode2protval(enum page_cache_mode pcm) {
  switch (pcm) {
    case _PAGE_CACHE_MODE_UC:
      return _PAGE_PCD | _PAGE_PWT;
    case _PAGE_CACHE_MODE_UC_MINUS:
      return _PAGE_PCD;
    case _PAGE_CACHE_MODE_WC:
      return _PAGE_PWT;
    case _PAGE_CACHE_MODE_WP:
      return _PAGE_PCD | _PAGE_PWT;
    case _PAGE_CACHE_MODE_WB:
    default:
      return 0;
  }
}

pgprot_t pgprot_writecombine(pgprot_t prot) {
  /* Mapping protections built from this are ignored by the HHDM kmap. */
  return prot;
}

void clear_page_orig(void *page) { memset(page, 0, PAGE_SIZE); }
void clear_page_rep(void *page) { memset(page, 0, PAGE_SIZE); }
void clear_page_erms(void *page) { memset(page, 0, PAGE_SIZE); }

int set_pages_wb(struct page *page, int numpages) {
  (void)page;
  (void)numpages;
  return 0;
}

int set_pages_array_uc(struct page **pages, int addrinarray) {
  (void)pages;
  (void)addrinarray;
  return 0;
}

int set_pages_array_wc(struct page **pages, int addrinarray) {
  (void)pages;
  (void)addrinarray;
  return 0;
}

int set_pages_array_wb(struct page **pages, int addrinarray) {
  (void)pages;
  (void)addrinarray;
  return 0;
}
