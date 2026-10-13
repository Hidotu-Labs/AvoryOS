/* LinuxKPI completions.  See linux/completion.h for the contract. */

#include <linux/completion.h>

#define UINT_MAX_DONE (~0U)

void complete(struct completion *x) {
  unsigned long flags;

  spin_lock_irqsave(&x->wait.lock, flags);
  if (x->done != UINT_MAX_DONE)
    x->done++;
  /* A consumer may release the object as soon as its wait returns. Keep
   * token publication and the last access to the queue under one lock. */
  __kpi_wake_up_locked(&x->wait, 1, 0);
  spin_unlock_irqrestore(&x->wait.lock, flags);
}

void complete_all(struct completion *x) {
  unsigned long flags;

  spin_lock_irqsave(&x->wait.lock, flags);
  x->done = UINT_MAX_DONE;
  __kpi_wake_up_locked(&x->wait, 0, 0);
  spin_unlock_irqrestore(&x->wait.lock, flags);
}

/* Observe and consume the token under the same lock. An unlocked ready
 * check followed by a later decrement permits two waiters to consume one
 * token and turn zero into UINT_MAX (a false permanent completion).
 * Queue insertion and wait-state publication also share the producer lock;
 * only the native scheduler wait runs with that lock/IRQ state released. */
static long completion_wait_common(struct completion *x, long timeout,
                                   int state) {
  struct wait_queue_entry wait;
  unsigned long flags;
  bool waited = false;

  init_wait_entry(&wait, WQ_FLAG_EXCLUSIVE);
  spin_lock_irqsave(&x->wait.lock, flags);
  while (!x->done) {
    if (___wait_is_interruptible(state) && signal_pending(current)) {
      timeout = -ERESTARTSYS;
      break;
    }
    if (!timeout)
      break;
    if (list_empty(&wait.entry))
      list_add_tail(&wait.entry, &x->wait.head);
    set_current_state(state);
    waited = true;
    spin_unlock_irqrestore(&x->wait.lock, flags);
    timeout = schedule_timeout(timeout);
    spin_lock_irqsave(&x->wait.lock, flags);
  }

  list_del_init(&wait.entry);
  if (waited)
    __set_current_state(TASK_RUNNING);
  if (x->done) {
    if (x->done != UINT_MAX_DONE)
      x->done--;
    if (!timeout)
      timeout = 1;
  }
  spin_unlock_irqrestore(&x->wait.lock, flags);
  return timeout;
}

void wait_for_completion(struct completion *x) {
  (void)completion_wait_common(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE);
}

long wait_for_completion_interruptible(struct completion *x) {
  long ret = completion_wait_common(x, MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE);
  return ret < 0 ? ret : 0;
}

long wait_for_completion_killable(struct completion *x) {
  long ret = completion_wait_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);
  return ret < 0 ? ret : 0;
}

unsigned long wait_for_completion_timeout(struct completion *x,
                                          unsigned long timeout) {
  return (unsigned long)completion_wait_common(x, (long)timeout,
                                               TASK_UNINTERRUPTIBLE);
}

long wait_for_completion_interruptible_timeout(struct completion *x,
                                               unsigned long timeout) {
  return completion_wait_common(x, (long)timeout, TASK_INTERRUPTIBLE);
}

bool try_wait_for_completion(struct completion *x) {
  unsigned long flags;
  bool ret = false;

  spin_lock_irqsave(&x->wait.lock, flags);
  if (x->done != 0) {
    if (x->done != UINT_MAX_DONE)
      x->done--;
    ret = true;
  }
  spin_unlock_irqrestore(&x->wait.lock, flags);

  return ret;
}

bool completion_done(struct completion *x) {
  unsigned long flags;
  bool ret;

  spin_lock_irqsave(&x->wait.lock, flags);
  ret = x->done != 0;
  spin_unlock_irqrestore(&x->wait.lock, flags);
  return ret;
}
