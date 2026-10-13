#ifndef __AVORY_LINUXKPI_LOCAL_LOCK_H
#define __AVORY_LINUXKPI_LOCAL_LOCK_H

/* Non-RT Linux local locks pin CPU-local data against preemption/IRQs.
 * In particular, idr_preload holds this pin until idr_preload_end. */

#include <linux/types.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>

typedef struct {
  int unused;
} local_lock_t;

#define INIT_LOCAL_LOCK(lock) {0}

static inline void local_lock_init(local_lock_t *l) { (void)l; }
static inline void local_lock(local_lock_t *l) { (void)l; preempt_disable(); }
static inline void local_unlock(local_lock_t *l) { (void)l; preempt_enable(); }
static inline void local_lock_irq(local_lock_t *l) { (void)l; local_irq_disable(); }
static inline void local_unlock_irq(local_lock_t *l) { (void)l; local_irq_enable(); }
#define local_lock_irqsave(l, flags) do { (void)(l); local_irq_save(flags); } while (0)
#define local_unlock_irqrestore(l, flags) do { (void)(l); local_irq_restore(flags); } while (0)

#endif /* __AVORY_LINUXKPI_LOCAL_LOCK_H */
