/* LinuxKPI waitqueues.  See linux/wait.h for the contract. */

#include <linux/wait.h>

#include <linuxkpi/native_sched.h>

void init_wait_entry(struct wait_queue_entry *wq_entry, int flags) {
  wq_entry->flags = (unsigned int)flags;
  wq_entry->private = linuxkpi_current_thread();
  wq_entry->func = autoremove_wake_function;
  INIT_LIST_HEAD(&wq_entry->entry);
}

int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
                          int flags, void *key) {
  (void)mode;
  (void)flags;
  (void)key;
  linuxkpi_wake_thread(wq_entry->private);
  return 1;
}

int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
                             int flags, void *key) {
  int ret = default_wake_function(wq_entry, mode, flags, key);

  if (ret)
    list_del_init(&wq_entry->entry);
  return ret;
}

void add_wait_queue(wait_queue_head_t *q, struct wait_queue_entry *wq_entry) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  if (list_empty(&wq_entry->entry))
    list_add(&wq_entry->entry, &q->head);
  spin_unlock_irqrestore(&q->lock, flags);
}

void remove_wait_queue(wait_queue_head_t *q,
                       struct wait_queue_entry *wq_entry) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  list_del_init(&wq_entry->entry);
  spin_unlock_irqrestore(&q->lock, flags);
}

void prepare_to_wait(wait_queue_head_t *q, struct wait_queue_entry *wq_entry,
                     int state) {
  unsigned long flags;

  (void)state;
  spin_lock_irqsave(&q->lock, flags);
  if (list_empty(&wq_entry->entry))
    list_add(&wq_entry->entry, &q->head);
  spin_unlock_irqrestore(&q->lock, flags);
}

int prepare_to_wait_event(wait_queue_head_t *q,
                          struct wait_queue_entry *wq_entry, int state) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  if (list_empty(&wq_entry->entry))
    list_add(&wq_entry->entry, &q->head);
  spin_unlock_irqrestore(&q->lock, flags);

  if (___wait_is_interruptible(state) && signal_pending(current))
    return -ERESTARTSYS;
  return 0;
}

void finish_wait(wait_queue_head_t *q, struct wait_queue_entry *wq_entry) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  list_del_init(&wq_entry->entry);
  spin_unlock_irqrestore(&q->lock, flags);
}

/* Locked wait_event support: the caller holds q->lock.  Add the entry under
 * that lock, then drop it around schedule() and re-acquire it (upstream
 * do_wait_intr/do_wait_intr_irq in kernel/sched/wait.c).  A wakeup removes
 * the entry via autoremove_wake_function; the macro's list_del_init() is
 * therefore idempotent. */
int do_wait_intr(wait_queue_head_t *q, wait_queue_entry_t *wait) {
  if (list_empty(&wait->entry))
    list_add_tail(&wait->entry, &q->head);

  set_current_state(TASK_INTERRUPTIBLE);
  if (signal_pending(current))
    return -ERESTARTSYS;

  spin_unlock(&q->lock);
  schedule();
  spin_lock(&q->lock);
  return 0;
}

int do_wait_intr_irq(wait_queue_head_t *q, wait_queue_entry_t *wait) {
  if (list_empty(&wait->entry))
    list_add_tail(&wait->entry, &q->head);

  set_current_state(TASK_INTERRUPTIBLE);
  if (signal_pending(current))
    return -ERESTARTSYS;

  spin_unlock_irq(&q->lock);
  schedule();
  spin_lock_irq(&q->lock);
  return 0;
}

static int __kpi_wake_up_common_locked(wait_queue_head_t *q, unsigned int nr,
                                       int flags) {
  int woken = 0;

  while (!list_empty(&q->head)) {
    struct wait_queue_entry *wq_entry =
        list_first_entry(&q->head, struct wait_queue_entry, entry);
    list_del_init(&wq_entry->entry);
    wq_entry->func(wq_entry, 0, flags, NULL);
    woken++;
    if (nr && (unsigned int)woken >= nr)
      break;
  }

  return woken;
}

int __kpi_wake_up_locked(wait_queue_head_t *q, unsigned int nr, int flags) {
  void *poll_wq = q->kpi_poll_wq;
  int woken = __kpi_wake_up_common_locked(q, nr, flags);

  if (poll_wq)
    linuxkpi_wake_poll_queue(poll_wq);

  return woken;
}

int __kpi_wake_up(wait_queue_head_t *q, unsigned int nr, int flags) {
  unsigned long irqflags;
  void *poll_wq;
  int woken;

  /* Wake functions must run under the queue lock.  A waiter that times out or
   * is interrupted can otherwise finish_wait() and return from the enclosing
   * wait_event() - reusing its stack-allocated entry - while this loop is
   * still calling into it.  Entries are unlinked before the call so the walk
   * stays valid even for wake functions that leave the entry queued. */
  spin_lock_irqsave(&q->lock, irqflags);
  poll_wq = q->kpi_poll_wq;
  woken = __kpi_wake_up_common_locked(q, nr, flags);
  spin_unlock_irqrestore(&q->lock, irqflags);

  /* Devices that used poll_wait() on this head have sys_poll() waiters parked
   * on a native wait queue; wake those too (outside the KPI queue lock). */
  if (poll_wq)
    linuxkpi_wake_poll_queue(poll_wq);

  return woken;
}

/* Wake a specific task regardless of its wait state.  The native scheduler
 * only has runnable/sleeping, so `state` is informational. */
int wake_up_state(struct task_struct *p, unsigned int state) {
  void *thread = task_struct_to_thread(p);

  (void)state;
  if (!thread)
    return 0;
  linuxkpi_wake_thread(thread);
  return 1;
}
