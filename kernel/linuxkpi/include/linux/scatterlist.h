#ifndef __AVORY_LINUXKPI_SCATTERLIST_H
#define __AVORY_LINUXKPI_SCATTERLIST_H

/* AvoryOS overlay for <linux/scatterlist.h>.
 *
 * Layout follows upstream (a tagged `page_link` word so `sg_next()` can walk
 * chains and find the end of a table); the allocator and DMA helpers are
 * implemented in linuxkpi/src/scatterlist.c / dma-mapping.c rather than by
 * importing lib/scatterlist.c.  Divergence: no sg_miter/highmem surface yet
 * (import it with the first driver that needs copy helpers). */

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/string.h>

struct scatterlist {
  unsigned long page_link;
  unsigned int offset;
  unsigned int length;
  dma_addr_t dma_address;
  unsigned int dma_length;
};

struct sg_table {
  struct scatterlist *sgl;
  unsigned int nents;
  unsigned int orig_nents;
};

/* page_link tag bits (upstream values). */
#define SG_CHAIN 0x01UL
#define SG_END 0x02UL

#define sg_is_chain(sg) ((sg)->page_link & SG_CHAIN)
#define sg_is_last(sg) ((sg)->page_link & SG_END)
#define sg_chain_ptr(sg)                                                      \
  ((struct scatterlist *)((sg)->page_link & ~(SG_CHAIN | SG_END)))

static inline struct page *sg_page(struct scatterlist *sg) {
  return (struct page *)(sg->page_link & ~(SG_CHAIN | SG_END));
}

static inline void sg_set_page(struct scatterlist *sg, struct page *page,
                               unsigned int len, unsigned int offset) {
  unsigned long page_link = sg->page_link & (SG_CHAIN | SG_END);

  /* Preserve SG_CHAIN/SG_END tags like upstream sg_assign_page(). */
  sg->page_link = page_link | (unsigned long)page;
  sg->offset = offset;
  sg->length = len;
}

static inline void sg_set_buf(struct scatterlist *sg, const void *buf,
                              unsigned int buflen) {
  sg_set_page(sg, virt_to_page(buf), buflen, offset_in_page(buf));
}

static inline void sg_set_dma_address(struct scatterlist *sg,
                                      dma_addr_t dma_address) {
  sg->dma_address = dma_address;
}

/* Upstream scatterlist.h exposes these as lvalues; amdgpu assigns through
 * them (sg_dma_address(sg) = addr).  Macros, not functions. */
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)

static inline void sg_dma_mark_bus_address(struct scatterlist *sg) {}

static inline struct scatterlist *sg_next(struct scatterlist *sg) {
  if (sg_is_last(sg))
    return NULL;

  sg++;
  if (unlikely(sg_is_chain(sg)))
    sg = sg_chain_ptr(sg);
  return sg;
}

static inline struct scatterlist *sg_last(struct scatterlist *s,
                                          unsigned int nents) {
  struct scatterlist *ret = &s[--nents];
  while (sg_is_chain(ret))
    ret = sg_chain_ptr(ret);
  return ret;
}

static inline unsigned int sg_nents(struct scatterlist *sg) {
  unsigned int nents = 0;

  for (; sg; sg = sg_next(sg))
    nents++;
  return nents;
}

unsigned int sg_nents_for_len(struct scatterlist *sg, u64 len);

#define for_each_sg(sgl, sg, nents, i)                                        \
  for (i = 0, sg = (sgl); i < (nents); i++, sg = sg_next(sg))

/* Note: the table parameter is named sgt, not sgl; using sgl would shadow
 * the member name in (sgt)->sgl during macro expansion. */
#define for_each_sgtable_sg(sgt, sg, i)                                       \
  for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
#define for_each_sgtable_dma_sg(sgt, sg, i)                                   \
  for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)

/* Page iterator (upstream layout; the helpers are implemented over our sg
 * tables in linuxkpi/src/scatterlist.c, matching lib/scatterlist.c). */
struct sg_page_iter {
  struct scatterlist *sg;
  unsigned int sg_pgoffset;

  unsigned int __nents;
  int __pg_advance;
};

static inline struct page *sg_page_iter_page(struct sg_page_iter *piter) {
  return nth_page(sg_page(piter->sg), piter->sg_pgoffset);
}

void __sg_page_iter_start(struct sg_page_iter *piter,
                          struct scatterlist *sglist, unsigned int nents,
                          unsigned long pgoffset);
bool __sg_page_iter_next(struct sg_page_iter *piter);

#define for_each_sg_page(sglist, piter, nents, pgoffset)                       \
  for (__sg_page_iter_start((piter), (sglist), (nents), (pgoffset));           \
       __sg_page_iter_next(piter);)

#define for_each_sgtable_page(sgt, piter, pgoffset)                            \
  for_each_sg_page((sgt)->sgl, piter, (sgt)->orig_nents, pgoffset)

/* DMA page iterator: same walk, but sg_page_iter_dma_address() reports the
 * mapped address instead of the physical page. */
struct sg_dma_page_iter {
  struct sg_page_iter base;
};

bool __sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter);

static inline dma_addr_t
sg_page_iter_dma_address(struct sg_dma_page_iter *dma_iter) {
  return sg_dma_address(dma_iter->base.sg) +
         (dma_iter->base.sg_pgoffset << PAGE_SHIFT);
}

#define for_each_sg_dma_page(sglist, dma_iter, dma_nents, pgoffset)            \
  for (__sg_page_iter_start(&(dma_iter)->base, (sglist), (dma_nents),          \
                            (pgoffset));                                       \
       __sg_page_iter_dma_next(dma_iter);)

#define for_each_sgtable_dma_page(sgt, dma_iter, pgoffset)                     \
  for_each_sg_dma_page((sgt)->sgl, dma_iter, (sgt)->nents, pgoffset)

void sg_init_table(struct scatterlist *sgl, unsigned int nents);
void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen);

int sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp_mask);
int sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages,
                              unsigned int n_pages, unsigned long offset,
                              unsigned long size, gfp_t gfp_mask);
int sg_alloc_table_from_pages_segment(struct sg_table *sgt,
                                      struct page **pages,
                                      unsigned int n_pages,
                                      unsigned long offset,
                                      unsigned long size,
                                      unsigned int max_segment,
                                      gfp_t gfp_mask);
void sg_free_table(struct sg_table *table);

#endif /* __AVORY_LINUXKPI_SCATTERLIST_H */
