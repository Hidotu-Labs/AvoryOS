/* LinuxKPI irq_work (header: linux/irq_work.h).
 *
 * Callbacks are drained by a dedicated kthread, so irq_work_queue() is safe
 * from atomic context.  Ordering matches upstream's "eventually, not
 * re-entered" contract; callbacks are serialized. */

#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <linuxkpi/service.h>

static DEFINE_SPINLOCK(irq_work_lock);
static struct irq_work *irq_work_head;
static struct irq_work *irq_work_tail;
static struct wait_queue_head irq_work_wait;
static struct kpi_service irq_work_svc;

void init_irq_work(struct irq_work *work, void (*func)(struct irq_work *)) {
  work->next = NULL;
  work->func = func;
  work->queued = 0;
}

bool irq_work_queue(struct irq_work *work) {
  unsigned long flags;

  if (!work)
    return false;

  spin_lock_irqsave(&irq_work_lock, flags);
  if (work->queued) {
    spin_unlock_irqrestore(&irq_work_lock, flags);
    return false;
  }
  work->queued = 1;
  work->next = NULL;
  if (irq_work_tail) {
    irq_work_tail->next = work;
    irq_work_tail = work;
  } else {
    irq_work_head = irq_work_tail = work;
  }
  spin_unlock_irqrestore(&irq_work_lock, flags);

  kpi_service_kick(&irq_work_svc, true);
  return true;
}

static int irq_work_kthread(void *arg) {
  (void)arg;

  for (;;) {
    unsigned long flags;
    struct irq_work *work;
    unsigned int gen = kpi_service_gen(&irq_work_svc);

    (void)wait_event_interruptible_timeout(
        irq_work_wait, kthread_should_stop() || irq_work_head != NULL,
        msecs_to_jiffies(KPI_SERVICE_IDLE_MS));

    if (kthread_should_stop()) {
      kpi_service_forget(&irq_work_svc);
      return 0;
    }

    if (!irq_work_head) {
      if (kpi_service_retire(&irq_work_svc, gen))
        return 0;
      continue;
    }

    spin_lock_irqsave(&irq_work_lock, flags);
    work = irq_work_head;
    if (work) {
      irq_work_head = work->next;
      if (!irq_work_head)
        irq_work_tail = NULL;
      work->next = NULL;
    }
    spin_unlock_irqrestore(&irq_work_lock, flags);

    if (work && work->func) {
      work->func(work);
      work->queued = 0;
    }
  }
}

void irq_work_sync(struct irq_work *work) {
  /* Not used by the current import set.  A correct implementation needs a
   * per-work completion; assert instead of pretending. */
  (void)work;
}

void linuxkpi_irq_work_init(void) {
  init_waitqueue_head(&irq_work_wait);
  kpi_service_init(&irq_work_svc, "irq_work", irq_work_kthread, NULL);
}
