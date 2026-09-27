/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_KERNEL_H
#define ASCENT_LINUX_KERNEL_H

/*
 * LinuxKPI — kernel.h
 * Core macros used throughout driver code: min/max, DIV_ROUND_UP, BIT,
 * ARRAY_SIZE, container_of, likely/unlikely, WARN_ON, etc.
 */

#include <linux/types.h>
#include <linux/bitops.h>   /* BIT() lives here but re-export for compat */

/* ── Compiler hints ──────────────────────────────────────────────────────── */

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#define __must_check    __attribute__((__warn_unused_result__))
#define __packed        __attribute__((__packed__))
#define __aligned(x)    __attribute__((__aligned__(x)))
#define __printf(a,b)   __attribute__((__format__(printf, a, b)))
#define __cold          __attribute__((__cold__))
#define __noreturn      __attribute__((__noreturn__))
#define noinline        __attribute__((__noinline__))
#define __always_inline __attribute__((__always_inline__)) inline
#define __weak          __attribute__((__weak__))
#define __used          __attribute__((__used__))
#define __deprecated    __attribute__((__deprecated__))
#define __maybe_unused  __attribute__((__unused__))

/* ── Arithmetic macros ───────────────────────────────────────────────────── */

/* Type-safe min/max that avoid double-evaluation */
#define min(a, b) ({                        \
    __typeof__(a) _a = (a);                 \
    __typeof__(b) _b = (b);                 \
    _a < _b ? _a : _b;                      \
})

#define max(a, b) ({                        \
    __typeof__(a) _a = (a);                 \
    __typeof__(b) _b = (b);                 \
    _a > _b ? _a : _b;                      \
})

#define min3(a, b, c)   min(min(a, b), c)
#define max3(a, b, c)   max(max(a, b), c)

/* Clamp x to [lo, hi] */
#define clamp(x, lo, hi) ({                 \
    __typeof__(x) _x = (x);                 \
    __typeof__(lo) _lo = (lo);              \
    __typeof__(hi) _hi = (hi);              \
    _x < _lo ? _lo : (_x > _hi ? _hi : _x); \
})

#define clamp_t(type, x, lo, hi)    clamp((type)(x), (type)(lo), (type)(hi))
#define clamp_val(x, lo, hi)        clamp_t(__typeof__(x), x, lo, hi)

/* min_t / max_t: explicit type cast before comparison */
#define min_t(type, a, b)   min((type)(a), (type)(b))
#define max_t(type, a, b)   max((type)(a), (type)(b))

/* DIV_ROUND_UP(n, d): integer division rounding up */
#define DIV_ROUND_UP(n, d)          (((n) + (d) - 1) / (d))
#define DIV_ROUND_DOWN(n, d)        ((n) / (d))
#define DIV_ROUND_CLOSEST(n, d) ({              \
    __typeof__(n) _n = (n);                     \
    __typeof__(d) _d = (d);                     \
    (_n + (_d / 2)) / _d;                       \
})

/* Round n up/down to the nearest multiple of m (m must be power-of-2) */
#define ROUND_UP(n, m)      (((n) + (m) - 1) & ~((m) - 1))
#define ROUND_DOWN(n, m)    ((n) & ~((m) - 1))

/* Absolute value (avoids float; for integer types only) */
#define abs(x) ({                           \
    __typeof__(x) _x = (x);                 \
    _x < 0 ? -_x : _x;                     \
})

/* ── Array / struct helpers ──────────────────────────────────────────────── */

#define ARRAY_SIZE(arr)     (sizeof(arr) / sizeof((arr)[0]))

/*
 * container_of — get pointer to enclosing struct from a member pointer.
 * Uses a compile-time type-check via __typeof__.
 */
#define container_of(ptr, type, member) ({                      \
    const __typeof__(((type *)0)->member) *_mptr = (ptr);       \
    (type *)((char *)_mptr - offsetof(type, member));           \
})

/* offsetof is provided by <stddef.h> already included via linux/types.h */

/* ── Stringify / token-paste ─────────────────────────────────────────────── */

#define __stringify_1(x)    #x
#define __stringify(x)      __stringify_1(x)

#define ___PASTE(a, b)      a##b
#define __PASTE(a, b)       ___PASTE(a, b)

/* ── Misc ────────────────────────────────────────────────────────────────── */

/* Intentional no-op; used to silence "unused variable" warnings */
#define UNUSED(x)           ((void)(x))

/* Suppress -Wunused-result */
#define ignore_result(x)    ({ __typeof__(x) _r = (x); (void)_r; })

/* BUILD_BUG_ON: compile-time assertion */
#define BUILD_BUG_ON(cond)  _Static_assert(!(cond), "BUILD_BUG_ON: " #cond)
#define BUILD_BUG_ON_ZERO(cond) (sizeof(struct { int:(-!!(cond)); }))

/* Swap two values of the same type */
#define swap(a, b) ({               \
    __typeof__(a) _t = (a);         \
    (a) = (b);                      \
    (b) = _t;                       \
})

/* ── WARN_ON (non-fatal assertion with log) ──────────────────────────────── */
/*
 * Declared here; the actual printk call resolves at link-time via printk.h.
 * Drivers include kernel.h, which in turn relies on printk.h being included
 * somewhere in the TU (usually via the driver's own headers).
 */
#ifndef WARN_ON
#define WARN_ON(cond) ({                                        \
    int __ret_warn = !!(cond);                                  \
    if (unlikely(__ret_warn)) {                                 \
        printk("WARNING: %s:%d %s\n",                          \
               __FILE__, __LINE__, __func__);                   \
    }                                                           \
    unlikely(__ret_warn);                                       \
})
#endif

#ifndef WARN
#define WARN(cond, fmt, ...) ({                                 \
    int __ret_warn = !!(cond);                                  \
    if (unlikely(__ret_warn))                                   \
        printk("WARNING: " fmt, ##__VA_ARGS__);                 \
    unlikely(__ret_warn);                                       \
})
#endif

#ifndef WARN_ONCE
/* Simple non-race-free once; good enough for a single-threaded boot path */
#define WARN_ONCE(cond, fmt, ...) ({                            \
    static int __warned;                                        \
    int __ret_warn = !!(cond);                                  \
    if (unlikely(__ret_warn && !__warned)) {                    \
        __warned = 1;                                           \
        printk("WARNING (once): " fmt, ##__VA_ARGS__);         \
    }                                                           \
    unlikely(__ret_warn);                                       \
})
#endif

/* Forward-declare printk so kernel.h is self-contained */
int printk(const char *fmt, ...) __attribute__((__format__(printf, 1, 2)));

#endif /* ASCENT_LINUX_KERNEL_H */
