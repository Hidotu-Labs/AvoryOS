/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Phase 1a Stress Test — Types, Limits, Align, Cache, Log2, Bitops, Bitmap
 *
 * Runs at kernel boot (after heap_init). Reports PASS/FAIL per section via
 * klog. A single failure sets the global result to FAIL; the kernel continues
 * regardless so a soft failure doesn't stop the system.
 */

#include <linux/align.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/limits.h>
#include <linux/log2.h>
#include <linux/types.h>

#include "console/klog.h"

/* ── tiny assertion helper ─────────────────────────────────────────────── */
static int g_1a_fail = 0;

#define T1A_ASSERT(expr)                                            \
    do {                                                            \
        if (!(expr)) {                                              \
            klog_puts("  [1a] FAIL: " #expr "\n");                 \
            g_1a_fail = 1;                                          \
        }                                                           \
    } while (0)

/* ── section runners ───────────────────────────────────────────────────── */

static void test_1a_types(void)
{
    T1A_ASSERT(sizeof(u8)  == 1);
    T1A_ASSERT(sizeof(u16) == 2);
    T1A_ASSERT(sizeof(u32) == 4);
    T1A_ASSERT(sizeof(u64) == 8);
    T1A_ASSERT(sizeof(s8)  == 1);
    T1A_ASSERT(sizeof(s16) == 2);
    T1A_ASSERT(sizeof(s32) == 4);
    T1A_ASSERT(sizeof(s64) == 8);
    T1A_ASSERT(sizeof(__le16) == 2);
    T1A_ASSERT(sizeof(__le32) == 4);
    T1A_ASSERT(sizeof(__le64) == 8);
    T1A_ASSERT(sizeof(__be16) == 2);
    T1A_ASSERT(sizeof(__be32) == 4);
    T1A_ASSERT(sizeof(__be64) == 8);
    T1A_ASSERT(sizeof(__u8)  == 1);
    T1A_ASSERT(sizeof(__u64) == 8);
    T1A_ASSERT(sizeof(__s32) == 4);

    /* Sign-extension: s8 -1 must stay negative after integer promotion */
    s8 neg8 = (s8)-1;
    T1A_ASSERT((int)neg8 == -1);
    s16 neg16 = (s16)-1;
    T1A_ASSERT((int)neg16 == -1);
    s32 neg32 = (s32)-1;
    T1A_ASSERT(neg32 == -1);
    s64 neg64 = (s64)-1LL;
    T1A_ASSERT(neg64 == -1LL);

    /* Unsigned wrap-around — must not sign-extend */
    u8 wrap8 = (u8)256;          /* wraps to 0 */
    T1A_ASSERT(wrap8 == 0);
    u8 wrap8b = (u8)255 + (u8)1; /* wrap8b is u8(0) after the add */
    T1A_ASSERT((u8)wrap8b == 0);
    u32 wrap32 = (u32)0xFFFFFFFFU + 1U;
    T1A_ASSERT(wrap32 == 0);

    /* NULL */
    void *p = NULL;
    T1A_ASSERT(p == (void *)0);
    T1A_ASSERT(!p);
}

static void test_1a_limits(void)
{
    T1A_ASSERT(U8_MAX  == 0xFFU);
    T1A_ASSERT(U16_MAX == 0xFFFFU);
    T1A_ASSERT(U32_MAX == 0xFFFFFFFFU);
    T1A_ASSERT(U64_MAX == 0xFFFFFFFFFFFFFFFFULL);

    T1A_ASSERT(S8_MAX  == 127);
    T1A_ASSERT(S8_MIN  == -128);
    T1A_ASSERT(S16_MAX == 32767);
    T1A_ASSERT(S16_MIN == -32768);
    T1A_ASSERT(S32_MAX == 0x7FFFFFFF);
    T1A_ASSERT(S32_MIN == (s32)(-0x7FFFFFFF - 1));
    T1A_ASSERT(S64_MAX == 0x7FFFFFFFFFFFFFFFLL);
    T1A_ASSERT(S64_MIN == (s64)(-0x7FFFFFFFFFFFFFFFLL - 1));

    /* Alias consistency */
    T1A_ASSERT(INT_MAX  == S32_MAX);
    T1A_ASSERT(INT_MIN  == S32_MIN);
    T1A_ASSERT(UINT_MAX == U32_MAX);
    T1A_ASSERT(LONG_MAX == S64_MAX);
    T1A_ASSERT(ULONG_MAX == U64_MAX);

    /* Boundary arithmetic: S32_MAX + 1 must wrap to S32_MIN */
    s32 overflow = S32_MAX;
    overflow++;
    T1A_ASSERT(overflow == S32_MIN);

    /* S64_MIN must be representable (two's complement).
     * Signed overflow is UB in C, so cast to u64 before subtracting. */
    s64 min64 = S64_MIN;
    T1A_ASSERT(min64 < 0);
    T1A_ASSERT((u64)min64 - 1ULL == (u64)S64_MAX);  /* wraps in unsigned domain */
}

static void test_1a_align(void)
{
    /* ALIGN basic cases */
    T1A_ASSERT(ALIGN(0,    16) == 0);
    T1A_ASSERT(ALIGN(1,    16) == 16);
    T1A_ASSERT(ALIGN(16,   16) == 16);
    T1A_ASSERT(ALIGN(17,   16) == 32);
    T1A_ASSERT(ALIGN(4095, 4096) == 4096);
    T1A_ASSERT(ALIGN(4096, 4096) == 4096);
    T1A_ASSERT(ALIGN(4097, 4096) == 8192);

    /* ALIGN with u64 — catch 32-bit truncation bugs */
    u64 big = 0x100000001ULL;   /* 4GB + 1 */
    T1A_ASSERT(ALIGN(big, 4096ULL) == 0x100001000ULL);

    /* ALIGN_DOWN */
    T1A_ASSERT(ALIGN_DOWN(0,  16) == 0);
    T1A_ASSERT(ALIGN_DOWN(16, 16) == 16);
    T1A_ASSERT(ALIGN_DOWN(17, 16) == 16);
    T1A_ASSERT(ALIGN_DOWN(31, 16) == 16);
    T1A_ASSERT(ALIGN_DOWN(32, 16) == 32);

    /* IS_ALIGNED */
    T1A_ASSERT(IS_ALIGNED(0,   64));
    T1A_ASSERT(IS_ALIGNED(64,  64));
    T1A_ASSERT(!IS_ALIGNED(65, 64));
    T1A_ASSERT(!IS_ALIGNED(63, 64));

    /* PAGE_ALIGN */
    T1A_ASSERT(PAGE_ALIGN(0)    == 0);
    T1A_ASSERT(PAGE_ALIGN(1)    == 4096);
    T1A_ASSERT(PAGE_ALIGN(4096) == 4096);
    T1A_ASSERT(PAGE_ALIGN(4097) == 8192);

    /* PTR_ALIGN: pointer value must be >= original and be aligned */
    uintptr_t raw = 0x1001;
    char *aligned = PTR_ALIGN((char *)raw, 64);
    T1A_ASSERT(((uintptr_t)aligned % 64) == 0);
    T1A_ASSERT((uintptr_t)aligned >= raw);
    T1A_ASSERT((uintptr_t)aligned - raw < 64);  /* can't overshoot by a full unit */

    /* PTR_ALIGN on already-aligned pointer must be identity */
    char *already = (char *)0x2000;
    T1A_ASSERT(PTR_ALIGN(already, 4096) == already);

    /* PTR_ALIGN_DOWN */
    char *down = PTR_ALIGN_DOWN((char *)0x1FFF, 4096);
    T1A_ASSERT((uintptr_t)down == 0x1000);
}

/* File-scope variable to verify ____cacheline_aligned compiles and aligns */
static int _t1a_cache_var ____cacheline_aligned = 0;

static void test_1a_cache(void)
{
    T1A_ASSERT(SMP_CACHE_BYTES == 64);
    T1A_ASSERT(L1_CACHE_BYTES  == 64);
    T1A_ASSERT(L1_CACHE_SHIFT  == 6);
    /* Verify the attribute actually produces 64-byte alignment */
    T1A_ASSERT(((uintptr_t)&_t1a_cache_var % 64) == 0);
    (void)_t1a_cache_var;
}

static void test_1a_log2(void)
{
    /* ilog2 — 32-bit range */
    T1A_ASSERT(ilog2(1)    == 0);
    T1A_ASSERT(ilog2(2)    == 1);
    T1A_ASSERT(ilog2(3)    == 1);
    T1A_ASSERT(ilog2(4)    == 2);
    T1A_ASSERT(ilog2(7)    == 2);
    T1A_ASSERT(ilog2(8)    == 3);
    T1A_ASSERT(ilog2(1024) == 10);
    T1A_ASSERT(ilog2(1025) == 10);
    T1A_ASSERT(ilog2(0x80000000UL) == 31);

    /* ilog2_u64 — 64-bit range */
    T1A_ASSERT(ilog2_u64(1ULL)                    == 0);
    T1A_ASSERT(ilog2_u64(0x100000000ULL)          == 32);  /* 4 GB */
    T1A_ASSERT(ilog2_u64(0x8000000000000000ULL)   == 63);

    /* roundup_pow_of_two */
    T1A_ASSERT(roundup_pow_of_two(1)    == 1);
    T1A_ASSERT(roundup_pow_of_two(2)    == 2);
    T1A_ASSERT(roundup_pow_of_two(3)    == 4);
    T1A_ASSERT(roundup_pow_of_two(5)    == 8);
    T1A_ASSERT(roundup_pow_of_two(1024) == 1024);
    T1A_ASSERT(roundup_pow_of_two(1025) == 2048);
    T1A_ASSERT(roundup_pow_of_two(0x7FFFFFFFUL) == 0x80000000UL);

    /* idempotency: already power-of-2 must not change */
    for (int e = 0; e <= 30; e++) {
        unsigned long p = 1UL << e;
        T1A_ASSERT(roundup_pow_of_two(p) == p);
    }

    /* rounddown_pow_of_two */
    T1A_ASSERT(rounddown_pow_of_two(1)  == 1);
    T1A_ASSERT(rounddown_pow_of_two(7)  == 4);
    T1A_ASSERT(rounddown_pow_of_two(8)  == 8);
    T1A_ASSERT(rounddown_pow_of_two(9)  == 8);
    T1A_ASSERT(rounddown_pow_of_two(0x80000001UL) == 0x80000000UL);

    /* is_power_of_2 */
    T1A_ASSERT(is_power_of_2(1));
    T1A_ASSERT(is_power_of_2(2));
    T1A_ASSERT(is_power_of_2(1024));
    T1A_ASSERT(!is_power_of_2(0));
    T1A_ASSERT(!is_power_of_2(1023));
    T1A_ASSERT(!is_power_of_2(3));
    /* Every exact power of 2 from 1..2^30 must pass */
    for (int e = 0; e <= 30; e++)
        T1A_ASSERT(is_power_of_2(1UL << e));
    /* Adjacent non-powers must fail.
     * Start at e=2: (1<<1)-1 == 1 which IS a power of 2, so skip e=1. */
    for (int e = 2; e <= 30; e++) {
        T1A_ASSERT(!is_power_of_2((1UL << e) - 1));
        T1A_ASSERT(!is_power_of_2((1UL << e) + 1));
    }
}

static void test_1a_bitops(void)
{
    /* BIT macro */
    T1A_ASSERT(BIT(0)  == 1UL);
    T1A_ASSERT(BIT(7)  == 128UL);
    T1A_ASSERT(BIT(63) == (1UL << 63));
    T1A_ASSERT(BIT_ULL(63) == (1ULL << 63));

    /* fls */
    T1A_ASSERT(fls(0)          == 0);
    T1A_ASSERT(fls(1)          == 1);
    T1A_ASSERT(fls(4)          == 3);
    T1A_ASSERT(fls(0xFF)       == 8);
    T1A_ASSERT(fls(0xFFFFFFFF) == 32);
    T1A_ASSERT(fls(0x80000000) == 32);

    /* fls64 */
    T1A_ASSERT(fls64(0ULL)                    == 0);
    T1A_ASSERT(fls64(1ULL)                    == 1);
    T1A_ASSERT(fls64(0x100000000ULL)          == 33);
    T1A_ASSERT(fls64(0x8000000000000000ULL)   == 64);
    T1A_ASSERT(fls64(0xFFFFFFFFFFFFFFFFULL)   == 64);

    /* __fls */
    T1A_ASSERT(__fls(1UL)  == 0);
    T1A_ASSERT(__fls(2UL)  == 1);
    T1A_ASSERT(__fls(8UL)  == 3);
    T1A_ASSERT(__fls(BIT(63)) == 63);

    /* __ffs */
    T1A_ASSERT(__ffs(1UL)    == 0);
    T1A_ASSERT(__ffs(8UL)    == 3);
    T1A_ASSERT(__ffs(0x10UL) == 4);
    T1A_ASSERT(__ffs(BIT(63)) == 63);

    /* hweight */
    T1A_ASSERT(hweight32(0)                   == 0);
    T1A_ASSERT(hweight32(0xFF)                == 8);
    T1A_ASSERT(hweight32(0xFFFFFFFF)          == 32);
    T1A_ASSERT(hweight32(0xAAAAAAAA)          == 16);
    T1A_ASSERT(hweight64(0)                   == 0);
    T1A_ASSERT(hweight64(0xFFFFFFFFFFFFFFFFULL) == 64);
    T1A_ASSERT(hweight64(0xAAAAAAAAAAAAAAAAULL) == 32);
    T1A_ASSERT(hweight_long(~0UL)             == 64);

    /* set_bit / clear_bit / test_bit */
    unsigned long bm[2] = {0, 0};
    set_bit(0,  bm);  T1A_ASSERT(test_bit(0, bm)  == 1);
    set_bit(63, bm);  T1A_ASSERT(test_bit(63, bm) == 1);
    set_bit(64, bm);  T1A_ASSERT(test_bit(64, bm) == 1);  /* second word */
    clear_bit(63, bm); T1A_ASSERT(test_bit(63, bm) == 0);
    T1A_ASSERT(test_bit(0,  bm) == 1);  /* neighbouring bit must be untouched */
    T1A_ASSERT(test_bit(64, bm) == 1);

    /* test_and_set / test_and_clear */
    T1A_ASSERT(test_and_set_bit(63, bm)   == 0); /* was 0, now 1 */
    T1A_ASSERT(test_and_set_bit(63, bm)   == 1); /* already 1 */
    T1A_ASSERT(test_and_clear_bit(63, bm) == 1); /* was 1, now 0 */
    T1A_ASSERT(test_bit(63, bm) == 0);

    /* find_first_bit */
    unsigned long fbm[2] = {0b10100UL, 0UL}; /* bits 2 and 4 set */
    T1A_ASSERT(find_first_bit(fbm, 128) == 2);

    /* find_first_zero_bit */
    unsigned long full[2] = {~0UL, ~0UL};
    T1A_ASSERT(find_first_zero_bit(full, 128) == 128); /* all set */
    unsigned long notfull[2] = {~0UL, ~1UL};           /* bit 64 clear */
    T1A_ASSERT(find_first_zero_bit(notfull, 128) == 64);

    /* find_next_bit — intra-word */
    T1A_ASSERT(find_next_bit(fbm, 128, 3) == 4);
    T1A_ASSERT(find_next_bit(fbm, 128, 5) == 128); /* none remaining */

    /* find_next_bit — cross-word boundary (bit 63 → bit 64) */
    unsigned long cross[2] = {BIT(63), BIT(0)};  /* bit 63 and bit 64 */
    T1A_ASSERT(find_next_bit(cross, 128, 0)  == 63);
    T1A_ASSERT(find_next_bit(cross, 128, 64) == 64);
    T1A_ASSERT(find_next_bit(cross, 128, 65) == 128);

    /* for_each_set_bit: count 2 bits */
    unsigned int count = 0;
    unsigned long iter_bm[1] = {0b10100UL};
    unsigned long bit;
    for_each_set_bit(bit, iter_bm, 8) { count++; }
    T1A_ASSERT(count == 2);

    /* for_each_set_bit: 64 consecutive bits spanning both words */
    unsigned long full64[2] = {~0UL, ~0UL};
    count = 0;
    for_each_set_bit(bit, full64, 128) { count++; }
    T1A_ASSERT(count == 128);

    /* STRESS: set every bit 0-255 via set_bit, verify popcount */
    unsigned long stress[4] = {0};
    for (int i = 0; i < 256; i++)
        set_bit((unsigned)i, stress);
    unsigned int total = 0;
    for (int w = 0; w < 4; w++)
        total += hweight_long(stress[w]);
    T1A_ASSERT(total == 256);

    /* STRESS: alternating set/clear — even bits only */
    unsigned long alt[4] = {0};
    for (int i = 0; i < 256; i += 2)
        set_bit((unsigned)i, alt);
    total = 0;
    for (int w = 0; w < 4; w++)
        total += hweight_long(alt[w]);
    T1A_ASSERT(total == 128);
    /* verify odd bits are still clear */
    for (int i = 1; i < 256; i += 2)
        T1A_ASSERT(test_bit((unsigned)i, alt) == 0);
}

static void test_1a_bitmap(void)
{
    /* ── basic alloc/free ─────────────────────────────────────────────── */
    unsigned long *bm = bitmap_zalloc(128);
    T1A_ASSERT(bm != (void *)0);
    T1A_ASSERT(bitmap_empty(bm, 128));
    T1A_ASSERT(bitmap_weight(bm, 128) == 0);

    /* fill / weight / full */
    bitmap_fill(bm, 128);
    T1A_ASSERT(bitmap_full(bm, 128));
    T1A_ASSERT(bitmap_weight(bm, 128) == 128);
    T1A_ASSERT(!bitmap_empty(bm, 128));

    /* zero */
    bitmap_zero(bm, 128);
    T1A_ASSERT(bitmap_empty(bm, 128));
    T1A_ASSERT(bitmap_weight(bm, 128) == 0);

    /* single-bit ops */
    bitmap_set_bit(bm, 0);
    bitmap_set_bit(bm, 63);
    bitmap_set_bit(bm, 64);   /* first bit of second word */
    bitmap_set_bit(bm, 127);
    T1A_ASSERT(bitmap_test_bit(bm, 0));
    T1A_ASSERT(bitmap_test_bit(bm, 63));
    T1A_ASSERT(bitmap_test_bit(bm, 64));
    T1A_ASSERT(bitmap_test_bit(bm, 127));
    T1A_ASSERT(!bitmap_test_bit(bm, 1));
    T1A_ASSERT(!bitmap_test_bit(bm, 65));
    T1A_ASSERT(bitmap_weight(bm, 128) == 4);
    bitmap_clear_bit(bm, 63);
    T1A_ASSERT(!bitmap_test_bit(bm, 63));
    T1A_ASSERT(bitmap_test_bit(bm, 64)); /* neighbour untouched */
    T1A_ASSERT(bitmap_weight(bm, 128) == 3);

    /* ── logical ops ─────────────────────────────────────────────────── */
    unsigned long *bm2 = bitmap_zalloc(128);
    unsigned long *bm3 = bitmap_zalloc(128);
    T1A_ASSERT(bm2 && bm3);

    /* OR: bm={0,64,127}, bm2={64} → bm3={0,64,127} */
    bitmap_set_bit(bm2, 64);
    bitmap_or(bm3, bm, bm2, 128);
    T1A_ASSERT(bitmap_test_bit(bm3, 0));
    T1A_ASSERT(bitmap_test_bit(bm3, 64));
    T1A_ASSERT(bitmap_test_bit(bm3, 127));
    T1A_ASSERT(!bitmap_test_bit(bm3, 1));
    T1A_ASSERT(bitmap_weight(bm3, 128) == 3);

    /* AND: bm={0,64,127}, bm2={64} → bm3={64} */
    bitmap_and(bm3, bm, bm2, 128);
    T1A_ASSERT(!bitmap_test_bit(bm3, 0));
    T1A_ASSERT(bitmap_test_bit(bm3, 64));
    T1A_ASSERT(!bitmap_test_bit(bm3, 127));
    T1A_ASSERT(bitmap_weight(bm3, 128) == 1);

    /* ANDNOT: bm={0,64,127}, bm2={64} → bm3={0,127} */
    bitmap_andnot(bm3, bm, bm2, 128);
    T1A_ASSERT(bitmap_test_bit(bm3, 0));
    T1A_ASSERT(!bitmap_test_bit(bm3, 64));
    T1A_ASSERT(bitmap_test_bit(bm3, 127));
    T1A_ASSERT(bitmap_weight(bm3, 128) == 2);

    /* ── padding-bit correctness (non-multiple-of-64) ────────────────── */
    unsigned long *pad = bitmap_zalloc(65);
    T1A_ASSERT(pad != (void *)0);
    bitmap_fill(pad, 65);
    /* must count exactly 65, not 128 */
    T1A_ASSERT(bitmap_weight(pad, 65) == 65);
    T1A_ASSERT(bitmap_full(pad, 65));
    T1A_ASSERT(!bitmap_test_bit(pad, 65)); /* bit 65 must NOT be set */
    bitmap_free(pad);

    /* non-power-of-2 size: 100 bits */
    unsigned long *p100 = bitmap_zalloc(100);
    T1A_ASSERT(p100 != (void *)0);
    bitmap_fill(p100, 100);
    T1A_ASSERT(bitmap_weight(p100, 100) == 100);
    T1A_ASSERT(bitmap_full(p100, 100));
    bitmap_zero(p100, 100);
    T1A_ASSERT(bitmap_empty(p100, 100));
    bitmap_free(p100);

    /* ── large bitmap stress ─────────────────────────────────────────── */
    unsigned long *big = bitmap_zalloc(4096); /* 64 words, 4096 bits */
    T1A_ASSERT(big != (void *)0);

    /* set every bit */
    for (int i = 0; i < 4096; i++)
        bitmap_set_bit(big, (unsigned)i);
    T1A_ASSERT(bitmap_full(big, 4096));
    T1A_ASSERT(bitmap_weight(big, 4096) == 4096);

    /* clear even bits */
    for (int i = 0; i < 4096; i += 2)
        bitmap_clear_bit(big, (unsigned)i);
    T1A_ASSERT(bitmap_weight(big, 4096) == 2048);
    /* verify all odd bits are set, even bits clear */
    for (int i = 0; i < 4096; i++) {
        if (i & 1)
            T1A_ASSERT(bitmap_test_bit(big, (unsigned)i));
        else
            T1A_ASSERT(!bitmap_test_bit(big, (unsigned)i));
    }

    /* for_each_set_bit_in_bitmap should visit exactly 2048 bits */
    unsigned long count = 0;
    unsigned long b;
    for_each_set_bit_in_bitmap(b, big, 4096) { count++; }
    T1A_ASSERT(count == 2048);

    bitmap_free(big);

    /* ── simulate a 256-CPU affinity mask ────────────────────────────── */
    unsigned long *cpumask = bitmap_zalloc(256);
    T1A_ASSERT(cpumask != (void *)0);
    /* "online" CPUs 0-127 */
    for (int i = 0; i < 128; i++)
        bitmap_set_bit(cpumask, (unsigned)i);
    T1A_ASSERT(bitmap_weight(cpumask, 256) == 128);
    /* first CPU: bit 0 */
    T1A_ASSERT(find_first_bit(cpumask, 256) == 0);
    /* cross-word first-set after bit 63: should be 64 */
    T1A_ASSERT(find_next_bit(cpumask, 256, 64) == 64);
    /* first zero after the online CPUs */
    T1A_ASSERT(find_first_zero_bit(cpumask, 256) == 128);
    bitmap_free(cpumask);

    bitmap_free(bm3);
    bitmap_free(bm2);
    bitmap_free(bm);
}

/* ── public entry point ────────────────────────────────────────────────── */
void linuxkpi_test_1a(void)
{
    klog_puts(KLOG_CLR_CYAN
              "[TEST ] Phase 1a — LinuxKPI Types/Limits/Align/Cache/Log2/Bitops/Bitmap\n"
              KLOG_CLR_RESET);

    test_1a_types();
    test_1a_limits();
    test_1a_align();
    test_1a_cache();
    test_1a_log2();
    test_1a_bitops();
    test_1a_bitmap();

    if (g_1a_fail) {
        klog_puts(KLOG_CLR_RED
                  "[ FAIL] Phase 1a LinuxKPI stress test — see failures above\n"
                  KLOG_CLR_RESET);
    } else {
        klog_puts(KLOG_CLR_GREEN
                  "[  OK ] Phase 1a LinuxKPI stress test PASSED\n"
                  KLOG_CLR_RESET);
    }
}
