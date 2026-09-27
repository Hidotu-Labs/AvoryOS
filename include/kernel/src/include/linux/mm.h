/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_MM_H
#define ASCENT_LINUX_MM_H

/*
 * LinuxKPI — mm.h
 * Page-level address conversion and helper macros.
 * All conversions rely on AscentOS's HHDM identity mapping:
 *   virt = phys + hhdm_offset
 */

#include <linux/page.h>
#include <linux/types.h>
#include <linux/align.h>

/* ── Address conversion ──────────────────────────────────────────────────── */

/*
 * __pa(virt) — virtual (HHDM) address → physical address.
 * Only valid for addresses in the direct-mapped HHDM window.
 */
static inline u64 __pa(const void *virt)
{
    return (u64)(uintptr_t)virt - pmm_get_hhdm_offset();
}

/*
 * __va(phys) — physical address → virtual (HHDM) address.
 */
static inline void *__va(u64 phys)
{
    return (void *)(uintptr_t)(phys + pmm_get_hhdm_offset());
}

/* Simpler macro forms used by some drivers */
#define phys_to_virt(phys)   __va((u64)(phys))
#define virt_to_phys(virt)   __pa((const void *)(virt))

/* ── Page-frame helpers ──────────────────────────────────────────────────── */

/*
 * virt_to_pfn(virt) — PFN for an HHDM virtual address.
 */
static inline unsigned long virt_to_pfn(const void *virt)
{
    return (unsigned long)(__pa(virt) / PAGE_SIZE);
}

/*
 * pfn_to_virt(pfn) — HHDM virtual address for a PFN.
 */
static inline void *pfn_to_virt(unsigned long pfn)
{
    return __va((u64)pfn * PAGE_SIZE);
}

/* ── Size / range helpers ────────────────────────────────────────────────── */

/*
 * PAGE_ALIGN(addr) — already defined in linux/align.h; re-export here.
 */
#ifndef PAGE_ALIGN
#define PAGE_ALIGN(addr)    ALIGN((addr), PAGE_SIZE)
#endif

/* Number of pages needed to cover `size` bytes */
#define PAGE_ALIGN_UP(size) DIV_ROUND_UP((size), PAGE_SIZE)

/* ── Kernel memory range checks ──────────────────────────────────────────── */

/*
 * virt_addr_valid(addr) — true if addr is in the HHDM kernel window.
 * Drivers use this to decide if they can call page_address() on a pointer.
 */
static inline int virt_addr_valid(const void *addr)
{
    unsigned long a = (unsigned long)(uintptr_t)addr;
    /* HHDM window: [HHDM_BASE, VMAP_BASE) */
    return a >= 0xFFFF800000000000UL && a < 0xFFFFC00000000000UL;
}

/*
 * pfn_valid(pfn) — true if the PFN corresponds to managed RAM.
 * We forward to pmm_is_managed.
 */
static inline int pfn_valid(unsigned long pfn)
{
    return pmm_is_managed((u64)pfn * PAGE_SIZE);
}

/* ── Misc ────────────────────────────────────────────────────────────────── */

/*
 * offset_in_page(addr) — byte offset of addr within its page.
 */
#define offset_in_page(addr)    ((unsigned long)(addr) & (PAGE_SIZE - 1))

/* offset_in_folio: same as offset_in_page for us */
#define offset_in_folio         offset_in_page

/*
 * round_up / round_down wrappers for page sizes.
 */
#define round_up_page(x)        PAGE_ALIGN(x)
#define round_down_page(x)      ALIGN_DOWN((x), (unsigned long)PAGE_SIZE)

/* ── VM flags (minimal set used by amdgpu mmap paths) ────────────────────── */
#define VM_READ         0x00000001
#define VM_WRITE        0x00000002
#define VM_EXEC         0x00000004
#define VM_SHARED       0x00000008
#define VM_MAYREAD      0x00000010
#define VM_MAYWRITE     0x00000020
#define VM_MAYEXEC      0x00000040
#define VM_MAYSHARE     0x00000080
#define VM_DONTCOPY     0x00020000
#define VM_DONTEXPAND   0x00040000
#define VM_IO           0x00004000
#define VM_NORESERVE    0x00200000
#define VM_PFNMAP       0x00000400
#define VM_MIXEDMAP     0x10000000
#define VM_HUGETLB      0x00400000
#define VM_DONTDUMP     0x04000000

#endif /* ASCENT_LINUX_MM_H */
