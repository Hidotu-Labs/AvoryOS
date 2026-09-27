/* SPDX-License-Identifier: GPL-2.0 */
/*
 * LinuxKPI — printk.c
 *
 * Implements the Linux-compatible printk() on top of AscentOS's
 * klog_puts + snprintf stack.
 *
 * Log-level prefixes (KERN_ERR "\001" "3" etc.) are stripped before
 * output so the serial log stays readable.
 */

#include <linux/printk.h>
#include "console/klog.h"
#include "lib/string.h"

#include <stdarg.h>

/* Size of the per-call format buffer.  Matches Linux's LOG_LINE_MAX. */
#define PRINTK_BUF_SIZE 1024

/*
 * Skip a Linux log-level prefix of the form "\001" <digit>.
 * Returns a pointer past the prefix, or the original pointer if none found.
 */
static const char *strip_loglevel(const char *fmt)
{
    if (fmt[0] == '\001' && fmt[1] != '\0')
        return fmt + 2;
    return fmt;
}

int printk(const char *fmt, ...)
{
    char buf[PRINTK_BUF_SIZE];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), strip_loglevel(fmt), ap);
    va_end(ap);

    klog_puts(buf);
    return 0; /* Linux printk returns chars printed; we return 0 for simplicity */
}
