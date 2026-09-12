#ifndef __AVORY_LINUXKPI_HARDIRQ_H
#define __AVORY_LINUXKPI_HARDIRQ_H

/* AvoryOS overlay for <linux/hardirq.h>.
 *
 * Upstream hardirq.h drags in vtime/context-tracking (which needs a full
 * task_struct); the context predicates themselves already live in the
 * preempt.h overlay and are backed by the native ISR hooks.  irq_enter()/
 * irq_exit() call those same hooks so imported code that brackets a handler
 * manually stays consistent with in_interrupt(). */

#include <linux/preempt.h>
#include <linuxkpi/native_sched.h>

static inline bool in_hardirq(void) { return __kpi_in_interrupt(); }

static inline void irq_enter(void) { linuxkpi_irq_enter(); }
static inline void irq_exit(void) { linuxkpi_irq_exit(); }

#endif /* __AVORY_LINUXKPI_HARDIRQ_H */
