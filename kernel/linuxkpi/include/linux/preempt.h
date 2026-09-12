#ifndef __AVORY_LINUXKPI_PREEMPT_H
#define __AVORY_LINUXKPI_PREEMPT_H

/* Native-backed Linux <linux/preempt.h> overlay.
 *
 * The preemption counter is per-CPU (linuxkpi/src/preempt.c) and is honored
 * by the native scheduler: a tick or a reschedule IPI will not switch away
 * from a thread whose counter is non-zero (weak linuxkpi_preempt_allowed()
 * hook).  That lets the plain spin_lock()/spin_unlock() pair disable
 * preemption instead of masking interrupts, as Linux does.
 *
 * Hardirq/softirq nesting is tracked per-thread by the native ISR hooks
 * (linuxkpi_irq_enter/exit from src/cpu/isr.c), which gives in_interrupt()
 * and in_softirq().  Per-thread, not per-CPU: the scheduler can switch to
 * another thread from inside a handler, and that thread must not observe the
 * suspended handler's context.  There are no softirqs yet, so in_softirq() is
 * always false. */

#include <linux/compiler.h>
#include <linux/types.h>

unsigned int __kpi_preempt_count(void);
void __kpi_preempt_add(int val);
void __kpi_preempt_sub(int val);
void __kpi_preempt_disable(void);
void __kpi_preempt_enable(void);
void __kpi_preempt_enable_resched(void);

bool __kpi_in_interrupt(void);
bool __kpi_in_softirq(void);

#define preempt_count() (__kpi_preempt_count())
#define preempt_disable() __kpi_preempt_disable()
#define preempt_enable() __kpi_preempt_enable()
#define preempt_enable_resched() __kpi_preempt_enable_resched()
#define preempt_disable_notrace() preempt_disable()
#define preempt_enable_notrace() preempt_enable()
#define preempt_enable_no_resched() preempt_enable()

static inline void preempt_count_add(int val) { __kpi_preempt_add(val); }
static inline void preempt_count_sub(int val) { __kpi_preempt_sub(val); }

static inline bool in_atomic(void) {
  return preempt_count() != 0 || __kpi_in_interrupt();
}
static inline bool in_interrupt(void) { return __kpi_in_interrupt(); }
static inline bool in_softirq(void) { return __kpi_in_softirq(); }
static inline bool in_serving_softirq(void) { return __kpi_in_softirq(); }

/* might_sleep()/might_sleep_if() live in <linux/kernel.h> upstream (they call
 * might_resched()); do not redefine them here. */
#define might_resched() do { } while (0)
static inline void cond_resched(void) { }

static inline void preempt_fold_need_resched(void) { }

/* No page migration exists, so migration disable is a no-op (stock io-mapping
 * inlines pair migrate_disable()/migrate_enable()). */
static inline void migrate_disable(void) { }
static inline void migrate_enable(void) { }

#endif /* __AVORY_LINUXKPI_PREEMPT_H */
