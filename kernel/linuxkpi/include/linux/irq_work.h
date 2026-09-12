#ifndef __AVORY_LINUXKPI_IRQ_WORK_H
#define __AVORY_LINUXKPI_IRQ_WORK_H

/* AvoryOS overlay for <linux/irq_work.h>.
 *
 * Upstream runs irq_work callbacks from a hardirq/softirq "lazy" context.
 * Here they run on a dedicated kthread, which is safe from any caller
 * context and matches the "eventually, outside locks" contract.  dma-fence
 * array/chain completion notifications are the only users. */

#include <linux/types.h>

struct irq_work {
  struct irq_work *next;
  void (*func)(struct irq_work *);
  int queued;
};

void init_irq_work(struct irq_work *work, void (*func)(struct irq_work *));
bool irq_work_queue(struct irq_work *work);
void irq_work_sync(struct irq_work *work);

#endif /* __AVORY_LINUXKPI_IRQ_WORK_H */
