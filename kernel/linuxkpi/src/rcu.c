/* LinuxKPI RCU.  See linux/rcupdate.h.
 *
 * Read side is a global atomic reader count (preempt-disable plus counter).
 * A grace period is observed by waiting for that count to reach zero; since
 * the read side may be preempted, the wait sleeps and rechecks.  Callbacks
 * are drained by a dedicated kthread after a grace period.  Correct, but the
 * global counter and polling GP are candidates for optimization later. */

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <linuxkpi/native_sched.h>

static volatile int rcu_readers;
static volatile int rcu_busy;
static struct rcu_head *rcu_callbacks;
static DEFINE_SPINLOCK(rcu_lock);
static struct wait_queue_head rcu_wait;
static struct wait_queue_head rcu_barrier_wait;
static struct task_struct *rcu_task;

void __kpi_rcu_read_lock(void) {
  preempt_disable();
  __atomic_add_fetch(&rcu_readers, 1, __ATOMIC_ACQ_REL);
}

void __kpi_rcu_read_unlock(void) {
  __atomic_sub_fetch(&rcu_readers, 1, __ATOMIC_ACQ_REL);
  preempt_enable();
}

static void rcu_wait_grace_period(void) {
  while (__atomic_load_n(&rcu_readers, __ATOMIC_ACQUIRE) != 0)
    msleep(1);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void synchronize_rcu(void) { rcu_wait_grace_period(); }

void synchronize_rcu_expedited(void) { rcu_wait_grace_period(); }

void call_rcu(struct rcu_head *head, rcu_callback_t func) {
  unsigned long flags;

  head->func = func;

  spin_lock_irqsave(&rcu_lock, flags);
  head->next = rcu_callbacks;
  rcu_callbacks = head;
  spin_unlock_irqrestore(&rcu_lock, flags);

  __kpi_wake_up(&rcu_wait, 1, 0);
}

void rcu_barrier(void) {
  while (__atomic_load_n(&rcu_busy, __ATOMIC_ACQUIRE) ||
         __atomic_load_n(&rcu_readers, __ATOMIC_ACQUIRE) != 0 ||
         rcu_callbacks != NULL) {
    /* Let any in-flight callback batch finish. */
    synchronize_rcu();
    if (__atomic_load_n(&rcu_busy, __ATOMIC_ACQUIRE))
      wait_event(rcu_barrier_wait,
                 !__atomic_load_n(&rcu_busy, __ATOMIC_ACQUIRE));
    if (rcu_callbacks == NULL)
      break;
  }
}

static int rcu_kthread(void *arg) {
  (void)arg;

  for (;;) {
    wait_event(rcu_wait, kthread_should_stop() || rcu_callbacks != NULL);

    if (kthread_should_stop())
      return 0;

    if (!rcu_callbacks)
      continue;

    rcu_wait_grace_period();

    unsigned long flags;
    spin_lock_irqsave(&rcu_lock, flags);
    struct rcu_head *head = rcu_callbacks;
    rcu_callbacks = NULL;
    if (head)
      __atomic_store_n(&rcu_busy, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&rcu_lock, flags);

    if (!head)
      continue;

    while (head) {
      struct rcu_head *next = head->next;
      head->func(head);
      head = next;
    }

    __atomic_store_n(&rcu_busy, 0, __ATOMIC_RELEASE);
    __kpi_wake_up(&rcu_barrier_wait, 0, 0);
  }
}

void linuxkpi_rcu_init(void) {
  init_waitqueue_head(&rcu_wait);
  init_waitqueue_head(&rcu_barrier_wait);
  rcu_task = kthread_run(rcu_kthread, NULL, "rcu_kpi");
}

/* kfree_rcu support: remember the object behind an embedded rcu_head. */
struct kfree_rcu_ent {
  struct rcu_head head;
  void *obj;
};

static void kfree_rcu_cb(struct rcu_head *head) {
  struct kfree_rcu_ent *ent =
      container_of(head, struct kfree_rcu_ent, head);

  kfree(ent->obj);
  kfree(ent);
}

void __kvfree_call_rcu(struct rcu_head *head, __SIZE_TYPE__ offset) {
  struct kfree_rcu_ent *ent = kmalloc(sizeof(*ent), GFP_KERNEL);

  if (!ent)
    return; /* leak rather than corrupt; never expected */
  ent->obj = (void *)((char *)head - offset);
  call_rcu(&ent->head, kfree_rcu_cb);
}
