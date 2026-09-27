/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_GFP_H
#define ASCENT_LINUX_GFP_H

/*
 * LinuxKPI — gfp.h
 * GFP (Get Free Pages) flag constants.  No allocation logic lives here;
 * these flags are passed to kmalloc/kzalloc in slab.h which maps them onto
 * the native heap allocator.
 */

#include <linux/types.h>

typedef unsigned int gfp_t;

/* ── Modifier flags (individual bits) ───────────────────────────────────── */

#define __GFP_DMA           ((__force gfp_t)0x01u)  /* DMA-able memory */
#define __GFP_HIGHMEM       ((__force gfp_t)0x02u)  /* High memory zone */
#define __GFP_DMA32         ((__force gfp_t)0x04u)  /* 32-bit DMA-able */
#define __GFP_MOVABLE       ((__force gfp_t)0x08u)  /* Movable page */
#define __GFP_RECLAIMABLE   ((__force gfp_t)0x10u)  /* Reclaimable */
#define __GFP_HIGH          ((__force gfp_t)0x20u)  /* Emergency pool */
#define __GFP_IO            ((__force gfp_t)0x40u)  /* Allow I/O */
#define __GFP_FS            ((__force gfp_t)0x80u)  /* Allow filesystem calls */
#define __GFP_ZERO          ((__force gfp_t)0x100u) /* Zero the allocation */
#define __GFP_ATOMIC        ((__force gfp_t)0x200u) /* Atomic allocation */
#define __GFP_DIRECT_RECLAIM ((__force gfp_t)0x400u) /* May call reclaim */
#define __GFP_KSWAPD_RECLAIM ((__force gfp_t)0x800u) /* kswapd may reclaim */
#define __GFP_NOWARN        ((__force gfp_t)0x1000u) /* Suppress alloc failure warnings */
#define __GFP_RETRY_MAYFAIL ((__force gfp_t)0x2000u) /* Retry but may fail */
#define __GFP_NOFAIL        ((__force gfp_t)0x4000u) /* Retry until success */
#define __GFP_NORETRY       ((__force gfp_t)0x8000u) /* Don't retry */
#define __GFP_ACCOUNT       ((__force gfp_t)0x10000u) /* Account to memcg */
#define __GFP_HARDWALL      ((__force gfp_t)0x20000u) /* Enforce NUMA cpuset */
#define __GFP_THISNODE      ((__force gfp_t)0x40000u) /* No fallback to other nodes */
#define __GFP_RECLAIM       (__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM)

/* Needed to suppress sparse __force warnings on plain int usage */
#ifndef __force
#define __force
#endif

/* ── Composite GFP masks (the ones drivers actually use) ─────────────────── */

#define GFP_ATOMIC      (__GFP_HIGH | __GFP_ATOMIC | __GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL      (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_NOWAIT      (__GFP_KSWAPD_RECLAIM)
#define GFP_NOIO        (__GFP_RECLAIM)
#define GFP_NOFS        (__GFP_RECLAIM | __GFP_IO)
#define GFP_USER        (__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA         (__GFP_DMA)
#define GFP_DMA32       (__GFP_DMA32)
#define GFP_HIGHUSER    (GFP_USER | __GFP_HIGHMEM)
#define GFP_TRANSHUGE   (GFP_HIGHUSER | __GFP_MOVABLE)

/* Convenience: zero-initialised kernel allocation */
#define GFP_KERNEL_ZERO (GFP_KERNEL | __GFP_ZERO)

/* ── Flag test helpers ───────────────────────────────────────────────────── */

static inline int gfpflags_allow_blocking(gfp_t gfp)
{
    return !!(gfp & __GFP_DIRECT_RECLAIM);
}

static inline int gfp_pfmemalloc_allowed(gfp_t gfp)
{
    return !!(gfp & __GFP_HIGH);
}

#endif /* ASCENT_LINUX_GFP_H */
