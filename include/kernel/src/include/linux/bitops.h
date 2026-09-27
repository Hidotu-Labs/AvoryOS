/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_BITOPS_H
#define ASCENT_LINUX_BITOPS_H

/* LinuxKPI — bitops.h: atomic and non-atomic bit operations */

#include <linux/types.h>

/* ── Non-atomic bit manipulation on plain unsigned longs ─────────────────── */

#define BIT(n)          (1UL << (n))
#define BIT_ULL(n)      (1ULL << (n))
#define BIT_MASK(n)     (1UL << ((n) % (8 * sizeof(unsigned long))))
#define BIT_WORD(n)     ((n) / (8 * sizeof(unsigned long)))

#define BITS_PER_LONG       64
#define BITS_PER_LONG_LONG  64
#define BITS_PER_BYTE       8

/* ── Bit scan helpers (GCC builtins → BSF/BSR) ───────────────────────────── */

/* Find First Set bit (1-based, 0 if value is 0) */
static __attribute__((always_inline)) inline int ffs(int x)
{
    return __builtin_ffs(x);
}

/* Find Last Set bit (0-based position of MSB; undefined for 0) */
static __attribute__((always_inline)) inline unsigned int __fls(unsigned long x)
{
    return (unsigned int)(8 * sizeof(x) - 1) - (unsigned int)__builtin_clzl(x);
}

static __attribute__((always_inline)) inline unsigned int __ffs(unsigned long x)
{
    return (unsigned int)__builtin_ctzl(x);
}

/* fls: 1-based most-significant set bit (returns 0 for x==0) */
static __attribute__((always_inline)) inline int fls(unsigned int x)
{
    return x ? (int)(8 * sizeof(x)) - __builtin_clz(x) : 0;
}

static __attribute__((always_inline)) inline int fls64(u64 x)
{
    return x ? 64 - __builtin_clzll(x) : 0;
}

/* ── Non-atomic bitfield ops on a bitmap (array of unsigned longs) ───────── */

static inline void set_bit(unsigned int nr, volatile unsigned long *addr)
{
    addr[BIT_WORD(nr)] |= BIT_MASK(nr);
}

static inline void clear_bit(unsigned int nr, volatile unsigned long *addr)
{
    addr[BIT_WORD(nr)] &= ~BIT_MASK(nr);
}

static inline int test_bit(unsigned int nr, const volatile unsigned long *addr)
{
    return (int)((addr[BIT_WORD(nr)] >> (nr % BITS_PER_LONG)) & 1UL);
}

static inline int test_and_set_bit(unsigned int nr, volatile unsigned long *addr)
{
    int old = test_bit(nr, addr);
    set_bit(nr, addr);
    return old;
}

static inline int test_and_clear_bit(unsigned int nr, volatile unsigned long *addr)
{
    int old = test_bit(nr, addr);
    clear_bit(nr, addr);
    return old;
}

/* ── Bitmap scan helpers ─────────────────────────────────────────────────── */

/* Find first zero bit in [0, size); returns size if all set */
static inline unsigned long find_first_zero_bit(const unsigned long *addr,
                                                 unsigned long size)
{
    unsigned long words = (size + BITS_PER_LONG - 1) / BITS_PER_LONG;
    for (unsigned long i = 0; i < words; i++) {
        if (addr[i] != ~0UL) {
            unsigned long bit = __ffs(~addr[i]);
            unsigned long pos = i * BITS_PER_LONG + bit;
            return pos < size ? pos : size;
        }
    }
    return size;
}

/* Find first set bit in [0, size); returns size if none set */
static inline unsigned long find_first_bit(const unsigned long *addr,
                                            unsigned long size)
{
    unsigned long words = (size + BITS_PER_LONG - 1) / BITS_PER_LONG;
    for (unsigned long i = 0; i < words; i++) {
        if (addr[i]) {
            unsigned long bit = __ffs(addr[i]);
            unsigned long pos = i * BITS_PER_LONG + bit;
            return pos < size ? pos : size;
        }
    }
    return size;
}

/* Find next set bit at or after 'offset' in [0, size) */
static inline unsigned long find_next_bit(const unsigned long *addr,
                                           unsigned long size,
                                           unsigned long offset)
{
    if (offset >= size)
        return size;

    unsigned long word_idx = offset / BITS_PER_LONG;
    unsigned long bit_idx  = offset % BITS_PER_LONG;
    unsigned long tmp = addr[word_idx] >> bit_idx;

    if (tmp) {
        unsigned long pos = word_idx * BITS_PER_LONG + bit_idx + __ffs(tmp);
        return pos < size ? pos : size;
    }

    word_idx++;
    unsigned long words = (size + BITS_PER_LONG - 1) / BITS_PER_LONG;
    for (; word_idx < words; word_idx++) {
        if (addr[word_idx]) {
            unsigned long pos = word_idx * BITS_PER_LONG + __ffs(addr[word_idx]);
            return pos < size ? pos : size;
        }
    }
    return size;
}

/* Iterate over set bits: for_each_set_bit(bit, addr, size) */
#define for_each_set_bit(bit, addr, size) \
    for ((bit) = find_first_bit((addr), (size)); \
         (bit) < (size); \
         (bit) = find_next_bit((addr), (size), (bit) + 1))

/* ── HWEIGHT (popcount) ──────────────────────────────────────────────────── */
static __attribute__((always_inline)) inline unsigned int hweight32(u32 w)
{
    return (unsigned int)__builtin_popcount(w);
}

static __attribute__((always_inline)) inline unsigned int hweight64(u64 w)
{
    return (unsigned int)__builtin_popcountll(w);
}

static __attribute__((always_inline)) inline unsigned int hweight_long(unsigned long w)
{
    return (unsigned int)__builtin_popcountl(w);
}

#endif /* ASCENT_LINUX_BITOPS_H */
