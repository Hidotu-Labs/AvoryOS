#ifndef __AVORY_LINUXKPI_HIGHMEM_H
#define __AVORY_LINUXKPI_HIGHMEM_H

/* AvoryOS overlay for <linux/highmem.h>.
 *
 * Every page is permanently mapped through the HHDM, so the kmap family is a
 * direct page_address() lookup.  Upstream's header (and highmem-internal.h)
 * builds atomic kmaps, pagefault/migrate disabling and KMSAN hooks that the
 * kernel does not model; this keeps only the API imported code calls. */

#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/types.h>

/* Upstream highmem.h includes mm.h; restore that chain so TUs that need the
 * page model (TTM's ttm_pool.c among them) get it the same way.  mm.h's own
 * include of this header is include-guarded, so the cycle is safe. */
#include <linux/mm.h>

struct page;

void *page_address(const struct page *page);

static inline void *kmap(struct page *page) {
  return page_address(compound_head(page));
}
static inline void kunmap(struct page *page) { (void)page; }

static inline void *kmap_local_page(struct page *page) {
  return page_address(compound_head(page));
}
static inline void *kmap_local_page_prot(struct page *page, pgprot_t prot) {
  (void)prot;
  return page_address(compound_head(page));
}
static inline void *kmap_local(struct page *page) {
  return page_address(compound_head(page));
}
static inline void kunmap_local(const void *addr) { (void)addr; }

/* Upstream highmem.h helpers; kept here because AvoryOS replaces the stock
 * header.  All pages are HHDM-mapped, so no flush is needed (x86 coherent).
 * VM_BUG_ON bounds checks are omitted (no VM debugging). */
static inline void memcpy_from_page(char *to, struct page *page, size_t offset,
                                    size_t len) {
  char *from = kmap_local_page(page);

  __builtin_memcpy(to, from + offset, len);
  kunmap_local(from);
}

static inline void memcpy_to_page(struct page *page, size_t offset,
                                  const char *from, size_t len) {
  char *to = kmap_local_page(page);

  __builtin_memcpy(to + offset, from, len);
  kunmap_local(to);
}

static inline void memzero_page(struct page *page, size_t offset, size_t len) {
  char *addr = kmap_local_page(page);

  __builtin_memset(addr + offset, 0, len);
  kunmap_local(addr);
}

static inline void *kmap_atomic(struct page *page) {
  return page_address(compound_head(page));
}
static inline void *kmap_atomic_prot(struct page *page, unsigned long prot) {
  (void)prot;
  return page_address(compound_head(page));
}
static inline void kunmap_atomic(void *addr) { (void)addr; }

/* No highmem on x86_64: these are plain copies in permanently-mapped memory. */
void clear_highpage(struct page *page);
void copy_highpage(struct page *to, struct page *from);

#endif /* __AVORY_LINUXKPI_HIGHMEM_H */
