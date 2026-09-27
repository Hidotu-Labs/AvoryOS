/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_SLAB_H
#define ASCENT_LINUX_SLAB_H

/*
 * LinuxKPI — slab.h
 * Maps Linux memory-allocation APIs onto AscentOS's heap (mm/heap.h).
 *
 * Design note: the native heap exports kmalloc(size), kcalloc(n, size),
 * krealloc(ptr, size) with 2-arg signatures.  Linux drivers call 3-arg
 * forms that also take a gfp_t.  We bridge them using function-pointer
 * aliases that are captured BEFORE the Linux macros shadow the names, so
 * macro expansion never recursively expands the wrong arity.
 */

#include <linux/gfp.h>
#include <linux/types.h>

/* ── Forward-declare native heap functions (mm/heap.h, linked in) ─────── */
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *kcalloc(size_t num, size_t size);
void *krealloc(void *ptr, size_t new_size);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

/*
 * Capture the native 2-arg function addresses into static inline helpers
 * BEFORE defining any macros that shadow those names.  These inlines are
 * never shadowed because they have the __lkpi_ prefix.
 */
static __attribute__((always_inline)) inline void *
__lkpi_kmalloc(size_t sz) { return kmalloc(sz); }

static __attribute__((always_inline)) inline void *
__lkpi_kcalloc(size_t n, size_t sz) { return kcalloc(n, sz); }

static __attribute__((always_inline)) inline void *
__lkpi_krealloc(void *p, size_t sz) { return krealloc(p, sz); }

/* ── Linux-compatible 3-arg wrappers ─────────────────────────────────── */

/*
 * kmalloc(size, flags)
 * __GFP_ZERO flag triggers a zero-fill, matching Linux semantics.
 */
#define kmalloc(size, flags) ({                         \
    void *_p = __lkpi_kmalloc(size);                    \
    if (_p && ((flags) & __GFP_ZERO))                   \
        memset(_p, 0, (size));                          \
    _p;                                                 \
})

/* kzalloc — always zero-initialised */
#define kzalloc(size, flags)        __lkpi_kcalloc(1, (size))

/* kcalloc — n elements of size bytes, zero-initialised */
#define kcalloc(n, size, flags)     __lkpi_kcalloc((n), (size))

/* krealloc — resize */
#define krealloc(ptr, size, flags)  __lkpi_krealloc((ptr), (size))

/* ── Higher-level helpers ─────────────────────────────────────────────── */

static inline void *kmemdup(const void *src, size_t len, gfp_t flags)
{
    void *dst = kmalloc(len, flags);
    if (dst)
        memcpy(dst, src, len);
    return dst;
}

static inline char *kstrdup(const char *s, gfp_t flags)
{
    size_t len = 0;
    while (s[len]) len++;
    return (char *)kmemdup(s, len + 1, flags);
}

static inline char *kstrndup(const char *s, size_t max, gfp_t flags)
{
    size_t len = 0;
    while (len < max && s[len]) len++;
    char *out = (char *)kmalloc(len + 1, flags);
    if (out) {
        memcpy(out, s, len);
        out[len] = '\0';
    }
    return out;
}

static inline void kfree_const(const void *x)
{
    kfree((void *)x);
}

/* kvmalloc family — we don't have a separate vmalloc pool yet, use kmalloc */
#define kvmalloc(size, flags)           kmalloc((size), (flags))
#define kvzalloc(size, flags)           kzalloc((size), (flags))
#define kvfree(ptr)                     kfree(ptr)
#define kvmalloc_array(n, size, flags)  kcalloc((n), (size), (flags))

/* devm_* — device-managed; not truly tracked here, maps to plain alloc/free */
struct device;

static inline void *devm_kmalloc(struct device *dev, size_t size, gfp_t flags)
{
    (void)dev;
    return kmalloc(size, flags);
}

static inline void *devm_kzalloc(struct device *dev, size_t size, gfp_t flags)
{
    (void)dev;
    return __lkpi_kcalloc(1, size);   /* bypass kzalloc macro entirely */
}

static inline void *devm_kcalloc(struct device *dev, size_t n, size_t size,
                                  gfp_t flags)
{
    (void)dev;
    return __lkpi_kcalloc(n, size);
}

static inline void devm_kfree(struct device *dev, const void *p)
{
    (void)dev;
    kfree((void *)p);
}

#endif /* ASCENT_LINUX_SLAB_H */
