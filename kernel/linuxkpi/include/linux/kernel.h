#ifndef __AVORY_LINUXKPI_KERNEL_H
#define __AVORY_LINUXKPI_KERNEL_H

/* Additive overlay for <linux/kernel.h> (Phase 5 C6).
 *
 * Stock might_sleep() is a no-op because CONFIG_DEBUG_ATOMIC_SLEEP is unset.
 * Route it through the LinuxKPI context check instead: sleeping with
 * preemption disabled, IRQs masked or inside an interrupt warns once per
 * caller site (WARN, never BUG).  The implementation lives in
 * linuxkpi/src/preempt.c; the divergence is recorded in
 * docs/linuxkpi-gaps.md (P5 C6). */

#include_next <linux/kernel.h>

void __kpi_might_sleep(const char *file, int line);
int kpi_might_sleep_warnings(void);

#undef might_sleep
#define might_sleep() __kpi_might_sleep(__FILE__, __LINE__)

#endif /* __AVORY_LINUXKPI_KERNEL_H */
