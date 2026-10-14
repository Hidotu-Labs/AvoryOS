/* LinuxKPI irq_work (header: linux/irq_work.h).
 *
 * Callbacks are drained by a dedicated kthread, so irq_work_queue() is safe
 * from atomic context.  Ordering matches upstream's "eventually, not
 * re-entered" contract; callbacks are serialized. */

#include <linux/irq_work.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <linuxkpi/service.h>

static DEFINE_SPINLOCK(irq_work_lock);
static struct irq_work *irq_work_head;
static struct irq_work *irq_work_tail;
/* Currently executing work, for irq_work_sync(). Stored separately from the
 * work object so the kthread never touches a callback that freed itself
 * (the test callbacks free immediately; in-kernel fence-array completion
 * defers via kfree_rcu, but the same rule keeps both safe). */
static struct irq_work *irq_work_current;
static struct wait_queue_head irq_work_wait;
static struct kpi_service irq_work_svc;

void init_irq_work(struct irq_work *work, void (*func)(struct irq_work *)) {
  work->next = NULL;
  work->func = func;
  __atomic_store_n(&work->queued, 0, __ATOMIC_RELEASE);
}

bool irq_work_queue(struct irq_work *work) {
  unsigned long flags;

  if (!work)
    return false;

  spin_lock_irqsave(&irq_work_lock, flags);
  if (__atomic_load_n(&work->queued, __ATOMIC_ACQUIRE)) {
    spin_unlock_irqrestore(&irq_work_lock, flags);
    return false;
  }
  __atomic_store_n(&work->queued, 1, __ATOMIC_RELEASE);
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
    void (*func)(struct irq_work *);
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
      /* Drop the pending claim before execution, as Linux irq_work does.
       * Fence-chain callbacks can requeue this same object while executing;
       * a requeue sets queued again and is picked up on a later pass.
       * Publish the running instance separately: the callback may free its
       * own object (test callbacks do immediately; fence-array completion
       * defers via kfree_rcu), so never touch *work after func() returns. */
      __atomic_store_n(&work->queued, 0, __ATOMIC_RELEASE);
      __atomic_store_n(&irq_work_current, work, __ATOMIC_RELEASE);
    }
    func = work ? work->func : NULL;
    spin_unlock_irqrestore(&irq_work_lock, flags);

    if (func)
      func(work);
    if (work) {
      spin_lock_irqsave(&irq_work_lock, flags);
      /* Clears only the side record, never the (possibly freed) object. */
      __atomic_store_n(&irq_work_current, NULL, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&irq_work_lock, flags);
    }
  }
}

void irq_work_sync(struct irq_work *work) {
  unsigned long flags;
  bool busy;
  if (!work)
    return;
  /* Queue -> running and callback requeue -> completion must be observed
   * as one snapshot. Separate loads can miss a requeue made by a callback
   * between the queued load and the current load. The caller must keep the
   * object alive and exclude new external producers before teardown. */
  do {
    spin_lock_irqsave(&irq_work_lock, flags);
    busy = work->queued || irq_work_current == work;
    spin_unlock_irqrestore(&irq_work_lock, flags);
    if (busy)
      msleep(1);
  } while (busy);
}

void linuxkpi_irq_work_init(void) {
  init_waitqueue_head(&irq_work_wait);
  kpi_service_init(&irq_work_svc, "irq_work", irq_work_kthread, NULL);
}
