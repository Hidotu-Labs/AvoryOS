/* Phase 2 — vmalloc/ioremap/vmap tests.
 *
 * Compiled with the real Linux headers.  Runs in a kthread; the VMAP window
 * is mapped into the kernel PML4 by linuxkpi_vmalloc_init() before any user
 * process exists. */

#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/io.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

/* ── vmalloc/vfree cycles with contents and leak invariant ──────────────── */

static bool test_vmalloc_cycles(void) {
  unsigned long before;

  /* Warm-up: the first allocations grow one-time kernel metadata (vmap_area
   * heap pages, page-table frames for this window), which must not be
   * charged to the leak invariant. */
  for (int i = 0; i < 8; i++) {
    void *w = vmalloc((size_t)(1 + i) * PAGE_SIZE);
    if (!w)
      return false;
    vfree(w);
  }

  before = asc_pmm_get_free_pages();

  for (int iter = 0; iter < 64; iter++) {
    size_t size = (size_t)(1 + (iter % 8)) * PAGE_SIZE;
    unsigned char *p = vmalloc(size);

    if (!p)
      return false;
    if (!is_vmalloc_addr(p))
      return false;

    for (size_t off = 0; off < size; off += PAGE_SIZE + 37)
      p[off] = (unsigned char)(iter + off);
    for (size_t off = 0; off < size; off += PAGE_SIZE + 37)
      if (p[off] != (unsigned char)(iter + off))
        return false;

    vfree(p);
  }

  return asc_pmm_get_free_pages() == before;
}

/* ── a large allocation (exercises many PTEs and >1 area) ───────────────── */

static bool test_vmalloc_large(void) {
  size_t size = 4 * 1024 * 1024;
  unsigned char *p = vmalloc(size);

  if (!p)
    return false;
  if (!is_vmalloc_addr(p))
    return false;

  for (size_t off = 0; off < size; off += PAGE_SIZE)
    p[off] = (unsigned char)(off >> PAGE_SHIFT);

  for (size_t off = 0; off < size; off += PAGE_SIZE)
    if (p[off] != (unsigned char)(off >> PAGE_SHIFT))
      return false;

  vfree(p);
  return true;
}

/* ── vmalloc_to_page aliases the backing pages ──────────────────────────── */

static bool test_vmalloc_to_page(void) {
  size_t size = 3 * PAGE_SIZE;
  unsigned char *p = vmalloc(size);
  struct page *p0, *p1;
  bool ok = true;

  if (!p)
    return false;

  p0 = vmalloc_to_page(p);
  p1 = vmalloc_to_page(p + PAGE_SIZE);
  if (!p0 || !p1 || p0 == p1)
    ok = false;

  if (ok) {
    p[PAGE_SIZE] = 0x5A;
    if (*(unsigned char *)page_address(p1) != 0x5A)
      ok = false;
  }

  vfree(p);
  return ok;
}

/* ── vmap over caller pages, contents alias both ways ───────────────────── */

static bool test_vmap_pages(void) {
  struct page *pages[3];
  void *mapping;
  bool ok = true;

  for (int i = 0; i < 3; i++) {
    pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!pages[i]) {
      ok = false;
      goto out;
    }
  }

  mapping = vmap(pages, 3, VM_MAP, PAGE_KERNEL);
  if (!mapping) {
    ok = false;
    goto out;
  }

  ((unsigned char *)mapping)[0] = 0x11;
  ((unsigned char *)mapping)[PAGE_SIZE] = 0x22;
  ((unsigned char *)mapping)[2 * PAGE_SIZE] = 0x33;

  ok = *(unsigned char *)page_address(pages[0]) == 0x11 &&
       *(unsigned char *)page_address(pages[1]) == 0x22 &&
       *(unsigned char *)page_address(pages[2]) == 0x33;

  vunmap(mapping);

out:
  for (int i = 0; i < 3; i++)
    if (pages[i])
      __free_page(pages[i]);
  return ok;
}

/* ── ioremap maps a physical page and reads/writes through it ───────────── */

static bool test_ioremap(void) {
  struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);
  unsigned char *direct, *mapped;
  bool ok;

  if (!page)
    return false;

  direct = page_address(page);
  mapped = ioremap((phys_addr_t)page_to_phys(page), 2 * PAGE_SIZE);
  if (!mapped) {
    __free_page(page);
    return false;
  }

  direct[0] = 0xAB;
  ok = mapped[0] == 0xAB;
  mapped[1] = 0xCD;
  ok = ok && direct[1] == 0xCD;

  iounmap(mapped);
  __free_page(page);
  return ok;
}

/* ── kv* allocators pick an allocator and kvfree detects it ─────────────── */

static bool test_kvmalloc(void) {
  unsigned long before = asc_pmm_get_free_pages();
  void *small = kvmalloc(256, GFP_KERNEL);
  void *large = kvmalloc(8 * 1024 * 1024, GFP_KERNEL);
  void *v = vmalloc(2 * PAGE_SIZE);

  if (!small || !large || !v)
    return false;

  __builtin_memset(small, 0xAA, 256);
  ((unsigned char *)large)[4 * 1024 * 1024] = 0x55;
  if (((unsigned char *)large)[4 * 1024 * 1024] != 0x55)
    return false;

  /* kvfree must route the explicit vmalloc pointer to vfree(). */
  if (!is_vmalloc_addr(v))
    return false;
  ((unsigned char *)v)[PAGE_SIZE + 5] = 0x42;

  kvfree(small);
  kvfree(large);
  kvfree(v);
  return asc_pmm_get_free_pages() == before;
}

/* ── aggregator ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase2_vmalloc(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"vmalloc/vfree cycles", test_vmalloc_cycles},
      {"vmalloc 4 MB", test_vmalloc_large},
      {"vmalloc_to_page", test_vmalloc_to_page},
      {"vmap/vunmap", test_vmap_pages},
      {"ioremap/iounmap", test_ioremap},
      {"kvmalloc/kvfree", test_kvmalloc},
  };

  klog_puts("[LINUXKPI] Phase 2 vmalloc self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s wrong result\n", tests[i].name);
  }
}
