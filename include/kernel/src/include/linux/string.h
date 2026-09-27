/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_STRING_H
#define ASCENT_LINUX_STRING_H

/*
 * LinuxKPI — string.h
 * Re-exports AscentOS lib/string.h under the Linux include path and adds
 * strlcpy, strnchr, and strnlen stubs used by amdgpu / DRM code.
 */

#include "lib/string.h"    /* strcmp, strlen, memcpy, memset, snprintf ... */
#include <linux/types.h>

/* ── Extensions not in lib/string.h ────────────────────────────────────── */

/*
 * strlcpy(dst, src, size) — copy at most size-1 bytes, always NUL-terminate.
 * Returns strlen(src).
 */
static inline size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t src_len = strlen(src);
    if (size) {
        size_t copy_len = src_len < size - 1 ? src_len : size - 1;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return src_len;
}

/*
 * strlcat(dst, src, size) — append src to dst, limit total to size bytes.
 * Returns initial strlen(dst) + strlen(src).
 */
static inline size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dst_len = strlen(dst);
    size_t src_len = strlen(src);
    if (dst_len < size - 1) {
        size_t copy_len = src_len < size - 1 - dst_len ? src_len : size - 1 - dst_len;
        memcpy(dst + dst_len, src, copy_len);
        dst[dst_len + copy_len] = '\0';
    }
    return dst_len + src_len;
}

/*
 * strnlen(s, maxlen) — length of s, but not more than maxlen.
 */
static inline size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n])
        n++;
    return n;
}

/*
 * strnchr(s, count, c) — find first occurrence of c in first count bytes.
 * Returns pointer to match or NULL.
 */
static inline char *strnchr(const char *s, size_t count, int c)
{
    for (size_t i = 0; i < count; i++) {
        if (s[i] == (char)c)
            return (char *)(s + i);
        if (!s[i])
            return NULL;
    }
    return NULL;
}

/*
 * strchr — find first occurrence of c in s (including NUL).
 * lib/string.h doesn't expose this; provide it here.
 */
static inline char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return c == '\0' ? (char *)s : NULL;
}

/*
 * memchr — find first occurrence of c in s[0..n).
 */
static inline void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    }
    return NULL;
}

/*
 * memzero_explicit — zero memory in a way the compiler won't optimize away.
 * Used for clearing sensitive buffers (keys, passwords).
 */
static inline void memzero_explicit(void *s, size_t count)
{
    memset(s, 0, count);
    /* Barrier to prevent the compiler from optimizing the memset away */
    __asm__ volatile("" : : "r"(s) : "memory");
}

#endif /* ASCENT_LINUX_STRING_H */
