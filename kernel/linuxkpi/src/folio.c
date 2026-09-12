/* Order-0 folio helpers with no natural home in an imported file.
 *
 * AvoryOS folios wrap single pages, so a folio_batch is an array of order-0
 * folios and releasing it drops one reference per page.  The highpage
 * helpers are plain memory operations because there is no highmem. */

#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/pagevec.h>
#include <linux/string.h>

void __folio_batch_release(struct folio_batch *pvec) {
  unsigned int i;

  for (i = 0; i < pvec->nr; i++)
    put_page(&pvec->folios[i]->page);
  folio_batch_reinit(pvec);
}

void clear_highpage(struct page *page) {
  memset(page_address(page), 0, PAGE_SIZE);
}

void copy_highpage(struct page *to, struct page *from) {
  memcpy(page_address(to), page_address(from), PAGE_SIZE);
}
