/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Phase 1c Stress Test — slab.h, string.h, vmalloc.h, page.h, mm.h
 *
 * Runs at kernel boot after heap_init and vmm_init (both needed before
 * this test).  vmalloc tests are deferred here because vmalloc.c is
 * Phase 2 — we only test the declarations compile and that NULL is
 * returned gracefully if the implementation isn't wired yet.
 */

#include <linux/mm.h>
#include <linux/page.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "console/klog.h"

/* ── assertion helper ──────────────────────────────────────────────────── */
static int g_1c_fail = 0;

#define T1C_ASSERT(expr)                                        \
    do {                                                        \
        if (!(expr)) {                                          \
            klog_puts("  [1c] FAIL: " #expr "\n");             \
            g_1c_fail = 1;                                      \
        }                                                       \
    } while (0)

/* ══════════════════════════════════════════════════════════════════════════
 * slab.h — kmalloc / kzalloc / kcalloc / kmemdup / kfree
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1c_slab(void)
{
    /* ── kmalloc basic ───────────────────────────────────────────────── */
    void *p = kmalloc(64, GFP_KERNEL);
    T1C_ASSERT(p != NULL);
    kfree(p);

    /* ── kzalloc: allocation is zero-initialised ─────────────────────── */
    u8 *z = (u8 *)kzalloc(128, GFP_KERNEL);
    T1C_ASSERT(z != NULL);
    for (int i = 0; i < 128; i++)
        T1C_ASSERT(z[i] == 0);
    kfree(z);

    /* ── kcalloc: n*size, zeroed ─────────────────────────────────────── */
    u32 *arr = (u32 *)kcalloc(16, sizeof(u32), GFP_KERNEL);
    T1C_ASSERT(arr != NULL);
    for (int i = 0; i < 16; i++)
        T1C_ASSERT(arr[i] == 0);
    arr[0]  = 0xDEAD;
    arr[15] = 0xBEEF;
    T1C_ASSERT(arr[0]  == 0xDEADU);
    T1C_ASSERT(arr[15] == 0xBEEFU);
    kfree(arr);

    /* ── kmemdup: copy of a region ───────────────────────────────────── */
    const char src[] = "LinuxKPI kmemdup test";
    char *dup = (char *)kmemdup(src, sizeof(src), GFP_KERNEL);
    T1C_ASSERT(dup != NULL);
    T1C_ASSERT(dup != (char *)src);   /* must be a fresh allocation */
    T1C_ASSERT(memcmp(dup, src, sizeof(src)) == 0);
    kfree(dup);

    /* ── kstrdup / kstrndup ──────────────────────────────────────────── */
    char *s = kstrdup("hello LinuxKPI", GFP_KERNEL);
    T1C_ASSERT(s != NULL);
    T1C_ASSERT(strcmp(s, "hello LinuxKPI") == 0);
    kfree(s);

    char *sn = kstrndup("hello world", 5, GFP_KERNEL);
    T1C_ASSERT(sn != NULL);
    T1C_ASSERT(strcmp(sn, "hello") == 0);  /* truncated at 5 + NUL */
    kfree(sn);

    /* ── GFP_KERNEL | __GFP_ZERO — explicit zero flag ───────────────── */
    u8 *gz = (u8 *)kmalloc(64, GFP_KERNEL | __GFP_ZERO);
    T1C_ASSERT(gz != NULL);
    for (int i = 0; i < 64; i++)
        T1C_ASSERT(gz[i] == 0);
    kfree(gz);

    /* ── GFP_ATOMIC should also work (same heap) ─────────────────────── */
    void *pa = kmalloc(32, GFP_ATOMIC);
    T1C_ASSERT(pa != NULL);
    kfree(pa);

    /* ── kvmalloc / kvfree (alias to kmalloc here) ───────────────────── */
    void *kv = kvmalloc(256, GFP_KERNEL);
    T1C_ASSERT(kv != NULL);
    kvfree(kv);

    /* ── devm_kzalloc (ignores device pointer) ───────────────────────── */
    void *dv = devm_kzalloc(NULL, 64, GFP_KERNEL);
    T1C_ASSERT(dv != NULL);
    devm_kfree(NULL, dv);

    /* ── STRESS: 512 alloc/free cycles, sizes 16..4096 ──────────────── */
    for (int i = 1; i <= 512; i++) {
        size_t sz = (size_t)(i * 8);
        void *mp = kmalloc(sz, GFP_KERNEL);
        T1C_ASSERT(mp != NULL);
        /* write a pattern and verify it */
        memset(mp, (int)(i & 0xFF), sz);
        u8 *bp = (u8 *)mp;
        T1C_ASSERT(bp[0]      == (u8)(i & 0xFF));
        T1C_ASSERT(bp[sz - 1] == (u8)(i & 0xFF));
        kfree(mp);
    }

    /* ── kfree(NULL) must be safe ────────────────────────────────────── */
    kfree(NULL);
    kfree_const(NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * string.h — strlcpy, strnchr, strnlen, strchr, memchr, memmove
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1c_string(void)
{
    /* ── strlcpy ─────────────────────────────────────────────────────── */
    char dst[16];
    size_t r;

    r = strlcpy(dst, "hello", sizeof(dst));
    T1C_ASSERT(r == 5);
    T1C_ASSERT(strcmp(dst, "hello") == 0);

    /* truncation: src longer than dst */
    r = strlcpy(dst, "this is too long for the buffer", sizeof(dst));
    T1C_ASSERT(r == 31);                   /* returns src length */
    T1C_ASSERT(dst[15] == '\0');           /* NUL-terminated */
    T1C_ASSERT(strlen(dst) == 15);         /* exactly size-1 chars */

    /* size == 0: must not write anything */
    char tiny[1] = { 0x7F };
    r = strlcpy(tiny, "nope", 0);
    T1C_ASSERT(r == 4);
    T1C_ASSERT(tiny[0] == 0x7F); /* unchanged */

    /* ── strlcat ─────────────────────────────────────────────────────── */
    char cat[16];
    strlcpy(cat, "foo", sizeof(cat));
    r = strlcat(cat, "bar", sizeof(cat));
    T1C_ASSERT(r == 6);
    T1C_ASSERT(strcmp(cat, "foobar") == 0);

    /* truncation */
    char cat2[8];
    strlcpy(cat2, "hello", sizeof(cat2));
    r = strlcat(cat2, "world", sizeof(cat2));
    T1C_ASSERT(r == 10);           /* src_len + dst_len */
    T1C_ASSERT(cat2[7] == '\0');   /* always NUL-terminated */
    T1C_ASSERT(strlen(cat2) == 7);

    /* ── strnlen ─────────────────────────────────────────────────────── */
    T1C_ASSERT(strnlen("abc",   10) == 3);
    T1C_ASSERT(strnlen("abc",   2)  == 2);
    T1C_ASSERT(strnlen("",      5)  == 0);
    T1C_ASSERT(strnlen("hello", 0)  == 0);

    /* ── strnchr ─────────────────────────────────────────────────────── */
    const char *hay = "hello world";
    T1C_ASSERT(strnchr(hay, 11, 'o')  == hay + 4);
    T1C_ASSERT(strnchr(hay, 4, 'o')   == NULL);  /* 'o' is at index 4, count=4 → not found */
    T1C_ASSERT(strnchr(hay, 11, 'z')  == NULL);

    /* ── strchr ──────────────────────────────────────────────────────── */
    T1C_ASSERT(strchr("hello", 'l')   == (char *)"hello" + 2);
    T1C_ASSERT(strchr("hello", 'z')   == NULL);
    T1C_ASSERT(strchr("hello", '\0')  == (char *)"hello" + 5);

    /* ── memchr ──────────────────────────────────────────────────────── */
    const char data[] = {0, 1, 2, 3, 4, 5};
    T1C_ASSERT(memchr(data, 3, 6) == (void *)(data + 3));
    T1C_ASSERT(memchr(data, 9, 6) == NULL);
    T1C_ASSERT(memchr(data, 0, 6) == (void *)data);

    /* ── memmove: non-overlapping ────────────────────────────────────── */
    char mbuf[16];
    memset(mbuf, 0, sizeof(mbuf));
    memcpy(mbuf, "ABCDE", 5);
    memmove(mbuf + 5, mbuf, 5);         /* dst > src, no overlap */
    T1C_ASSERT(memcmp(mbuf,     "ABCDE", 5) == 0);
    T1C_ASSERT(memcmp(mbuf + 5, "ABCDE", 5) == 0);

    /* ── memmove: overlapping forward ───────────────────────────────── */
    char ovl[16];
    memcpy(ovl, "12345678", 8);
    memmove(ovl + 2, ovl, 6);          /* move [0..5] to [2..7] */
    T1C_ASSERT(ovl[2] == '1');
    T1C_ASSERT(ovl[7] == '6');

    /* ── memmove: overlapping backward (src > dst) ───────────────────── */
    char ovl2[16];
    memcpy(ovl2, "12345678", 8);
    memmove(ovl2, ovl2 + 2, 6);        /* move [2..7] to [0..5] */
    T1C_ASSERT(ovl2[0] == '3');
    T1C_ASSERT(ovl2[5] == '8');

    /* ── memzero_explicit ────────────────────────────────────────────── */
    char secret[32];
    memcpy(secret, "super_secret_password_1234567890", 32);
    memzero_explicit(secret, 32);
    for (int i = 0; i < 32; i++)
        T1C_ASSERT(secret[i] == 0);

    /* ── memmove self (identical src/dst) ────────────────────────────── */
    char self[8];
    memcpy(self, "SELFCPY", 8);
    memmove(self, self, 8);
    T1C_ASSERT(memcmp(self, "SELFCPY", 8) == 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * page.h — alloc_page, get_page, put_page, page helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1c_page(void)
{
    /* ── alloc_page / put_page ───────────────────────────────────────── */
    struct page *pg = alloc_page(GFP_KERNEL);
    T1C_ASSERT(pg != NULL);
    T1C_ASSERT(pg->ref_count == 1);

    /* page_to_pfn must give a non-zero PFN */
    unsigned long pfn = page_to_pfn(pg);
    T1C_ASSERT(pfn > 0);

    /* page_address must give a valid HHDM virtual address */
    void *va = page_address(pg);
    T1C_ASSERT(va != NULL);
    T1C_ASSERT(virt_addr_valid(va));

    /* write to the page and read back */
    u32 *words = (u32 *)va;
    words[0]                   = 0xDEADBEEF;
    words[PAGE_SIZE / 4 - 1]   = 0xCAFEBABE;
    T1C_ASSERT(words[0]                 == 0xDEADBEEFU);
    T1C_ASSERT(words[PAGE_SIZE / 4 - 1] == 0xCAFEBABEU);

    /* ── get_page increments refcount ───────────────────────────────── */
    get_page(pg);
    T1C_ASSERT(pg->ref_count == 2);
    put_page(pg);  /* back to 1 — page still live */
    T1C_ASSERT(pg->ref_count == 1);

    /* ── put_page to 0 frees the page ───────────────────────────────── */
    put_page(pg);  /* refcount hits 0 → freed; pg pointer is now invalid */

    /* ── alloc_page with GFP_KERNEL | __GFP_ZERO ────────────────────── */
    struct page *zpg = alloc_page(GFP_KERNEL | __GFP_ZERO);
    T1C_ASSERT(zpg != NULL);
    u8 *zva = (u8 *)page_address(zpg);
    for (int i = 0; i < 64; i++)  /* spot-check first 64 bytes */
        T1C_ASSERT(zva[i] == 0);
    put_page(zpg);

    /* ── alloc_pages(order=1) → 2 pages ─────────────────────────────── */
    struct page *ppg = alloc_pages(GFP_KERNEL, 1);
    T1C_ASSERT(ppg != NULL);
    void *pva = page_address(ppg);
    T1C_ASSERT(pva != NULL);
    /* touch second page too */
    u8 *p2 = (u8 *)pva + PAGE_SIZE;
    p2[0] = 0x42;
    T1C_ASSERT(p2[0] == 0x42);
    __free_pages(ppg, 1);

    /* ── virt_to_page / page_to_virt round-trip ─────────────────────── */
    struct page *vpg = alloc_page(GFP_KERNEL);
    T1C_ASSERT(vpg != NULL);
    void *orig_va = page_to_virt(vpg);
    struct page *vpg2 = virt_to_page(orig_va);
    T1C_ASSERT(vpg2 != NULL);
    T1C_ASSERT(vpg2->phys == vpg->phys);
    kfree(vpg2);
    put_page(vpg);

    /* ── pfn_to_page round-trip ──────────────────────────────────────── */
    struct page *fpg = alloc_page(GFP_KERNEL);
    T1C_ASSERT(fpg != NULL);
    unsigned long fpfn = page_to_pfn(fpg);
    struct page *fpg2  = pfn_to_page(fpfn);
    T1C_ASSERT(fpg2 != NULL);
    T1C_ASSERT(fpg2->phys == fpg->phys);
    kfree(fpg2);
    put_page(fpg);

    /* ── put_page(NULL) must be safe ─────────────────────────────────── */
    put_page(NULL);
    get_page(NULL); /* no-op */
}

/* ══════════════════════════════════════════════════════════════════════════
 * mm.h — address conversion, pfn helpers, virt_addr_valid
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1c_mm(void)
{
    /* ── __pa / __va round-trip (HHDM window) ────────────────────────── */
    /* Allocate a page so we have a valid HHDM address */
    struct page *pg = alloc_page(GFP_KERNEL);
    T1C_ASSERT(pg != NULL);
    void *va = page_address(pg);
    T1C_ASSERT(va != NULL);

    u64 pa = __pa(va);
    void *va2 = __va(pa);
    T1C_ASSERT(va2 == va);

    /* phys_to_virt / virt_to_phys aliases */
    T1C_ASSERT(phys_to_virt(pa) == va);
    T1C_ASSERT(virt_to_phys(va) == pa);

    /* ── virt_to_pfn / pfn_to_virt round-trip ───────────────────────── */
    unsigned long pfn = virt_to_pfn(va);
    T1C_ASSERT(pfn == page_to_pfn(pg));
    void *va3 = pfn_to_virt(pfn);
    T1C_ASSERT(va3 == va);

    put_page(pg);

    /* ── virt_addr_valid ─────────────────────────────────────────────── */
    /* HHDM address — valid */
    struct page *vpg = alloc_page(GFP_KERNEL);
    T1C_ASSERT(vpg != NULL);
    T1C_ASSERT(virt_addr_valid(page_address(vpg)));
    put_page(vpg);

    /* Userspace address — invalid */
    T1C_ASSERT(!virt_addr_valid((void *)0x1000));
    /* VMAP region — invalid (not HHDM) */
    T1C_ASSERT(!virt_addr_valid((void *)VMAP_BASE_ADDR));

    /* ── offset_in_page ──────────────────────────────────────────────── */
    T1C_ASSERT(offset_in_page((void *)0)    == 0);
    T1C_ASSERT(offset_in_page((void *)1)    == 1);
    T1C_ASSERT(offset_in_page((void *)4095) == 4095);
    T1C_ASSERT(offset_in_page((void *)4096) == 0);
    T1C_ASSERT(offset_in_page((void *)4097) == 1);

    /* ── PAGE_ALIGN ──────────────────────────────────────────────────── */
    T1C_ASSERT(PAGE_ALIGN(0)    == 0);
    T1C_ASSERT(PAGE_ALIGN(1)    == 4096);
    T1C_ASSERT(PAGE_ALIGN(4096) == 4096);
    T1C_ASSERT(PAGE_ALIGN(4097) == 8192);

    /* ── pfn_valid for a real page ───────────────────────────────────── */
    struct page *pvpg = alloc_page(GFP_KERNEL);
    T1C_ASSERT(pvpg != NULL);
    T1C_ASSERT(pfn_valid(page_to_pfn(pvpg)));
    put_page(pvpg);
}

/* ══════════════════════════════════════════════════════════════════════════
 * vmalloc.h — interface declarations compile cleanly; is_vmalloc_addr
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_1c_vmalloc(void)
{
    /* ── is_vmalloc_addr on non-vmalloc addresses ────────────────────── */
    T1C_ASSERT(!is_vmalloc_addr(NULL));
    T1C_ASSERT(!is_vmalloc_addr((void *)0x1000));   /* user space */

    /* An HHDM address (page_address) must NOT be in the VMAP window */
    struct page *pg = alloc_page(GFP_KERNEL);
    T1C_ASSERT(pg != NULL);
    T1C_ASSERT(!is_vmalloc_addr(page_address(pg)));
    put_page(pg);

    /* ── is_vmalloc_addr on VMAP-range address ───────────────────────── */
    /* Construct a fake pointer inside the VMAP window */
    void *fake_vmap = (void *)(uintptr_t)(VMAP_BASE_ADDR + 0x1000ULL);
    T1C_ASSERT(is_vmalloc_addr(fake_vmap));

    /* Boundary: exactly at VMAP_BASE_ADDR */
    T1C_ASSERT(is_vmalloc_addr((void *)(uintptr_t)VMAP_BASE_ADDR));

    /* Boundary: just before VMAP_END_ADDR */
    void *vmap_end_minus = (void *)(uintptr_t)(VMAP_END_ADDR - 1ULL);
    T1C_ASSERT(is_vmalloc_addr(vmap_end_minus));

    /* Just past the end — NOT in VMAP */
    void *vmap_end = (void *)(uintptr_t)VMAP_END_ADDR;
    T1C_ASSERT(!is_vmalloc_addr(vmap_end));

    /*
     * vmalloc itself is implemented in Phase 2.
     * Here we only verify that calling it doesn't crash the kernel when
     * the implementation is a stub that returns NULL.
     */
    void *vp = vmalloc(4096);
    if (vp) {
        /* Phase 2 is implemented — basic write test */
        ((u8 *)vp)[0]    = 0xAA;
        ((u8 *)vp)[4095] = 0xBB;
        T1C_ASSERT(((u8 *)vp)[0]    == 0xAA);
        T1C_ASSERT(((u8 *)vp)[4095] == 0xBB);
        vfree(vp);
    } else {
        klog_puts("  [1c] vmalloc stub returns NULL (Phase 2 not yet wired)\n");
    }
}

/* ── public entry point ────────────────────────────────────────────────── */
void linuxkpi_test_1c(void)
{
    klog_puts(KLOG_CLR_CYAN
              "[TEST ] Phase 1c — LinuxKPI slab/string/page/mm/vmalloc\n"
              KLOG_CLR_RESET);

    test_1c_slab();
    test_1c_string();
    test_1c_page();
    test_1c_mm();
    test_1c_vmalloc();

    if (g_1c_fail) {
        klog_puts(KLOG_CLR_RED
                  "[ FAIL] Phase 1c LinuxKPI stress test — see failures above\n"
                  KLOG_CLR_RESET);
    } else {
        klog_puts(KLOG_CLR_GREEN
                  "[  OK ] Phase 1c LinuxKPI stress test PASSED\n"
                  KLOG_CLR_RESET);
    }
}
