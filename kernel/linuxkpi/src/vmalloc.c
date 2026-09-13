/* LinuxKPI vmalloc/ioremap over the native VMAP window.
 *
 * Layout: [0xFFFFC00000000000, 0xFFFFE00000000000) (see native VMAP_BASE /
 * KERNEL_HEAP_BASE).  Areas are tracked in an address-sorted list and served
 * first-fit; vmalloc() allocates fresh pages from the Linux page allocator,
 * vmap() references caller-owned pages, and ioremap() maps a physical range.
 * Everything is mapped into the kernel PML4.
 *
 * Process address spaces clone the kernel half of the PML4, so the window's
 * top-level entries are pre-populated by linuxkpi_vmalloc_init() before any
 * user process exists.  A kernel mapping created after a process was cloned
 * would allocate tables (PDPT/PD/PT) under a shared PML4 entry, which is why
 * the pre-population matters: within an existing PDPT, later PT/PD contents
 * are shared, so only brand-new PML4 entries are a problem.  Sub-allocation
 * beyond the pre-populated entries is therefore unsupported.
 */

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/string.h>

#include <linuxkpi/native_mm.h>

#define KPI_VMAP_BASE 0xFFFFC00000000000ULL
#define KPI_VMAP_END 0xFFFFE00000000000ULL

/* Pre-populate tables across the whole window: one entry per 512 GB PML4
 * slot.  Walking the window at init costs a few hundred page-table frames. */
#define KPI_VMAP_PREPOP_STRIDE 0x8000000000ULL /* 512 GB */
#define KPI_VMAP_PREPOP_ENTRIES 64ULL

struct vmap_area {
  uint64_t start;
  uint64_t end;
  unsigned long nr_pages;
  struct page **pages;   /* page array; owned only when owns_pages      */
  bool owns_pages;       /* vmalloc: free pages + array; vmap: neither  */
  bool is_ioremap;       /* physical mapping, no page array             */
  struct vmap_area *next;
  struct vmap_area *prev;
};

static struct vmap_area vmap_head = {
    .start = KPI_VMAP_BASE,
    .end = KPI_VMAP_BASE,
    .next = &vmap_head,
    .prev = &vmap_head,
};

static DEFINE_SPINLOCK(vmap_lock);
static bool vmap_ready;

/* ── address allocator ──────────────────────────────────────────────────── */

static struct vmap_area *vmap_area_find_locked(uint64_t addr) {
  for (struct vmap_area *a = vmap_head.next; a != &vmap_head; a = a->next) {
    if (addr >= a->start && addr < a->end)
      return a;
  }
  return NULL;
}

static void vmap_area_insert_locked(struct vmap_area *area) {
  struct vmap_area *pos = vmap_head.next;
  struct vmap_area *prev = &vmap_head;

  while (pos != &vmap_head && pos->start < area->start) {
    prev = pos;
    pos = pos->next;
  }

  area->prev = prev;
  area->next = pos;
  prev->next = area;
  pos->prev = area;
}

static void vmap_area_remove_locked(struct vmap_area *area) {
  area->prev->next = area->next;
  area->next->prev = area->prev;
}

/* First fit in [KPI_VMAP_BASE, KPI_VMAP_END). */
static uint64_t vmap_area_reserve_locked(size_t size) {
  uint64_t cursor = KPI_VMAP_BASE;
  struct vmap_area *pos = vmap_head.next;

  size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

  while (pos != &vmap_head) {
    if (pos->start > cursor && pos->start - cursor >= size)
      break;
    if (pos->start > cursor)
      cursor = pos->start;
    cursor = (pos->end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    pos = pos->next;
  }

  if (KPI_VMAP_END - cursor < size)
    return 0;
  return cursor;
}

/* ── mapping helpers ────────────────────────────────────────────────────── */

static void vmap_map_page(uint64_t va, uint64_t pa, unsigned long flags) {
  asc_vmm_map_page(asc_vmm_get_kernel_pml4(), va, pa,
                   ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_NX | flags);
}

static void vmap_unmap_page(uint64_t va) {
  asc_vmm_unmap_page(asc_vmm_get_kernel_pml4(), va);
  asc_invlpg(va);
}

static struct vmap_area *vmap_area_alloc(size_t size, unsigned long nr_pages,
                                         bool is_ioremap) {
  struct vmap_area *area;
  unsigned long flags;

  area = kzalloc(sizeof(*area), GFP_KERNEL);
  if (!area)
    return NULL;

  spin_lock_irqsave(&vmap_lock, flags);
  uint64_t start = vmap_area_reserve_locked(size);
  if (!start) {
    spin_unlock_irqrestore(&vmap_lock, flags);
    kfree(area);
    return NULL;
  }
  area->start = start;
  area->end = start + ((size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
  area->nr_pages = nr_pages;
  area->is_ioremap = is_ioremap;
  vmap_area_insert_locked(area);
  spin_unlock_irqrestore(&vmap_lock, flags);
  return area;
}

/* ── vmalloc ────────────────────────────────────────────────────────────── */

static void *vmap_do_alloc(unsigned long size, struct page **given_pages,
                           bool is_ioremap, unsigned long map_flags) {
  struct vmap_area *area;
  unsigned long nr_pages;
  uint64_t va;

  if (size == 0)
    return ZERO_SIZE_PTR;

  nr_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  area = vmap_area_alloc(size, nr_pages, is_ioremap);
  if (!area)
    return NULL;

  if (given_pages) {
    /* vmap: caller-owned pages, caller-owned array (never freed here). */
    area->pages = given_pages;
    area->owns_pages = false;
    for (unsigned long i = 0; i < nr_pages; i++)
      vmap_map_page(area->start + i * PAGE_SIZE, page_to_phys(given_pages[i]),
                    map_flags);
    return (void *)area->start;
  }

  if (!is_ioremap) {
    area->pages = kcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
    if (!area->pages)
      goto fail_area;
    area->owns_pages = true;

    for (unsigned long i = 0; i < nr_pages; i++) {
      area->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
      if (!area->pages[i])
        goto fail_pages;
      vmap_map_page(area->start + i * PAGE_SIZE, page_to_phys(area->pages[i]),
                    map_flags);
    }
    return (void *)area->start;
  }

  return (void *)area->start;

fail_pages:
  for (unsigned long i = 0; i < nr_pages; i++) {
    if (!area->pages[i])
      break;
    vmap_unmap_page(area->start + i * PAGE_SIZE);
    __free_page(area->pages[i]);
  }
  kfree(area->pages);
fail_area:
  {
    unsigned long flags;
    spin_lock_irqsave(&vmap_lock, flags);
    vmap_area_remove_locked(area);
    spin_unlock_irqrestore(&vmap_lock, flags);
  }
  kfree(area);
  return NULL;
}

void *vmalloc(unsigned long size) {
  return vmap_do_alloc(size, NULL, false, 0);
}

void *vzalloc(unsigned long size) { return vmalloc(size); }

void *vmalloc_user(unsigned long size) { return vmalloc(size); }

void *vmalloc_node(unsigned long size, int node) {
  (void)node;
  return vmalloc(size);
}

void *vmap(struct page **pages, unsigned int count, unsigned long flags,
           pgprot_t prot) {
  (void)flags;
  (void)prot;
  if (!pages || !count)
    return NULL;
  /* Do not take references: like Linux, the caller keeps ownership. */
  return vmap_do_alloc((unsigned long)count << PAGE_SHIFT, pages, false, 0);
}

/* Find the area owning `addr`; caller must hold vmap_lock. */
static struct vmap_area *vmap_area_at(uint64_t addr) {
  return vmap_area_find_locked(addr);
}

void vfree(const void *addr) {
  struct vmap_area *area;
  unsigned long flags;

  if (!addr || ZERO_OR_NULL_PTR(addr))
    return;
  if (!is_vmalloc_addr(addr))
    return;

  spin_lock_irqsave(&vmap_lock, flags);
  area = vmap_area_at((uint64_t)addr);
  if (area)
    vmap_area_remove_locked(area);
  spin_unlock_irqrestore(&vmap_lock, flags);

  if (!area)
    return;

  for (unsigned long i = 0; i < area->nr_pages; i++) {
    vmap_unmap_page(area->start + i * PAGE_SIZE);
    asc_vmm_free_empty_tables(asc_vmm_get_kernel_pml4(),
                              area->start + i * PAGE_SIZE);
  }

  /* vmap() pages are owned by the caller; vmalloc() pages are ours. */
  if (area->owns_pages) {
    for (unsigned long i = 0; i < area->nr_pages; i++)
      if (area->pages[i])
        __free_page(area->pages[i]);
    kfree(area->pages);
  }
  kfree(area);
}

void vunmap(const void *addr) { vfree(addr); }

struct page *vmalloc_to_page(const void *addr) {
  uint64_t va = (uint64_t)addr;
  struct vmap_area *area;
  unsigned long flags;
  struct page *page = NULL;

  if (!is_vmalloc_addr(addr))
    return pfn_to_page(__pa(addr) >> PAGE_SHIFT);

  spin_lock_irqsave(&vmap_lock, flags);
  area = vmap_area_at(va);
  if (area && area->pages) {
    unsigned long idx = (va - area->start) / PAGE_SIZE;
    if (idx < area->nr_pages)
      page = area->pages[idx];
  }
  spin_unlock_irqrestore(&vmap_lock, flags);
  return page;
}

unsigned long vmalloc_to_pfn(const void *addr) {
  struct page *page = vmalloc_to_page(addr);
  return page ? page_to_pfn(page) : 0;
}

/* ── ioremap family ─────────────────────────────────────────────────────── */

/* Boot-test support: remember which physical ranges were ioremapped, even
 * after iounmap(), so a probe that failed and unwound can still be checked
 * for "reached its MMIO setup" (amdgpu maps BAR5 in amdgpu_device_init before
 * early init and unmaps it again on the error path).  Diagnostic only: the
 * table keeps the first KPI_IOREMAP_TRACE mappings and drops later ones. */
#define KPI_IOREMAP_TRACE 16
static resource_size_t kpi_ioremap_trace_off[KPI_IOREMAP_TRACE];
static unsigned long kpi_ioremap_trace_size[KPI_IOREMAP_TRACE];
static unsigned int kpi_ioremap_trace_count;

static void kpi_ioremap_trace_add(resource_size_t offset, unsigned long size) {
  unsigned long flags;

  spin_lock_irqsave(&vmap_lock, flags);
  if (kpi_ioremap_trace_count < KPI_IOREMAP_TRACE) {
    kpi_ioremap_trace_off[kpi_ioremap_trace_count] = offset;
    kpi_ioremap_trace_size[kpi_ioremap_trace_count] = size;
    kpi_ioremap_trace_count++;
  }
  spin_unlock_irqrestore(&vmap_lock, flags);
}

bool linuxkpi_ioremap_was_mapped(resource_size_t offset, unsigned long size) {
  unsigned long flags;
  bool hit = false;
  unsigned int i;

  if (!size)
    return false;

  spin_lock_irqsave(&vmap_lock, flags);
  for (i = 0; i < kpi_ioremap_trace_count; i++) {
    resource_size_t start = kpi_ioremap_trace_off[i];
    resource_size_t end = start + kpi_ioremap_trace_size[i];

    if (offset >= start && offset < end) {
      hit = true;
      break;
    }
  }
  spin_unlock_irqrestore(&vmap_lock, flags);
  return hit;
}

static void *ioremap_flags(resource_size_t offset, unsigned long size,
                           unsigned long pte_flags) {
  struct vmap_area *area;
  uint64_t base;
  unsigned long first_off;
  unsigned long nr;

  if (!size)
    return NULL;

  base = offset & ~(uint64_t)(PAGE_SIZE - 1);
  first_off = offset & (PAGE_SIZE - 1);
  nr = (first_off + size + PAGE_SIZE - 1) / PAGE_SIZE;

  area = vmap_area_alloc(size, nr, true);
  if (!area)
    return NULL;

  for (unsigned long i = 0; i < nr; i++)
    vmap_map_page(area->start + i * PAGE_SIZE, base + i * PAGE_SIZE, pte_flags);

  kpi_ioremap_trace_add(base, (unsigned long)nr * PAGE_SIZE);

  return (void *)(area->start + first_off);
}

/* Cache-mode encodings for this kernel's PAT table.
 *
 * PTE bits select an IA32_PAT entry as (PAT << 2) | (PWT << 1) | PCD.
 * The native kernel keeps the architectural default table and only programs
 * entry 7 (PAT|PWT|PCD) to WC (`cpu_pat_init()`), unlike Linux, which
 * reprograms entry 1 as well.  So on AvoryOS:
 *
 *   PCD|PWT         -> entry 3, UC   (the MTRR-proof uncached type; what
 *                                     every native MMIO driver uses)
 *   PAT|PCD|PWT     -> entry 7, WC   (same bits fb/drm use)
 *   PWT             -> entry 1, WT
 *   0               -> entry 0, WB
 *
 * ioremap() must be true UC, not PCD-only: PCD alone is UC-, which an MTRR
 * (or a WB MTRR default type) can override back to cacheable, and a cached
 * BAR yields stale register reads and delayed doorbell writes.  ioremap_wc()
 * must use entry 7; PAT alone selects entry 4, which is WB. */
void *ioremap(resource_size_t offset, unsigned long size) {
  return ioremap_flags(offset, size, ASC_PAGE_PCD | ASC_PAGE_PWT);
}

void *ioremap_wc(resource_size_t offset, unsigned long size) {
  return ioremap_flags(offset, size,
                       ASC_PAGE_PAT | ASC_PAGE_PCD | ASC_PAGE_PWT);
}

void *ioremap_wt(resource_size_t offset, unsigned long size) {
  return ioremap_flags(offset, size, ASC_PAGE_PWT);
}

void *ioremap_cache(resource_size_t offset, unsigned long size) {
  return ioremap_flags(offset, size, 0);
}

void iounmap(volatile void __iomem *addr) {
  uint64_t va = (uint64_t)addr & ~(uint64_t)(PAGE_SIZE - 1);
  struct vmap_area *area;
  unsigned long flags;

  if (!addr)
    return;

  spin_lock_irqsave(&vmap_lock, flags);
  area = vmap_area_at(va);
  if (area && area->is_ioremap)
    vmap_area_remove_locked(area);
  else
    area = NULL;
  spin_unlock_irqrestore(&vmap_lock, flags);

  if (!area)
    return;

  for (unsigned long i = 0; i < area->nr_pages; i++) {
    vmap_unmap_page(area->start + i * PAGE_SIZE);
    asc_vmm_free_empty_tables(asc_vmm_get_kernel_pml4(),
                              area->start + i * PAGE_SIZE);
  }
  kfree(area);
}

/* ── memremap ───────────────────────────────────────────────────────────── */

void *memremap(resource_size_t offset, size_t size, unsigned long flags) {
  if (flags & MEMREMAP_WC)
    return ioremap_wc(offset, size);
  if (flags & MEMREMAP_WT)
    return ioremap_wt(offset, size);
  return ioremap_cache(offset, size);
}

void memunmap(void *addr) { iounmap((volatile void __iomem *)addr); }

/* ── kvmalloc ───────────────────────────────────────────────────────────── */

void *kvmalloc_node(size_t size, gfp_t flags, int node) {
  void *ret;

  (void)node;
  ret = kmalloc(size, flags);
  if (!ret)
    ret = vmalloc(size);
  return ret;
}

void *kvmalloc(size_t size, gfp_t flags) {
  return kvmalloc_node(size, flags, -1);
}

void *kvzalloc(size_t size, gfp_t flags) {
  return kvmalloc_node(size, flags | __GFP_ZERO, -1);
}

void *kvmalloc_array(size_t num, size_t size, gfp_t flags) {
  size_t bytes;

  if (__builtin_mul_overflow(num, size, &bytes))
    return NULL;
  if (bytes == 0)
    return ZERO_SIZE_PTR;
  return kvmalloc(bytes, flags);
}

void *kvcalloc(size_t num, size_t size, gfp_t flags) {
  return kvmalloc_array(num, size, flags | __GFP_ZERO);
}

void kvfree(const void *addr) {
  if (!addr || ZERO_OR_NULL_PTR(addr))
    return;
  if (is_vmalloc_addr(addr))
    vfree(addr);
  else
    __kpi_kfree(addr);
}

void kvfree_sensitive(const void *addr, size_t len) {
  if (!addr || ZERO_OR_NULL_PTR(addr))
    return;
  if (is_vmalloc_addr(addr)) {
    __builtin_memset((void *)addr, 0, len);
    vfree(addr);
  } else {
    __kpi_kfree_sensitive(addr);
  }
}

/* Upstream mm/util.c: grow-only reallocation of a kvmalloc() buffer. */
void *kvrealloc(const void *p, size_t oldsize, size_t newsize, gfp_t flags) {
  void *newp;

  if (oldsize >= newsize)
    return (void *)p;
  newp = kvmalloc(newsize, flags);
  if (!newp)
    return NULL;
  __builtin_memcpy(newp, p, oldsize);
  kvfree(p);
  return newp;
}

/* ── init ───────────────────────────────────────────────────────────────── */

/* Map the shared zero page at each 512 GB boundary of the VMAP window so the
 * PML4 entries (and their PDPTs) exist in the kernel PML4 before user
 * processes are cloned.  The mappings are read-only and unmap nothing; they
 * are a small, permanent table reservation, not address-space use. */
void linuxkpi_vmalloc_init(void) {
  uint64_t zero_phys;

  if (vmap_ready)
    return;
  vmap_ready = true;

  zero_phys = asc_pmm_get_zero_page_phys();
  if (!zero_phys)
    return;

  for (uint64_t i = 0; i < KPI_VMAP_PREPOP_ENTRIES; i++) {
    uint64_t va = KPI_VMAP_BASE + i * KPI_VMAP_PREPOP_STRIDE;
    if (va >= KPI_VMAP_END)
      break;
    asc_vmm_map_page(asc_vmm_get_kernel_pml4(), va, zero_phys,
                     ASC_PAGE_PRESENT | ASC_PAGE_NX);
  }
}
