/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_VMALLOC_H
#define ASCENT_LINUX_VMALLOC_H

/*
 * LinuxKPI — vmalloc.h
 * Virtually-contiguous allocation backed by PMM pages mapped into the
 * VMAP window (0xFFFFC00000000000).
 *
 * Implemented in linuxkpi/vmalloc.c (Phase 2).
 * For Phase 1c we only declare the interface so dependent headers compile.
 */

#include <linux/types.h>
#include <linux/gfp.h>

/* ── Allocators ──────────────────────────────────────────────────────────── */

/* vmalloc: allocate size bytes in the VMAP region */
void *vmalloc(unsigned long size);

/* vzalloc: vmalloc + zero-fill */
void *vzalloc(unsigned long size);

/* vmalloc_user: vmalloc + zero-fill + user-mappable */
static inline void *vmalloc_user(unsigned long size)
{
    return vzalloc(size);
}

/* vmalloc_node: NUMA-aware vmalloc (we have no NUMA; ignore node) */
static inline void *vmalloc_node(unsigned long size, int node)
{
    (void)node;
    return vmalloc(size);
}

/* vfree: free a vmalloc allocation */
void vfree(const void *addr);

/* ── Test helpers ────────────────────────────────────────────────────────── */

/*
 * is_vmalloc_addr(x) — true if x was allocated via vmalloc
 * (i.e. lies in the VMAP window).
 */
#define VMAP_BASE_ADDR  0xFFFFC00000000000ULL
#define VMAP_END_ADDR   0xFFFFE00000000000ULL

static inline int is_vmalloc_addr(const void *x)
{
    unsigned long addr = (unsigned long)(uintptr_t)x;
    return addr >= VMAP_BASE_ADDR && addr < VMAP_END_ADDR;
}

/* ── Size query ──────────────────────────────────────────────────────────── */

/*
 * vmalloc_to_page(addr) — look up the struct page for a vmalloc address.
 * Declared here; implemented in linuxkpi/vmalloc.c (Phase 2).
 */
struct page;
struct page *vmalloc_to_page(const void *addr);

/*
 * vmalloc_to_pfn(addr) — PFN for the physical page backing a vmalloc addr.
 */
static inline unsigned long vmalloc_to_pfn(const void *addr)
{
    struct page *pg = vmalloc_to_page(addr);
    if (!pg)
        return 0;
    /* page_to_pfn is in linux/page.h but we avoid the circular include
     * by doing the arithmetic directly. */
    extern unsigned long page_to_pfn(const struct page *pg);
    return page_to_pfn(pg);
}

#endif /* ASCENT_LINUX_VMALLOC_H */
