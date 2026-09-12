/* LinuxKPI scatterlist allocator (header: linux/scatterlist.h).
 *
 * Deliberately simpler than upstream lib/scatterlist.c: tables are single
 * contiguous arrays (no chaining), but the page_link tag layout matches, so
 * sg_next()/sg_is_last()/for_each_sg() behave identically.  Import the
 * upstream file when a driver needs sg_miter or chain allocation. */

#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>

/* ── page iterator (mirrors lib/scatterlist.c) ──────────────────────────── */

static int sg_page_count(struct scatterlist *sg) {
  return (int)(PAGE_ALIGN((unsigned long)sg->offset + sg->length) >>
               PAGE_SHIFT);
}

void __sg_page_iter_start(struct sg_page_iter *piter,
                          struct scatterlist *sglist, unsigned int nents,
                          unsigned long pgoffset) {
  piter->__pg_advance = 0;
  piter->__nents = nents;
  piter->sg = sglist;
  piter->sg_pgoffset = (unsigned int)pgoffset;
}

bool __sg_page_iter_next(struct sg_page_iter *piter) {
  if (!piter->__nents || !piter->sg)
    return false;

  piter->sg_pgoffset += (unsigned int)piter->__pg_advance;
  piter->__pg_advance = 1;

  while (piter->sg_pgoffset >= (unsigned int)sg_page_count(piter->sg)) {
    piter->sg_pgoffset -= (unsigned int)sg_page_count(piter->sg);
    piter->sg = sg_next(piter->sg);
    if (!--piter->__nents || !piter->sg)
      return false;
  }
  return true;
}

bool __sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter) {
  return __sg_page_iter_next(&dma_iter->base);
}

int sg_alloc_table_from_pages_segment(struct sg_table *sgt,
                                      struct page **pages,
                                      unsigned int n_pages,
                                      unsigned long offset,
                                      unsigned long size,
                                      unsigned int max_segment,
                                      gfp_t gfp_mask) {
  /* The native tables are one sg per contiguous run; max_segment only
   * matters for DMA engines with a segment limit (none here yet). */
  (void)max_segment;
  return sg_alloc_table_from_pages(sgt, pages, n_pages, offset, size,
                                   gfp_mask);
}

void sg_init_table(struct scatterlist *sgl, unsigned int nents) {
  if (!nents)
    return;
  __builtin_memset(sgl, 0, sizeof(*sgl) * nents);
  sgl[nents - 1].page_link = SG_END;
}

void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen) {
  sg_init_table(sg, 1);
  sg_set_buf(sg, buf, buflen);
}

int sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp_mask) {
  struct scatterlist *sgl;

  if (!nents)
    return -EINVAL;

  sgl = kcalloc(nents, sizeof(*sgl), gfp_mask);
  if (!sgl)
    return -ENOMEM;

  sg_init_table(sgl, nents);
  table->sgl = sgl;
  table->nents = table->orig_nents = nents;
  return 0;
}

int sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages,
                              unsigned int n_pages, unsigned long offset,
                              unsigned long size, gfp_t gfp_mask) {
  unsigned long page_off, left;
  unsigned int nents, i, page_idx;
  struct scatterlist *sg;
  int ret;

  if (!pages || !size || n_pages == 0)
    return -EINVAL;

  /* One entry per page touched; simple and correct, if not maximally
   * compact. */
  page_off = offset & (PAGE_SIZE - 1);
  left = size;
  nents = 0;
  while (left) {
    unsigned long chunk = left < PAGE_SIZE - page_off ? left
                                                      : PAGE_SIZE - page_off;
    nents++;
    left -= chunk;
    page_off = 0;
  }
  page_idx = (unsigned int)(offset >> PAGE_SHIFT);
  if (page_idx + nents > n_pages)
    return -EINVAL;

  ret = sg_alloc_table(sgt, nents, gfp_mask);
  if (ret)
    return ret;

  page_off = offset & (PAGE_SIZE - 1);
  left = size;
  i = 0;
  for (sg = sgt->sgl; i < nents; i++, sg = sg_next(sg)) {
    unsigned long chunk =
        left < PAGE_SIZE - page_off ? left : PAGE_SIZE - page_off;
    sg_set_page(sg, pages[page_idx], (unsigned int)chunk,
                (unsigned int)page_off);
    left -= chunk;
    page_off = 0;
    page_idx++;
  }
  return 0;
}

void sg_free_table(struct sg_table *table) {
  if (table->sgl) {
    kfree(table->sgl);
    table->sgl = NULL;
  }
  table->nents = table->orig_nents = 0;
}

unsigned int sg_nents_for_len(struct scatterlist *sg, u64 len) {
  unsigned int nents = 0;
  u64 total = 0;

  if (len == 0)
    return 0;

  for (; sg; sg = sg_next(sg)) {
    nents++;
    total += sg->length;
    if (total >= len)
      return nents;
  }
  return 0;
}
