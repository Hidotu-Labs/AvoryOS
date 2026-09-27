/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_LOG2_H
#define ASCENT_LINUX_LOG2_H

/* LinuxKPI — log2.h: integer logarithm and power-of-2 helpers */

#include <linux/types.h>

/*
 * ilog2(n) — floor(log2(n)) for n >= 1.
 * Uses GCC's __builtin_clz family which compile to single BSR instructions.
 */
static __attribute__((always_inline)) inline int ilog2(unsigned long n)
{
    return (int)(sizeof(unsigned long) * 8 - 1) - __builtin_clzl(n);
}

static __attribute__((always_inline)) inline int ilog2_u32(u32 n)
{
    return 31 - __builtin_clz(n);
}

static __attribute__((always_inline)) inline int ilog2_u64(u64 n)
{
    return 63 - __builtin_clzll(n);
}

/*
 * roundup_pow_of_two(n) — smallest power of 2 >= n.
 * Undefined for n == 0.
 */
static __attribute__((always_inline)) inline unsigned long
roundup_pow_of_two(unsigned long n)
{
    if (n == 0)
        return 1;
    if ((n & (n - 1)) == 0)
        return n;                       /* already a power of two */
    return 1UL << (ilog2(n) + 1);
}

static __attribute__((always_inline)) inline unsigned long
rounddown_pow_of_two(unsigned long n)
{
    return 1UL << ilog2(n);
}

/* True if n is an exact power of two (n > 0). */
#define is_power_of_2(n) (((n) != 0) && (((n) & ((n) - 1)) == 0))

#endif /* ASCENT_LINUX_LOG2_H */
