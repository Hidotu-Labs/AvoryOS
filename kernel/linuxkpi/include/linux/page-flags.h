#ifndef __AVORY_LINUXKPI_PAGE_FLAGS_H
#define __AVORY_LINUXKPI_PAGE_FLAGS_H

/* AvoryOS overlay for <linux/page-flags.h>.
 *
 * The upstream header is entangled with folios, kasan, page extensions and
 * the SPARSEMEM layout.  This overlay keeps the standard bit numbers (so any
 * code or header that hard-codes them agrees) and provides only the helpers
 * the current import set calls.  PG_tail is an AvoryOS-only bit: unlike
 * upstream, `struct page::compound_head` is a plain pointer and PageTail()
 * says whether it is meaningful. */

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/bitops.h>

enum pageflags {
  PG_locked = 0,  /* Page is locked. Don't touch. */
  PG_writeback = 1,
  PG_referenced = 2,
  PG_uptodate = 3,
  PG_dirty = 4,
  PG_lru = 5,
  PG_head = 6, /* Must be in bit 6 */
  PG_waiters = 7,
  PG_active = 8,
  PG_workingset = 9,
  PG_error = 10,
  PG_slab = 11,
  PG_owner_priv_1 = 12,
  PG_arch_1 = 13,
  PG_reserved = 14,
  PG_private = 15,
  PG_private_2 = 16,
  PG_mappedtodisk = 17,
  PG_reclaim = 18,
  PG_swapbacked = 19,
  PG_unevictable = 20,
  __NR_PAGEFLAGS,

  /* AvoryOS-only: page is a tail page of a compound allocation. */
  PG_tail = __NR_PAGEFLAGS,

  PG_readahead = PG_reclaim,
  PG_checked = PG_owner_priv_1,
  PG_swapcache = PG_owner_priv_1,
};

#define TESTPAGEFLAG(uname, lname, policy)                                    \
  static inline int Page##uname(const struct page *page) {                    \
    return test_bit(PG_##lname, &page->flags);                                \
  }
#define SETPAGEFLAG(uname, lname, policy)                                     \
  static inline void SetPage##uname(struct page *page) {                      \
    set_bit(PG_##lname, &page->flags);                                        \
  }
#define CLEARPAGEFLAG(uname, lname, policy)                                   \
  static inline void ClearPage##uname(struct page *page) {                    \
    clear_bit(PG_##lname, &page->flags);                                      \
  }
#define SET_CLEAR_PAGEFLAGS(uname, lname, policy)                             \
  static inline void __SetPage##uname(struct page *page) {                    \
    __set_bit(PG_##lname, &page->flags);                                      \
  }                                                                           \
  static inline void __ClearPage##uname(struct page *page) {                  \
    __clear_bit(PG_##lname, &page->flags);                                    \
  }

TESTPAGEFLAG(Locked, locked, PF_NO_TAIL)
TESTPAGEFLAG(Writeback, writeback, PF_NO_TAIL)
TESTPAGEFLAG(Referenced, referenced, PF_HEAD)
TESTPAGEFLAG(Dirty, dirty, PF_HEAD)
TESTPAGEFLAG(LRU, lru, PF_HEAD)
TESTPAGEFLAG(Active, active, PF_HEAD)
TESTPAGEFLAG(Workingset, workingset, PF_HEAD)
TESTPAGEFLAG(Error, error, PF_NO_TAIL)
TESTPAGEFLAG(Slab, slab, PF_NO_TAIL)
TESTPAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)
TESTPAGEFLAG(Private, private, PF_NO_COMPOUND)
TESTPAGEFLAG(Private2, private_2, PF_NO_COMPOUND)
TESTPAGEFLAG(Reclaim, reclaim, PF_HEAD)
TESTPAGEFLAG(Unevictable, unevictable, PF_HEAD)
TESTPAGEFLAG(MappedToDisk, mappedtodisk, PF_NO_TAIL)

SETPAGEFLAG(Locked, locked, PF_NO_TAIL)
SETPAGEFLAG(Writeback, writeback, PF_NO_TAIL)
SETPAGEFLAG(Referenced, referenced, PF_HEAD)
SETPAGEFLAG(Dirty, dirty, PF_HEAD)
SETPAGEFLAG(LRU, lru, PF_HEAD)
SETPAGEFLAG(Active, active, PF_HEAD)
SETPAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)
SETPAGEFLAG(Private, private, PF_NO_COMPOUND)
SETPAGEFLAG(Unevictable, unevictable, PF_HEAD)

CLEARPAGEFLAG(Locked, locked, PF_NO_TAIL)
CLEARPAGEFLAG(Writeback, writeback, PF_NO_TAIL)
CLEARPAGEFLAG(Referenced, referenced, PF_HEAD)
CLEARPAGEFLAG(Dirty, dirty, PF_HEAD)
CLEARPAGEFLAG(LRU, lru, PF_HEAD)
CLEARPAGEFLAG(Active, active, PF_HEAD)
CLEARPAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)
CLEARPAGEFLAG(Private, private, PF_NO_COMPOUND)
CLEARPAGEFLAG(Unevictable, unevictable, PF_HEAD)

SET_CLEAR_PAGEFLAGS(Head, head, PF_HEAD)

static inline int PageHead(const struct page *page) {
  return test_bit(PG_head, (unsigned long *)&page->flags);
}

static inline void SetPageHead(struct page *page) {
  set_bit(PG_head, &page->flags);
}

static inline void ClearPageHead(struct page *page) {
  clear_bit(PG_head, &page->flags);
}

static inline int PageTail(const struct page *page) {
  return test_bit(PG_tail, (unsigned long *)&page->flags);
}

static inline void __SetPageTail(struct page *page) {
  __set_bit(PG_tail, &page->flags);
}

static inline void __ClearPageTail(struct page *page) {
  __clear_bit(PG_tail, &page->flags);
}

static inline struct page *compound_head(struct page *page) {
  if (PageTail(page))
    return page->compound_head;
  return page;
}

static inline int PageCompound(struct page *page) {
  return PageHead(page) || PageTail(page);
}

static inline unsigned int compound_order(struct page *page) {
  if (!PageHead(page))
    return 0;
  return page->compound_order;
}

static inline struct page *compound_head_by_tail(struct page *page) {
  return page->compound_head;
}

static inline void set_compound_order(struct page *page, unsigned int order) {
  page->compound_order = (unsigned char)order;
}

static inline void set_page_compound(struct page *page, struct page *head,
                                     unsigned int order) {
  page->compound_head = head;
  __SetPageTail(page);
  (void)order;
}

static inline int PageHighMem(const struct page *page) {
  (void)page;
  return 0;
}

/* PageWaiters is checked by wake_up_page() paths; always false here. */
static inline int PageWaiters(const struct page *page) {
  (void)page;
  return 0;
}

/* ── folio helpers ──────────────────────────────────────────────────────── */
/*
 * AvoryOS folios are order-0, so every folio maps to exactly one head page.
 * The helpers below are the subset imported DRM code calls; page_to_pfn()
 * and put_page() live in mm.h and must not be used from here.
 */

struct folio;

static inline struct folio *page_folio(struct page *page) {
  return (struct folio *)compound_head(page);
}

static inline struct page *folio_page(struct folio *folio, pgoff_t index) {
  (void)index;
  return &folio->page;
}

static inline struct page *folio_file_page(struct folio *folio, pgoff_t index) {
  return folio_page(folio, index);
}

static inline unsigned int folio_nr_pages(struct folio *folio) {
  return 1U << compound_order(&folio->page);
}

static inline struct address_space *folio_mapping(const struct folio *folio) {
  return folio->page.mapping;
}

static inline pgoff_t folio_index(const struct folio *folio) {
  return folio->page.index;
}

static inline void folio_mark_dirty(struct folio *folio) {
  set_bit(PG_dirty, &folio->page.flags);
}

static inline void folio_mark_accessed(struct folio *folio) {
  set_bit(PG_referenced, &folio->page.flags);
}

static inline bool folio_test_dirty(const struct folio *folio) {
  return test_bit(PG_dirty, (unsigned long *)&folio->page.flags);
}

/* Order-0 folios only: a folio is never "large" in AvoryOS. */
static inline bool folio_test_large(const struct folio *folio) {
  (void)folio;
  return false;
}

#endif /* __AVORY_LINUXKPI_PAGE_FLAGS_H */
