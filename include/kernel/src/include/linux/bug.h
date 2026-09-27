/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_BUG_H
#define ASCENT_LINUX_BUG_H

/*
 * LinuxKPI — bug.h
 * BUG / BUG_ON / WARN / WARN_ON family.
 *
 * BUG() halts the kernel (infinite loop after logging); the driver must
 * never call it on a live system path but it satisfies the symbol requirement.
 * WARN_ON is non-fatal; it logs and continues.
 */

#include <linux/printk.h>

/* ── Halt helpers ────────────────────────────────────────────────────────── */

/* Unreachable marker for the compiler */
#define unreachable()   __builtin_unreachable()

/*
 * BUG() — unrecoverable error.  Logs file/line then spins forever.
 * Declared __noreturn so GCC knows control never returns.
 */
#define BUG() do {                                              \
    printk(KERN_EMERG "BUG: failure at %s:%d/%s()!\n",         \
           __FILE__, __LINE__, __func__);                       \
    for (;;) { __asm__ volatile("hlt" ::: "memory"); }         \
} while (0)

#define BUG_ON(cond) do {                                       \
    if (unlikely(cond))                                         \
        BUG();                                                  \
} while (0)

/* ── Non-fatal assertions ────────────────────────────────────────────────── */

/*
 * WARN() — logs a warning with file/line, evaluates to the condition.
 * Returns the boolean value of cond so callers can do:
 *   if (WARN(x < 0, "bad value %d\n", x)) return;
 */
#define WARN(cond, fmt, ...) ({                                 \
    int __ret_warn_on = !!(cond);                               \
    if (unlikely(__ret_warn_on))                                \
        printk(KERN_WARNING "WARNING at %s:%d %s(): " fmt,     \
               __FILE__, __LINE__, __func__, ##__VA_ARGS__);    \
    unlikely(__ret_warn_on);                                    \
})

#define WARN_ON(cond) ({                                        \
    int __ret_warn_on = !!(cond);                               \
    if (unlikely(__ret_warn_on))                                \
        printk(KERN_WARNING "WARNING at %s:%d %s()\n",         \
               __FILE__, __LINE__, __func__);                   \
    unlikely(__ret_warn_on);                                    \
})

/* Once-only variants — use a static flag to fire at most once */
#define WARN_ON_ONCE(cond) ({                                   \
    static int __warned;                                        \
    int __ret_warn_on = !!(cond);                               \
    if (unlikely(__ret_warn_on && !__warned)) {                 \
        __warned = 1;                                           \
        printk(KERN_WARNING "WARNING (once) at %s:%d %s()\n",  \
               __FILE__, __LINE__, __func__);                   \
    }                                                           \
    unlikely(__ret_warn_on);                                    \
})

#define WARN_ONCE(cond, fmt, ...) ({                            \
    static int __warned;                                        \
    int __ret_warn_on = !!(cond);                               \
    if (unlikely(__ret_warn_on && !__warned)) {                 \
        __warned = 1;                                           \
        printk(KERN_WARNING "WARNING (once) at %s:%d: " fmt,   \
               __FILE__, __LINE__, ##__VA_ARGS__);              \
    }                                                           \
    unlikely(__ret_warn_on);                                    \
})

/* ── Static analysis / unreachable ──────────────────────────────────────── */

/* WARN_ON_ONCE for use in interrupt context (same impl here) */
#define WARN_ON_ONCE_IRQ(cond)  WARN_ON_ONCE(cond)

/* Placeholders that compile to nothing in non-debug builds */
#define MAYBE_BUILD_BUG_ON(cond)    BUILD_BUG_ON(cond)

/* VM_WARN_ON — VMM subsystem non-fatal assert */
#define VM_WARN_ON(cond)    WARN_ON(cond)
#define VM_BUG_ON(cond)     BUG_ON(cond)

#include <linux/kernel.h>   /* for BUILD_BUG_ON, unlikely */

#endif /* ASCENT_LINUX_BUG_H */
