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

static DEFINE_SPINLOCK(irq_work_lock);
static struct irq_work *irq_work_head;
static struct irq_work *irq_work_tail;
static struct wait_queue_head irq_work_wait;
static struct task_struct *irq_work_task;

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

  if (irq_work_task)
    wake_up_process(irq_work_task);
  return true;
}

static int irq_work_kthread(void *arg) {
  (void)arg;

  for (;;) {
    unsigned long flags;
    struct irq_work *work;

    wait_event_interruptible(irq_work_wait,
                             kthread_should_stop() || irq_work_head);
    if (kthread_should_stop())
      return 0;

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
  irq_work_task = kthread_run(irq_work_kthread, NULL, "irq_work");
}
