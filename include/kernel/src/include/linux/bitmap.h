/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_BITMAP_H
#define ASCENT_LINUX_BITMAP_H

/* LinuxKPI — bitmap.h: heap-allocated bitmap operations */

#include <linux/bitops.h>
#include <linux/types.h>

/* Round nbits up to the number of unsigned longs needed */
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define BITMAP_SIZE(n)   (BITS_TO_LONGS(n) * sizeof(unsigned long))

/*
 * Pull in the real allocator and string declarations so bitmap.h never
 * emits duplicate or conflicting forward declarations in TUs that already
 * include mm/heap.h or lib/string.h.
 */
#include "mm/heap.h"
#include "lib/string.h"

/* Allocate a zero-cleared bitmap for 'nbits' bits. Returns NULL on OOM. */
static inline unsigned long *bitmap_zalloc(unsigned int nbits)
{
    size_t sz = BITMAP_SIZE(nbits);
    unsigned long *bm = (unsigned long *)kmalloc(sz);
    if (bm)
        memset(bm, 0, sz);
    return bm;
}

static inline void bitmap_free(unsigned long *bm)
{
    kfree(bm);
}

/* Set / clear all bits in [0, nbits) */
static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
    unsigned int longs = BITS_TO_LONGS(nbits);
    for (unsigned int i = 0; i < longs; i++)
        dst[i] = ~0UL;
    /* Clear any padding bits in the last word */
    unsigned int rem = nbits % BITS_PER_LONG;
    if (rem)
        dst[longs - 1] = (1UL << rem) - 1UL;
}

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
    memset(dst, 0, BITMAP_SIZE(nbits));
}

/* Logical operations */
static inline void bitmap_or(unsigned long *dst,
                              const unsigned long *src1,
                              const unsigned long *src2,
                              unsigned int nbits)
{
    unsigned int longs = BITS_TO_LONGS(nbits);
    for (unsigned int i = 0; i < longs; i++)
        dst[i] = src1[i] | src2[i];
}

static inline void bitmap_and(unsigned long *dst,
                               const unsigned long *src1,
                               const unsigned long *src2,
                               unsigned int nbits)
{
    unsigned int longs = BITS_TO_LONGS(nbits);
    for (unsigned int i = 0; i < longs; i++)
        dst[i] = src1[i] & src2[i];
}

static inline void bitmap_andnot(unsigned long *dst,
                                  const unsigned long *src1,
                                  const unsigned long *src2,
                                  unsigned int nbits)
{
    unsigned int longs = BITS_TO_LONGS(nbits);
    for (unsigned int i = 0; i < longs; i++)
        dst[i] = src1[i] & ~src2[i];
}

/* True if all bits in [0, nbits) are zero */
static inline int bitmap_empty(const unsigned long *src, unsigned int nbits)
{
    return find_first_bit(src, nbits) == nbits;
}

/* True if all bits in [0, nbits) are set */
static inline int bitmap_full(const unsigned long *src, unsigned int nbits)
{
    return find_first_zero_bit(src, nbits) == nbits;
}

/* Count set bits — only counts bits in [0, nbits), ignoring padding */
static inline unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
    unsigned int longs = BITS_TO_LONGS(nbits);
    unsigned int w = 0;
    for (unsigned int i = 0; i < longs - 1; i++)
        w += hweight_long(src[i]);
    /* mask padding bits in the last word */
    if (longs > 0) {
        unsigned int rem = nbits % BITS_PER_LONG;
        unsigned long last = src[longs - 1];
        if (rem)
            last &= (1UL << rem) - 1UL;
        w += hweight_long(last);
    }
    return w;
}

/* Convenience: set/clear single bit */
#define bitmap_set_bit(bm, n)   set_bit((n), (bm))
#define bitmap_clear_bit(bm, n) clear_bit((n), (bm))
#define bitmap_test_bit(bm, n)  test_bit((n), (bm))

/* Iterate over set bits in a bitmap */
#define for_each_set_bit_in_bitmap(bit, bm, nbits) \
    for_each_set_bit((bit), (bm), (nbits))

#endif /* ASCENT_LINUX_BITMAP_H */
