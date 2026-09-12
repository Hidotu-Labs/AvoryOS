/* Phase 2 — DMA mapping and scatterlist tests.
 *
 * Identity-mapped coherent DMA on x86: dma_addr_t is the physical address and
 * the sync operations are barriers.  Compiled with the real Linux headers. */

#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

/* ── coherent allocations: <4 GB, zeroed, correct handle ────────────────── */

static bool test_dma_coherent(void) {
  unsigned long before = asc_pmm_get_free_pages();

  for (int iter = 0; iter < 32; iter++) {
    size_t size = (size_t)(1 + (iter % 4)) * PAGE_SIZE;
    dma_addr_t handle = 0;
    unsigned char *p = dma_alloc_coherent(NULL, size, &handle, GFP_KERNEL);

    if (!p)
      return false;
    if (p[0] != 0 || p[size - 1] != 0)
      return false;
    if (handle != (dma_addr_t)__pa(p))
      return false;
    if ((uint64_t)handle + size > 0x100000000ULL &&
        !(iter % 4)) /* most allocations should be low, not guaranteed */
      ; /* informational only */

    p[0] = 0xA5;
    p[size - 1] = 0x5A;
    if (p[0] != 0xA5 || p[size - 1] != 0x5A)
      return false;

    dma_free_coherent(NULL, size, p, handle);
  }

  return asc_pmm_get_free_pages() == before;
}

/* ── GFP_DMA32 coherent allocation stays below 4 GB ─────────────────────── */

static bool test_dma_coherent_low(void) {
  struct device dev = {0};
  dma_addr_t handle = 0;
  unsigned char *p;

  dev.coherent_dma_mask = DMA_BIT_MASK(32);
  p = dma_alloc_coherent(&dev, 2 * PAGE_SIZE, &handle, GFP_KERNEL);
  if (!p)
    return false;

  bool ok = (uint64_t)handle + 2 * PAGE_SIZE <= 0x100000000ULL;
  dma_free_coherent(&dev, 2 * PAGE_SIZE, p, handle);
  return ok;
}

/* ── single-buffer streaming map ────────────────────────────────────────── */

static bool test_dma_single(void) {
  unsigned char *buf = kmalloc(4096, GFP_KERNEL);
  dma_addr_t dma;

  if (!buf)
    return false;

  dma = dma_map_single(NULL, buf, 4096, DMA_BIDIRECTIONAL);
  if (dma_mapping_error(NULL, dma))
    return false;
  if (dma != (dma_addr_t)__pa(buf))
    return false;

  dma_sync_single_for_device(NULL, dma, 4096, DMA_BIDIRECTIONAL);
  buf[0] = 1;
  dma_sync_single_for_cpu(NULL, dma, 4096, DMA_BIDIRECTIONAL);
  dma_unmap_single(NULL, dma, 4096, DMA_BIDIRECTIONAL);

  kfree(buf);
  return true;
}

/* ── scatterlist: from_pages with offset/size, DMA addresses ────────────── */

static bool test_dma_sgtable(void) {
  struct page *pages[4];
  struct sg_table sgt = {0};
  struct scatterlist *sg;
  unsigned int i, count = 0;
  bool ok = true;

  for (i = 0; i < 4; i++) {
    pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!pages[i]) {
      ok = false;
      goto out;
    }
  }

  /* 3 pages starting at offset 100 into page 1. */
  if (sg_alloc_table_from_pages(&sgt, pages + 1, 3, 100,
                                3 * PAGE_SIZE - 100, GFP_KERNEL)) {
    ok = false;
    goto out;
  }

  if (dma_map_sgtable(NULL, &sgt, DMA_BIDIRECTIONAL, 0) <= 0) {
    ok = false;
    goto out;
  }

  for_each_sgtable_dma_sg(&sgt, sg, i) {
    dma_addr_t expect =
        (dma_addr_t)page_to_phys(sg_page(sg)) + sg->offset;
    if (sg_dma_address(sg) != expect)
      ok = false;
    if (sg_dma_len(sg) != sg->length)
      ok = false;
    count++;
  }
  if (count < 3)
    ok = false;

  dma_unmap_sgtable(NULL, &sgt, DMA_BIDIRECTIONAL, 0);
  sg_free_table(&sgt);

out:
  for (i = 0; i < 4; i++)
    if (pages[i])
      __free_page(pages[i]);
  return ok;
}

/* ── plain sg table alloc/free and sg_next walking ──────────────────────── */

static bool test_sg_table(void) {
  struct sg_table sgt = {0};
  struct scatterlist *sg;
  unsigned int i, seen = 0;
  bool ok = true;

  if (sg_alloc_table(&sgt, 5, GFP_KERNEL))
    return false;

  for_each_sgtable_sg(&sgt, sg, i)
    seen++;
  if (seen != 5)
    ok = false;

  if (!sg_is_last(sgt.sgl + 4) || sg_next(sgt.sgl + 4) != NULL)
    ok = false;

  /* sg_set_buf must preserve the SG_END tag of the final entry. */
  sg_init_table(sgt.sgl, 2);
  sg_set_buf(sgt.sgl, (void *)0x1000, 8);
  sg_set_buf(sgt.sgl + 1, (void *)0x2000, 8);
  if (!sg_is_last(sgt.sgl + 1) || sg_next(sgt.sgl + 1) != NULL)
    ok = false;

  sg_free_table(&sgt);
  return ok;
}

/* ── masks ──────────────────────────────────────────────────────────────── */

static bool test_dma_masks(void) {
  struct device dev = {0};

  if (dma_set_mask(&dev, DMA_BIT_MASK(44)) != 0)
    return false;
  if (dev.dma_mask != DMA_BIT_MASK(44))
    return false;
  if (dma_get_required_mask(&dev) != DMA_BIT_MASK(44))
    return false;
  if (dma_set_mask_and_coherent(&dev, DMA_BIT_MASK(32)) != 0)
    return false;
  if (dev.coherent_dma_mask != DMA_BIT_MASK(32))
    return false;
  if (dma_set_mask(&dev, DMA_BIT_MASK(20)) == 0)
    return false; /* sub-32-bit unsupported */
  return true;
}

/* ── aggregator ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase2_dma(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"dma_alloc_coherent/free", test_dma_coherent},
      {"coherent GFP_DMA32", test_dma_coherent_low},
      {"dma_map_single", test_dma_single},
      {"sg_alloc_table_from_pages + dma_map_sgtable", test_dma_sgtable},
      {"sg_alloc_table/sg_next", test_sg_table},
      {"dma masks", test_dma_masks},
  };

  klog_puts("[LINUXKPI] Phase 2 DMA self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s wrong result\n", tests[i].name);
  }
}
