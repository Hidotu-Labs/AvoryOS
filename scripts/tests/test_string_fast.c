#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <x86intrin.h>

typedef uint64_t __attribute__((__may_alias__, aligned(1))) u64_ua;
typedef uint32_t __attribute__((__may_alias__, aligned(1))) u32_ua;

#define ONES64  0x0101010101010101ULL
#define HIGHS64 0x8080808080808080ULL

static inline uint64_t has_zero_byte(uint64_t v) {
    return (v - ONES64) & ~v & HIGHS64;
}

/* --- Optimized strlen --- */
size_t fast_strlen(const char *s) {
    const char *orig = s;

    // Align s to 8-byte boundary
    while ((uintptr_t)s & 7) {
        if (!*s) return (size_t)(s - orig);
        s++;
    }

    const uint64_t *w = (const uint64_t *)s;
    while (1) {
        uint64_t v = *w;
        uint64_t mask = has_zero_byte(v);
        if (mask) {
            return (size_t)((const char *)w - orig) + (__builtin_ctzll(mask) >> 3);
        }
        w++;
    }
}

/* --- Optimized strcmp --- */
int fast_strcmp(const char *s1, const char *s2) {
    // Check initial bytes until s1 is aligned or differ/null
    while (((uintptr_t)s1 & 7) && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    if (!*s1 || *s1 != *s2) {
        return *(const unsigned char *)s1 - *(const unsigned char *)s2;
    }

    // Both or at least s1 is aligned.
    // For s2: safe if within page (offset <= 4096 - 8)
    while (1) {
        if (__builtin_expect((((uintptr_t)s2 & 4095) > 4096 - 8), 0)) {
            // Near page boundary: fall back to byte comparison for this word
            for (int i = 0; i < 8; i++) {
                if (!*s1 || *s1 != *s2)
                    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
                s1++;
                s2++;
            }
            continue;
        }

        uint64_t v1 = *(const uint64_t *)s1;
        uint64_t v2 = *(const u64_ua *)s2;

        if (v1 != v2) {
            // Found difference: resolve byte
            for (int i = 0; i < 8; i++) {
                unsigned char c1 = (unsigned char)s1[i];
                unsigned char c2 = (unsigned char)s2[i];
                if (c1 != c2 || c1 == 0) return c1 - c2;
            }
        }

        uint64_t z = has_zero_byte(v1);
        if (z) {
            return 0; // Both matched up to null terminator
        }

        s1 += 8;
        s2 += 8;
    }
}

/* --- Optimized strncmp --- */
int fast_strncmp(const char *s1, const char *s2, size_t n) {
    while (n && ((uintptr_t)s1 & 7) && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    if (!*s1 || *s1 != *s2) {
        return *(const unsigned char *)s1 - *(const unsigned char *)s2;
    }

    while (n >= 8) {
        if (__builtin_expect((((uintptr_t)s2 & 4095) > 4096 - 8), 0)) {
            for (int i = 0; i < 8; i++) {
                if (!*s1 || *s1 != *s2)
                    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
                s1++;
                s2++;
                n--;
            }
            continue;
        }

        uint64_t v1 = *(const uint64_t *)s1;
        uint64_t v2 = *(const u64_ua *)s2;

        if (v1 != v2) {
            for (int i = 0; i < 8; i++) {
                unsigned char c1 = (unsigned char)s1[i];
                unsigned char c2 = (unsigned char)s2[i];
                if (c1 != c2 || c1 == 0) return c1 - c2;
            }
        }

        if (has_zero_byte(v1)) {
            return 0;
        }

        s1 += 8;
        s2 += 8;
        n -= 8;
    }

    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* --- Optimized strcpy --- */
char *fast_strcpy(char *dest, const char *src) {
    char *orig_dest = dest;

    // Align src to 8-byte boundary
    while ((uintptr_t)src & 7) {
        if (!(*dest++ = *src++)) return orig_dest;
    }

    while (1) {
        uint64_t v = *(const uint64_t *)src;
        uint64_t z = has_zero_byte(v);
        if (z) {
            int len = __builtin_ctzll(z) >> 3;
            for (int i = 0; i <= len; i++) {
                dest[i] = src[i];
            }
            return orig_dest;
        }
        *(u64_ua *)dest = v;
        src += 8;
        dest += 8;
    }
}

/* --- Optimized strchr --- */
char *fast_strchr(const char *s, int c) {
    uint8_t ch = (uint8_t)c;
    if (ch == 0) return (char *)s + fast_strlen(s);

    while ((uintptr_t)s & 7) {
        if (*s == (char)ch) return (char *)s;
        if (!*s) return NULL;
        s++;
    }

    uint64_t c8 = ch * ONES64;
    const uint64_t *w = (const uint64_t *)s;

    while (1) {
        uint64_t v = *w;
        uint64_t z = has_zero_byte(v);
        uint64_t match = v ^ c8;
        uint64_t m = has_zero_byte(match);

        if (z || m) {
            const char *p = (const char *)w;
            for (int i = 0; i < 8; i++) {
                if ((uint8_t)p[i] == ch) return (char *)p + i;
                if (p[i] == 0) return NULL;
            }
        }
        w++;
    }
}

int main(void) {
    printf("Testing correctness...\n");
    char buf[256];
    char dst[256];

    // Correctness tests for strlen
    for (int len = 0; len < 120; len++) {
        for (int align = 0; align < 8; align++) {
            memset(buf, 'A', sizeof(buf));
            char *p = buf + align;
            p[len] = '\0';
            assert(fast_strlen(p) == (size_t)len);
        }
    }
    printf("fast_strlen correctness: PASSED\n");

    // Correctness tests for strcmp
    for (int len = 0; len < 120; len++) {
        for (int a1 = 0; a1 < 8; a1++) {
            for (int a2 = 0; a2 < 8; a2++) {
                char s1[256], s2[256];
                memset(s1, 'x', sizeof(s1));
                memset(s2, 'x', sizeof(s2));
                char *p1 = s1 + a1;
                char *p2 = s2 + a2;
                p1[len] = '\0';
                p2[len] = '\0';
                assert(fast_strcmp(p1, p2) == 0);
                assert(fast_strncmp(p1, p2, len + 10) == 0);

                if (len > 0) {
                    p2[len - 1] = 'y';
                    assert(fast_strcmp(p1, p2) < 0);
                    assert(fast_strcmp(p2, p1) > 0);
                    p2[len - 1] = 'x';
                }
            }
        }
    }
    printf("fast_strcmp / fast_strncmp correctness: PASSED\n");

    // Correctness tests for strcpy
    for (int len = 0; len < 120; len++) {
        for (int a1 = 0; a1 < 8; a1++) {
            for (int a2 = 0; a2 < 8; a2++) {
                char s1[256], s2[256];
                memset(s1, 'M', sizeof(s1));
                memset(s2, 0, sizeof(s2));
                char *src = s1 + a1;
                char *d = s2 + a2;
                src[len] = '\0';
                fast_strcpy(d, src);
                assert(strcmp(d, src) == 0);
            }
        }
    }
    printf("fast_strcpy correctness: PASSED\n");

    // Correctness tests for strchr
    for (int len = 1; len < 120; len++) {
        for (int a = 0; a < 8; a++) {
            char s[256];
            memset(s, 'A', sizeof(s));
            char *p = s + a;
            p[len] = '\0';
            p[len / 2] = '/';
            assert(fast_strchr(p, '/') == p + (len / 2));
            assert(fast_strchr(p, 'Z') == NULL);
        }
    }
    printf("fast_strchr correctness: PASSED\n\n");

    /* Performance Benchmarks */
    printf("===================================================================\n");
    printf("        Kernel Freestanding String Operations Microbenchmark        \n");
    printf("===================================================================\n\n");

    const int ITERS = 1000000;
    char path_buf[256];
    char path_buf2[256];
    snprintf(path_buf, sizeof(path_buf), "/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.20.0");
    snprintf(path_buf2, sizeof(path_buf2), "/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.20.1");

    char *dyn_path = path_buf;
    char *dyn_path2 = path_buf2;
    __asm__ volatile("" : "+r"(dyn_path), "+r"(dyn_path2));
    size_t path_len = strlen(dyn_path);

    // 1. strlen benchmark
    uint64_t t0 = __rdtsc();
    volatile size_t l = 0;
    for (int i = 0; i < ITERS; i++) {
        size_t len = 0;
        const char *p = dyn_path;
        __asm__ volatile("" : "+r"(p));
        while (p[len]) len++;
        l += len;
    }
    uint64_t cycles_strlen_scalar = (__rdtsc() - t0) / ITERS;

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        const char *p = dyn_path;
        __asm__ volatile("" : "+r"(p));
        l += fast_strlen(p);
    }
    uint64_t cycles_strlen_fast = (__rdtsc() - t0) / ITERS;

    printf("[1] strlen(path, len=%zu):\n", path_len);
    printf("    Scalar Byte-by-Byte (Kernel Baseline) : %4lu cycles/call\n", cycles_strlen_scalar);
    printf("    Word-at-a-Time (Optimized)            : %4lu cycles/call\n", cycles_strlen_fast);
    printf("    -> Speedup                            : %.2fX FASTER\n\n", (double)cycles_strlen_scalar / cycles_strlen_fast);

    // 2. strcmp benchmark
    t0 = __rdtsc();
    volatile int cmp_res = 0;
    for (int i = 0; i < ITERS; i++) {
        const char *s1 = dyn_path;
        const char *s2 = dyn_path2;
        __asm__ volatile("" : "+r"(s1), "+r"(s2));
        while (*s1 && (*s1 == *s2)) { s1++; s2++; }
        cmp_res += *(const unsigned char *)s1 - *(const unsigned char *)s2;
    }
    uint64_t cycles_strcmp_scalar = (__rdtsc() - t0) / ITERS;

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        const char *s1 = dyn_path;
        const char *s2 = dyn_path2;
        __asm__ volatile("" : "+r"(s1), "+r"(s2));
        cmp_res += fast_strcmp(s1, s2);
    }
    uint64_t cycles_strcmp_fast = (__rdtsc() - t0) / ITERS;

    printf("[2] strcmp(path1, path2, diff at end):\n");
    printf("    Scalar Byte-by-Byte (Kernel Baseline) : %4lu cycles/call\n", cycles_strcmp_scalar);
    printf("    Word-at-a-Time (Optimized)            : %4lu cycles/call\n", cycles_strcmp_fast);
    printf("    -> Speedup                            : %.2fX FASTER\n\n", (double)cycles_strcmp_scalar / cycles_strcmp_fast);

    // 3. strchr benchmark
    t0 = __rdtsc();
    volatile char *c_res;
    for (int i = 0; i < ITERS; i++) {
        const char *s = dyn_path;
        __asm__ volatile("" : "+r"(s));
        while (*s && *s != '0') s++;
        c_res = (char *)s;
    }
    uint64_t cycles_strchr_scalar = (__rdtsc() - t0) / ITERS;

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        const char *s = dyn_path;
        __asm__ volatile("" : "+r"(s));
        c_res = fast_strchr(s, '0');
    }
    uint64_t cycles_strchr_fast = (__rdtsc() - t0) / ITERS;

    printf("[3] strchr(path, '0' search near end):\n");
    printf("    Scalar Byte-by-Byte (Kernel Baseline) : %4lu cycles/call\n", cycles_strchr_scalar);
    printf("    Word-at-a-Time (Optimized)            : %4lu cycles/call\n", cycles_strchr_fast);
    printf("    -> Speedup                            : %.2fX FASTER\n\n", (double)cycles_strchr_scalar / cycles_strchr_fast);

    // 4. strcpy benchmark
    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        char *d = dst;
        const char *s = dyn_path;
        __asm__ volatile("" : "+r"(d), "+r"(s));
        while ((*d++ = *s++));
    }
    uint64_t cycles_strcpy_scalar = (__rdtsc() - t0) / ITERS;

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        char *d = dst;
        const char *s = dyn_path;
        __asm__ volatile("" : "+r"(d), "+r"(s));
        fast_strcpy(d, s);
    }
    uint64_t cycles_strcpy_fast = (__rdtsc() - t0) / ITERS;

    printf("[4] strcpy(dest, path):\n");
    printf("    Scalar Byte-by-Byte (Kernel Baseline) : %4lu cycles/call\n", cycles_strcpy_scalar);
    printf("    Word-at-a-Time (Optimized)            : %4lu cycles/call\n", cycles_strcpy_fast);
    printf("    -> Speedup                            : %.2fX FASTER\n\n", (double)cycles_strcpy_scalar / cycles_strcpy_fast);

    // 5. Long path benchmark (160 bytes)
    char long_path[256];
    memset(long_path, 'a', 160);
    long_path[160] = '\0';
    char *dyn_long = long_path;
    __asm__ volatile("" : "+r"(dyn_long));

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        size_t len = 0;
        const char *p = dyn_long;
        __asm__ volatile("" : "+r"(p));
        while (p[len]) len++;
        l += len;
    }
    uint64_t cycles_long_scalar = (__rdtsc() - t0) / ITERS;

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        const char *p = dyn_long;
        __asm__ volatile("" : "+r"(p));
        l += fast_strlen(p);
    }
    uint64_t cycles_long_fast = (__rdtsc() - t0) / ITERS;

    printf("[5] strlen(long_path, len=160):\n");
    printf("    Scalar Byte-by-Byte (Kernel Baseline) : %4lu cycles/call\n", cycles_long_scalar);
    printf("    Word-at-a-Time (Optimized)            : %4lu cycles/call\n", cycles_long_fast);
    printf("    -> Speedup                            : %.2fX FASTER\n\n", (double)cycles_long_scalar / cycles_long_fast);

    printf("===================================================================\n");
    printf("                      BENCHMARK COMPLETE                           \n");
    printf("===================================================================\n");

    return 0;
}
