/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_PRINTK_H
#define ASCENT_LINUX_PRINTK_H

/*
 * LinuxKPI — printk.h
 *
 * Maps Linux printk / dev_err / dev_warn / dev_info onto AscentOS's
 * klog_puts + snprintf stack.  No ring buffer, no log levels stored —
 * everything goes to the serial log immediately.
 *
 * The `struct device *` is accepted but ignored; drivers pass it for
 * dev_err/dev_warn/dev_info but we have no device-name database yet.
 */

#include <linux/types.h>
#include "lib/string.h"    /* snprintf */
#include "console/klog.h"  /* klog_puts */

/* ── printk log-level prefix strings (Linux convention) ─────────────────── */
#define KERN_EMERG      "\001" "0"   /* system is unusable */
#define KERN_ALERT      "\001" "1"   /* action must be taken immediately */
#define KERN_CRIT       "\001" "2"   /* critical conditions */
#define KERN_ERR        "\001" "3"   /* error conditions */
#define KERN_WARNING    "\001" "4"   /* warning conditions */
#define KERN_NOTICE     "\001" "5"   /* normal but significant condition */
#define KERN_INFO       "\001" "6"   /* informational */
#define KERN_DEBUG      "\001" "7"   /* debug-level messages */
#define KERN_DEFAULT    "\001" "d"

/* ── printk() implementation ─────────────────────────────────────────────── */
/*
 * Declared as a real function so callers that take its address compile.
 * Defined in linuxkpi/printk.c.
 */
int printk(const char *fmt, ...) __attribute__((__format__(printf, 1, 2)));

/* ── pr_* convenience wrappers ───────────────────────────────────────────── */
#define pr_emerg(fmt, ...)   printk(KERN_EMERG   fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   printk(KERN_ALERT   fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    printk(KERN_CRIT    fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     printk(KERN_ERR     fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  printk(KERN_NOTICE  fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    printk(KERN_INFO    fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   printk(KERN_DEBUG   fmt, ##__VA_ARGS__)
#define pr_devel(fmt, ...)   printk(KERN_DEBUG   fmt, ##__VA_ARGS__)

/* Once-only variants */
#define pr_err_once(fmt, ...) ({                    \
    static int __done;                              \
    if (!__done) { __done = 1;                      \
        pr_err(fmt, ##__VA_ARGS__); }               \
})
#define pr_warn_once(fmt, ...) ({                   \
    static int __done;                              \
    if (!__done) { __done = 1;                      \
        pr_warn(fmt, ##__VA_ARGS__); }              \
})
#define pr_info_once(fmt, ...) ({                   \
    static int __done;                              \
    if (!__done) { __done = 1;                      \
        pr_info(fmt, ##__VA_ARGS__); }              \
})

/* ── dev_* wrappers (struct device * ignored) ───────────────────────────── */
struct device;

#define dev_err(dev,  fmt, ...)   printk(KERN_ERR     "amdgpu: " fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)   printk(KERN_WARNING "amdgpu: " fmt, ##__VA_ARGS__)
#define dev_notice(dev,fmt,...) printk(KERN_NOTICE  "amdgpu: " fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)   printk(KERN_INFO    "amdgpu: " fmt, ##__VA_ARGS__)
#define dev_dbg(dev,  fmt, ...)   printk(KERN_DEBUG   "amdgpu: " fmt, ##__VA_ARGS__)

/* Rate-limited variants — delegate to regular for now */
#define dev_err_ratelimited(dev,  fmt, ...)  dev_err(dev,  fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...)  dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...)  dev_info(dev, fmt, ##__VA_ARGS__)

/* ── drm_* DRM-subsystem log wrappers ────────────────────────────────────── */
#define drm_err(drm,  fmt, ...)   printk(KERN_ERR     "drm: " fmt, ##__VA_ARGS__)
#define drm_warn(drm, fmt, ...)   printk(KERN_WARNING "drm: " fmt, ##__VA_ARGS__)
#define drm_info(drm, fmt, ...)   printk(KERN_INFO    "drm: " fmt, ##__VA_ARGS__)
#define drm_dbg(drm, cat, fmt,...) printk(KERN_DEBUG  "drm: " fmt, ##__VA_ARGS__)

#define drm_err_ratelimited(drm, fmt, ...)   drm_err(drm, fmt, ##__VA_ARGS__)
#define drm_warn_ratelimited(drm, fmt, ...)  drm_warn(drm, fmt, ##__VA_ARGS__)

#endif /* ASCENT_LINUX_PRINTK_H */
