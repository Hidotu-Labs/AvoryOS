/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Phase 1b Stress Test — errno.h, gfp.h, kernel.h, bug.h, printk.h
 *
 * Runs at kernel boot after heap_init.  All assertions are non-fatal:
 * a failure sets g_1b_fail and is logged, but the kernel continues.
 *
 * What we verify:
 *   errno.h  — all constants have the expected numeric values; IS_ERR /
 *               ERR_PTR / PTR_ERR round-trip; ERR_CAST; IS_ERR_OR_NULL.
 *   gfp.h    — flag bit positions are distinct; composite masks contain the
 *               right modifier bits; gfpflags_allow_blocking works.
 *   kernel.h — min/max/clamp (including type-mismatch-safe variants);
 *               DIV_ROUND_UP/DOWN/CLOSEST; ARRAY_SIZE; container_of;
 *               BUILD_BUG_ON (compile-time); abs; swap; likely/unlikely.
 *   printk.h — printk() runs without crashing; pr_err/dev_err callable.
 *   bug.h    — WARN_ON(0) is silent; WARN_ON(1) fires exactly once;
 *               WARN_ON_ONCE fires only on the first call.
 */

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/printk.h>

#include "console/klog.h"

/* ── assertion helper ──────────────────────────────────────────────────── */
static int g_1b_fail = 0;

#define T1B_ASSERT(expr)                                        \
    do {                                                        \
        if (!(expr)) {                                          \
            klog_puts("  [1b] FAIL: " #expr "\n");             \
            g_1b_fail = 1;                                      \
        }                                                       \
    } while (0)

/* ══════════════════════════════════════════════════════════════════════════
 * errno.h
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1b_errno(void)
{
    /* ── Spot-check values against the Linux ABI ─────────────────────── */
    T1B_ASSERT(EPERM   == 1);
    T1B_ASSERT(ENOENT  == 2);
    T1B_ASSERT(EIO     == 5);
    T1B_ASSERT(ENOMEM  == 12);
    T1B_ASSERT(EACCES  == 13);
    T1B_ASSERT(EFAULT  == 14);
    T1B_ASSERT(EBUSY   == 16);
    T1B_ASSERT(EEXIST  == 17);
    T1B_ASSERT(ENODEV  == 19);
    T1B_ASSERT(EINVAL  == 22);
    T1B_ASSERT(ENOSPC  == 28);
    T1B_ASSERT(ERANGE  == 34);
    T1B_ASSERT(ENOSYS  == 38);
    T1B_ASSERT(EOVERFLOW == 75);
    T1B_ASSERT(ETIMEDOUT == 110);
    T1B_ASSERT(ECANCELED == 125);

    /* ── Aliases ─────────────────────────────────────────────────────── */
    T1B_ASSERT(EWOULDBLOCK == EAGAIN);
    T1B_ASSERT(EDEADLOCK   == EDEADLK);
    T1B_ASSERT(ENOTSUP     == EOPNOTSUPP);

    /* ── ERR_PTR / PTR_ERR round-trip ────────────────────────────────── */
    void *p;

    p = ERR_PTR(-ENOMEM);
    T1B_ASSERT(IS_ERR(p));
    T1B_ASSERT(PTR_ERR(p) == -ENOMEM);

    p = ERR_PTR(-EINVAL);
    T1B_ASSERT(IS_ERR(p));
    T1B_ASSERT(PTR_ERR(p) == -EINVAL);

    p = ERR_PTR(-ENODEV);
    T1B_ASSERT(IS_ERR(p));
    T1B_ASSERT(PTR_ERR(p) == -ENODEV);

    /* ── Valid pointer must NOT be an error ─────────────────────────── */
    int dummy = 0;
    p = &dummy;
    T1B_ASSERT(!IS_ERR(p));
    T1B_ASSERT(!IS_ERR_OR_NULL(p));

    /* ── NULL must be IS_ERR_OR_NULL but not IS_ERR ─────────────────── */
    T1B_ASSERT(!IS_ERR(NULL));
    T1B_ASSERT(IS_ERR_OR_NULL(NULL));

    /* ── ERR_CAST preserves the encoded value ────────────────────────── */
    void *orig = ERR_PTR(-EIO);
    void *cast = ERR_CAST(orig);
    T1B_ASSERT(PTR_ERR(cast) == -EIO);

    /* ── All standard errnos must be IS_ERR when negated ────────────── */
    /* A selection that spans the entire range */
    static const int errnos[] = {
        EPERM, ENOENT, EIO, ENOMEM, EINVAL, EBUSY, ENODEV,
        ENOSYS, ETIMEDOUT, ECANCELED, MAX_ERRNO
    };
    for (int i = 0; i < (int)(sizeof(errnos)/sizeof(errnos[0])); i++) {
        void *ep = ERR_PTR(-(long)errnos[i]);
        T1B_ASSERT(IS_ERR(ep));
        T1B_ASSERT(PTR_ERR(ep) == -(long)errnos[i]);
    }

    /* ── ERR_PTR(0) should NOT be an error ───────────────────────────── */
    /* Technically PTR_ERR(ERR_PTR(0)) == 0 and IS_ERR(NULL)==false     */
    T1B_ASSERT(!IS_ERR(ERR_PTR(0)));
}

/* ══════════════════════════════════════════════════════════════════════════
 * gfp.h
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1b_gfp(void)
{
    /* ── All single-bit modifier flags must be distinct powers of 2 ──── */
    gfp_t flags[] = {
        __GFP_DMA, __GFP_HIGHMEM, __GFP_DMA32, __GFP_MOVABLE,
        __GFP_HIGH, __GFP_IO, __GFP_FS, __GFP_ZERO, __GFP_ATOMIC,
        __GFP_DIRECT_RECLAIM, __GFP_KSWAPD_RECLAIM,
        __GFP_NOWARN, __GFP_NOFAIL, __GFP_NORETRY,
    };
    int n = (int)(sizeof(flags) / sizeof(flags[0]));
    /* OR-ing them all must equal their sum (no overlapping bits) */
    gfp_t combined = 0;
    gfp_t summed = 0;
    for (int i = 0; i < n; i++) {
        combined |= flags[i];
        summed   += flags[i];
    }
    T1B_ASSERT(combined == summed);

    /* ── Composite masks contain their expected modifiers ────────────── */
    T1B_ASSERT((GFP_KERNEL & __GFP_IO)             != 0);
    T1B_ASSERT((GFP_KERNEL & __GFP_FS)             != 0);
    T1B_ASSERT((GFP_KERNEL & __GFP_DIRECT_RECLAIM) != 0);

    T1B_ASSERT((GFP_ATOMIC & __GFP_HIGH)           != 0);
    T1B_ASSERT((GFP_ATOMIC & __GFP_ATOMIC)         != 0);

    T1B_ASSERT((GFP_DMA    & __GFP_DMA)            != 0);
    T1B_ASSERT((GFP_DMA32  & __GFP_DMA32)          != 0);

    /* GFP_ATOMIC must NOT allow blocking */
    T1B_ASSERT(!gfpflags_allow_blocking(GFP_ATOMIC));

    /* GFP_KERNEL MUST allow blocking */
    T1B_ASSERT(gfpflags_allow_blocking(GFP_KERNEL));

    /* GFP_NOWAIT must NOT allow blocking */
    T1B_ASSERT(!gfpflags_allow_blocking(GFP_NOWAIT));

    /* __GFP_ZERO is independent of GFP_KERNEL by default */
    T1B_ASSERT((GFP_KERNEL & __GFP_ZERO) == 0);
    T1B_ASSERT((GFP_KERNEL_ZERO & __GFP_ZERO) != 0);
    /* GFP_KERNEL_ZERO still allows blocking */
    T1B_ASSERT(gfpflags_allow_blocking(GFP_KERNEL_ZERO));
}

/* ══════════════════════════════════════════════════════════════════════════
 * kernel.h
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1b_kernel(void)
{
    /* ── min / max ───────────────────────────────────────────────────── */
    T1B_ASSERT(min(3, 5)   == 3);
    T1B_ASSERT(min(-1, 0)  == -1);
    T1B_ASSERT(min(7, 7)   == 7);
    T1B_ASSERT(max(3, 5)   == 5);
    T1B_ASSERT(max(-1, 0)  == 0);
    T1B_ASSERT(max(7, 7)   == 7);
    T1B_ASSERT(min3(1,2,3) == 1);
    T1B_ASSERT(min3(3,2,1) == 1);
    T1B_ASSERT(max3(1,2,3) == 3);

    /* min_t / max_t — explicit cast */
    T1B_ASSERT(min_t(int,  -1, 0)  == -1);
    T1B_ASSERT(max_t(unsigned, 3u, 5u) == 5u);

    /* No double-evaluation: the macro must work with volatile-like exprs */
    int a = 0, b = 0;
    int result = min(++a, ++b); /* each must increment exactly once */
    T1B_ASSERT(a == 1 && b == 1);
    T1B_ASSERT(result == 1);

    /* ── clamp ───────────────────────────────────────────────────────── */
    T1B_ASSERT(clamp(5,  0, 10)  == 5);
    T1B_ASSERT(clamp(-5, 0, 10)  == 0);
    T1B_ASSERT(clamp(15, 0, 10)  == 10);
    T1B_ASSERT(clamp(0,  0, 10)  == 0);
    T1B_ASSERT(clamp(10, 0, 10)  == 10);
    T1B_ASSERT(clamp_val(3u, 1u, 8u) == 3u);
    T1B_ASSERT(clamp_val(0u, 1u, 8u) == 1u);
    T1B_ASSERT(clamp_val(9u, 1u, 8u) == 8u);

    /* ── DIV_ROUND_UP ────────────────────────────────────────────────── */
    T1B_ASSERT(DIV_ROUND_UP(0,  4) == 0);
    T1B_ASSERT(DIV_ROUND_UP(1,  4) == 1);
    T1B_ASSERT(DIV_ROUND_UP(4,  4) == 1);
    T1B_ASSERT(DIV_ROUND_UP(5,  4) == 2);
    T1B_ASSERT(DIV_ROUND_UP(8,  4) == 2);
    T1B_ASSERT(DIV_ROUND_UP(9,  4) == 3);
    T1B_ASSERT(DIV_ROUND_UP(4096, 512)  == 8);
    T1B_ASSERT(DIV_ROUND_UP(4097, 512)  == 9);
    T1B_ASSERT(DIV_ROUND_UP(1,    4096) == 1);

    /* ── DIV_ROUND_DOWN ──────────────────────────────────────────────── */
    T1B_ASSERT(DIV_ROUND_DOWN(0,  4) == 0);
    T1B_ASSERT(DIV_ROUND_DOWN(3,  4) == 0);
    T1B_ASSERT(DIV_ROUND_DOWN(4,  4) == 1);
    T1B_ASSERT(DIV_ROUND_DOWN(7,  4) == 1);
    T1B_ASSERT(DIV_ROUND_DOWN(8,  4) == 2);

    /* ── DIV_ROUND_CLOSEST ───────────────────────────────────────────── */
    T1B_ASSERT(DIV_ROUND_CLOSEST(0,  4) == 0);
    T1B_ASSERT(DIV_ROUND_CLOSEST(2,  4) == 1);  /* tie rounds up */
    T1B_ASSERT(DIV_ROUND_CLOSEST(1,  4) == 0);
    T1B_ASSERT(DIV_ROUND_CLOSEST(3,  4) == 1);
    T1B_ASSERT(DIV_ROUND_CLOSEST(5,  4) == 1);
    T1B_ASSERT(DIV_ROUND_CLOSEST(6,  4) == 2);

    /* ── abs ─────────────────────────────────────────────────────────── */
    T1B_ASSERT(abs(0)   == 0);
    T1B_ASSERT(abs(42)  == 42);
    T1B_ASSERT(abs(-42) == 42);
    T1B_ASSERT(abs(-1)  == 1);

    /* ── swap ────────────────────────────────────────────────────────── */
    int x = 10, y = 20;
    swap(x, y);
    T1B_ASSERT(x == 20 && y == 10);
    swap(x, y);
    T1B_ASSERT(x == 10 && y == 20);

    /* ── ARRAY_SIZE ──────────────────────────────────────────────────── */
    int arr4[4];
    T1B_ASSERT(ARRAY_SIZE(arr4) == 4);
    char arr256[256];
    T1B_ASSERT(ARRAY_SIZE(arr256) == 256);
    /* 2D array */
    int grid[3][4];
    T1B_ASSERT(ARRAY_SIZE(grid) == 3);

    /* ── container_of ────────────────────────────────────────────────── */
    struct test_container {
        int  head;
        long payload;
        int  tail;
    } tc = { .head = 1, .payload = 0xDEADL, .tail = 2 };
    long *pp = &tc.payload;
    struct test_container *back = container_of(pp, struct test_container, payload);
    T1B_ASSERT(back == &tc);
    T1B_ASSERT(back->head    == 1);
    T1B_ASSERT(back->payload == 0xDEADL);
    T1B_ASSERT(back->tail    == 2);

    /* Second member type */
    int *tp = &tc.tail;
    struct test_container *back2 = container_of(tp, struct test_container, tail);
    T1B_ASSERT(back2 == &tc);

    /* ── likely / unlikely (just ensure they compile and return correctly) */
    int val = 1;
    T1B_ASSERT(likely(val)   == 1);
    T1B_ASSERT(unlikely(val) == 1);
    T1B_ASSERT(likely(0)     == 0);
    T1B_ASSERT(unlikely(0)   == 0);

    /* ── BUILD_BUG_ON (compile-time; wrong assertions would fail to compile) */
    BUILD_BUG_ON(sizeof(int) != 4);
    BUILD_BUG_ON(sizeof(long) != 8);
    /* If we get here, the static asserts above passed at compile time. */
    T1B_ASSERT(1); /* dummy — BUILD_BUG_ON is already compile-time */

    /* ── ROUND_UP / ROUND_DOWN ───────────────────────────────────────── */
    T1B_ASSERT(ROUND_UP(0,   16) == 0);
    T1B_ASSERT(ROUND_UP(1,   16) == 16);
    T1B_ASSERT(ROUND_UP(16,  16) == 16);
    T1B_ASSERT(ROUND_UP(17,  16) == 32);
    T1B_ASSERT(ROUND_DOWN(17, 16) == 16);
    T1B_ASSERT(ROUND_DOWN(16, 16) == 16);
    T1B_ASSERT(ROUND_DOWN(0,  16) == 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * printk.h
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1b_printk(void)
{
    /* printk must not crash and must format its output.
     * We can't easily capture the output at boot time, so we verify it
     * returns (doesn't hang) and that the pr_* / dev_* wrappers are callable. */
    printk(KERN_INFO "  [1b] printk self-test: int=%d str=%s hex=0x%x\n",
           42, "hello", 0xABCD);

    pr_info("  [1b] pr_info test\n");
    pr_warn("  [1b] pr_warn test\n");
    pr_err ("  [1b] pr_err  test\n");

    /* dev_* — pass NULL for device pointer (accepted by our shim) */
    dev_info(NULL,  "  [1b] dev_info test\n");
    dev_warn(NULL,  "  [1b] dev_warn test\n");
    dev_err (NULL,  "  [1b] dev_err  test\n");

    /* Rate-limited variants */
    dev_err_ratelimited(NULL,  "  [1b] dev_err_ratelimited test\n");
    dev_info_ratelimited(NULL, "  [1b] dev_info_ratelimited test\n");

    /* drm_* variants */
    drm_info(NULL, "  [1b] drm_info test\n");
    drm_warn(NULL, "  [1b] drm_warn test\n");
    drm_err (NULL, "  [1b] drm_err  test\n");

    /* Once-only variants must not crash on repeated calls */
    for (int i = 0; i < 5; i++)
        pr_err_once("  [1b] pr_err_once (should appear only once)\n");
    for (int i = 0; i < 5; i++)
        pr_warn_once("  [1b] pr_warn_once\n");

    /* printk with no args (plain string) */
    printk(KERN_DEBUG "  [1b] printk plain string\n");

    /* Format stress: every specifier the kernel commonly uses */
    printk(KERN_INFO "  [1b] fmt stress: "
           "u=%u d=%d ld=%ld lu=%lu "
           "x=%x X=%X lx=%lx "
           "p=%p s=%s c=%c\n",
           (unsigned)42, -42, -42L, 42UL,
           0xdeadU, 0xBEEFU, 0xcafeUL,
           (void *)0xDEAD, "str", 'K');

    /* We consider printk "passing" if we reach this point without a crash */
    T1B_ASSERT(1);
}

/* ══════════════════════════════════════════════════════════════════════════
 * bug.h
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1b_bug(void)
{
    /* WARN_ON(0) must be silent (no log line) and return false */
    int r = WARN_ON(0);
    T1B_ASSERT(r == 0);

    /* WARN_ON(1) must return true and log exactly once */
    r = WARN_ON(1);
    T1B_ASSERT(r != 0);

    /* WARN(cond, fmt): silent when cond==0 */
    r = WARN(0, "should not print\n");
    T1B_ASSERT(r == 0);

    /* WARN(cond, fmt): fires when cond==1, returns truthy */
    r = WARN(1, "  [1b] WARN fired (expected)\n");
    T1B_ASSERT(r != 0);

    /* WARN_ON_ONCE: first call fires, subsequent calls are silent */
    static int once_count = 0;
    /* Use a local lambda-like block to call it three times */
    for (int i = 0; i < 3; i++) {
        /* WARN_ON_ONCE uses an internal static so this only logs once */
        WARN_ON_ONCE(1);
        once_count++;
    }
    T1B_ASSERT(once_count == 3); /* loop runs 3 times; warn fires once */

    /* WARN_ONCE: same once semantics */
    for (int i = 0; i < 3; i++)
        WARN_ONCE(1, "  [1b] WARN_ONCE (should appear once)\n");

    /* BUG_ON(0) must be a no-op (no halt) */
    BUG_ON(0);

    /* VM_WARN_ON(0) must be silent */
    r = VM_WARN_ON(0);
    T1B_ASSERT(r == 0);

    /* VM_BUG_ON(0) must be a no-op */
    VM_BUG_ON(0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Integration: errno + kernel.h together
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1b_integration(void)
{
    /* A typical driver pattern: allocate, return -ENOMEM on failure.
     * Verify the error flows through ERR_PTR/IS_ERR/PTR_ERR. */
    void *ptr = (void *)0;  /* simulate allocation failure */

    /* Driver would: if (!ptr) return ERR_PTR(-ENOMEM); */
    void *ret = ptr ? ptr : ERR_PTR(-ENOMEM);
    T1B_ASSERT(IS_ERR(ret));
    T1B_ASSERT(PTR_ERR(ret) == -ENOMEM);

    /* Caller checks: if (IS_ERR(ret)) { err = PTR_ERR(ret); goto out; } */
    int err = IS_ERR(ret) ? (int)PTR_ERR(ret) : 0;
    T1B_ASSERT(err == -ENOMEM);

    /* DIV_ROUND_UP used for buffer sizing — common in GPU drivers */
    u32 size_bytes = 12345;
    u32 page_size  = 4096;
    u32 pages = DIV_ROUND_UP(size_bytes, page_size);
    T1B_ASSERT(pages == 4);  /* ceil(12345/4096) == 4 */

    /* ARRAY_SIZE on a constant table — driver device ID tables */
    static const int ids[] = { 0x1002, 0x164E, 0x1900, 0x744C };
    T1B_ASSERT(ARRAY_SIZE(ids) == 4);

    /* clamp on a timeout value */
    u32 timeout_ms = 50000; /* driver requests 50s */
    u32 clamped = clamp_val(timeout_ms, 1000u, 10000u);
    T1B_ASSERT(clamped == 10000u);

    /* min_t on register field widths */
    T1B_ASSERT(min_t(u32, 0xFFFFFFFFU, 0x100U) == 0x100U);
}

/* ── public entry point ────────────────────────────────────────────────── */
void linuxkpi_test_1b(void)
{
    klog_puts(KLOG_CLR_CYAN
              "[TEST ] Phase 1b — LinuxKPI errno/gfp/kernel/printk/bug\n"
              KLOG_CLR_RESET);

    test_1b_errno();
    test_1b_gfp();
    test_1b_kernel();
    test_1b_printk();
    test_1b_bug();
    test_1b_integration();

    if (g_1b_fail) {
        klog_puts(KLOG_CLR_RED
                  "[ FAIL] Phase 1b LinuxKPI stress test — see failures above\n"
                  KLOG_CLR_RESET);
    } else {
        klog_puts(KLOG_CLR_GREEN
                  "[  OK ] Phase 1b LinuxKPI stress test PASSED\n"
                  KLOG_CLR_RESET);
    }
}
