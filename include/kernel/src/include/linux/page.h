/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_PAGE_H
#define ASCENT_LINUX_PAGE_H

/*
 * LinuxKPI — page.h
 * Minimal struct page abstraction backed by AscentOS's PMM.
 *
 * On a real Linux system each physical page has a struct page in the
 * mem_map array.  Here we use a lightweight opaque wrapper: struct page
 * just holds the physical address.  This is sufficient for amdgpu's TTM
 * layer which never dereferences page->flags or page->mapping.
 *
 * The HHDM identity mapping means:
 *   page_address(page)  → (void *)(phys + hhdm_offset)
 *   virt_to_page(virt)  → page whose phys = (virt - hhdm_offset)
 */

#include <linux/types.h>
#include "mm/pmm.h"    /* PAGE_SIZE, pmm_alloc_page, pmm_free_page,
                          pmm_get_hhdm_offset, pmm_incref, pmm_decref */

/* ── Core page descriptor ────────────────────────────────────────────────── */

struct page {
    u64 phys;       /* Physical address of this page */
    int ref_count;  /* Simple reference count (not atomic here) */
};

/* ── Conversion helpers (HHDM identity mapping) ──────────────────────────── */

static inline void *page_to_virt(const struct page *pg)
{
    return (void *)(uintptr_t)(pg->phys + pmm_get_hhdm_offset());
}

static inline u64 page_to_phys(const struct page *pg)
{
    return pg->phys;
}

static inline unsigned long page_to_pfn(const struct page *pg)
{
    return (unsigned long)(pg->phys / PAGE_SIZE);
}

/* ── Allocate / free ─────────────────────────────────────────────────────── */

/*
 * alloc_page(gfp) — allocate one physical page, return a struct page *.
 * The struct page itself is allocated from the heap.
 */
#include <linux/gfp.h>
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *memset(void *s, int c, size_t n);

static inline struct page *alloc_page(gfp_t gfp)
{
    struct page *pg = (struct page *)kmalloc(sizeof(struct page));
    if (!pg)
        return NULL;
    void *frame = pmm_alloc_page();
    if (!frame) {
        kfree(pg);
        return NULL;
    }
    pg->phys      = (u64)(uintptr_t)frame - pmm_get_hhdm_offset();
    pg->ref_count = 1;
    if (gfp & __GFP_ZERO)
        memset(frame, 0, PAGE_SIZE);
    return pg;
}

static inline struct page *alloc_pages(gfp_t gfp, unsigned int order)
{
    /* order = log2(n_pages); allocate 2^order pages */
    size_t count = 1u << order;
    struct page *pg = (struct page *)kmalloc(sizeof(struct page));
    if (!pg)
        return NULL;
    void *frame = pmm_alloc_pages(count);
    if (!frame) {
        kfree(pg);
        return NULL;
    }
    pg->phys      = (u64)(uintptr_t)frame - pmm_get_hhdm_offset();
    pg->ref_count = 1;
    if (gfp & __GFP_ZERO)
        memset(frame, 0, count * PAGE_SIZE);
    return pg;
}

static inline void __free_page(struct page *pg)
{
    if (!pg)
        return;
    void *virt = page_to_virt(pg);
    pmm_free_page(virt);
    kfree(pg);
}

static inline void __free_pages(struct page *pg, unsigned int order)
{
    if (!pg)
        return;
    void *virt = page_to_virt(pg);
    pmm_free_pages(virt, 1u << order);
    kfree(pg);
}

/* ── Reference counting ──────────────────────────────────────────────────── */

static inline void get_page(struct page *pg)
{
    if (pg)
        pg->ref_count++;
}

static inline void put_page(struct page *pg)
{
    if (!pg)
        return;
    if (--pg->ref_count <= 0)
        __free_page(pg);
}

/* ── Address helpers ─────────────────────────────────────────────────────── */

/*
 * page_address(pg) — kernel virtual address of the page.
 */
static inline void *page_address(const struct page *pg)
{
    return page_to_virt(pg);
}

/*
 * virt_to_page(virt) — create a transient struct page for a HHDM address.
 * NOTE: returns a heap-allocated struct page; caller must kfree when done.
 * For temporary lookups, prefer page_to_virt on an existing page.
 */
static inline struct page *virt_to_page(const void *virt)
{
    struct page *pg = (struct page *)kmalloc(sizeof(struct page));
    if (!pg)
        return NULL;
    pg->phys      = (u64)(uintptr_t)virt - pmm_get_hhdm_offset();
    pg->ref_count = 1;
    return pg;
}

/*
 * pfn_to_page(pfn) — page descriptor for a physical frame number.
 */
static inline struct page *pfn_to_page(unsigned long pfn)
{
    struct page *pg = (struct page *)kmalloc(sizeof(struct page));
    if (!pg)
        return NULL;
    pg->phys      = (u64)pfn * PAGE_SIZE;
    pg->ref_count = 1;
    return pg;
}

/* ── Page order helpers ──────────────────────────────────────────────────── */

/* compound_order: order of a compound page (we don't support compound
 * pages, always 0) */
static inline unsigned int compound_order(const struct page *pg)
{
    (void)pg;
    return 0;
}

/* page_size: byte size of a (potentially compound) page */
static inline unsigned long page_size(const struct page *pg)
{
    return PAGE_SIZE << compound_order(pg);
}

/* ── Convenience macros ──────────────────────────────────────────────────── */

#define nth_page(pg, n)     pfn_to_page(page_to_pfn(pg) + (n))

/* page_shift: log2(PAGE_SIZE) */
#define PAGE_SHIFT  12
#define PAGE_MASK   (~((u64)PAGE_SIZE - 1))

#endif /* ASCENT_LINUX_PAGE_H */
