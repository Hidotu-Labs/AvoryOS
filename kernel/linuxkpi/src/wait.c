/* LinuxKPI waitqueues.  See linux/wait.h for the contract. */

#include <linux/wait.h>
#include <linux/slab.h>
#include <linuxkpi/native_vfs.h>

#include <linuxkpi/native_sched.h>

/* Poll registrations have two owners: a Linux wait head and a polled VFS
 * node. One IRQ-safe lock protects both lists and serializes notification
 * against teardown. It never nests a Linux wait-head lock inside it. Native
 * notification only queues epoll candidates; it must not call driver poll. */
struct kpi_poll_link {
  struct kpi_poll_link *queue_next, *file_next;
  wait_queue_head_t *queue;
  void *node, *native_wq;
};
static DEFINE_SPINLOCK(kpi_poll_lock);

int linuxkpi_poll_register(struct kpi_poll_link **file_links, wait_queue_head_t *q,
                           void *node, void *native_wq) {
  struct kpi_poll_link *link, *fresh = NULL;
  unsigned long flags;
  for (;;) {
    spin_lock_irqsave(&kpi_poll_lock, flags);
    for (link = *file_links; link; link = link->file_next) {
      if (link->queue == q && link->node == node) {
        spin_unlock_irqrestore(&kpi_poll_lock, flags);
        kfree(fresh);
        return 0;
      }
    }
    if (fresh)
      break;
    spin_unlock_irqrestore(&kpi_poll_lock, flags);
    fresh = kzalloc(sizeof(*fresh), GFP_ATOMIC);
    if (!fresh)
      return -ENOMEM;
  }
  fresh->queue = q;
  fresh->node = node;
  fresh->native_wq = native_wq;
  fresh->file_next = *file_links;
  *file_links = fresh;
  fresh->queue_next = q->kpi_poll_links;
  q->kpi_poll_links = fresh;
  spin_unlock_irqrestore(&kpi_poll_lock, flags);
  return 0;
}

void linuxkpi_poll_detach(struct kpi_poll_link **file_links, void *node) {
  unsigned long flags;
  spin_lock_irqsave(&kpi_poll_lock, flags);
  struct kpi_poll_link **cursor = file_links;
  while (*cursor) {
    struct kpi_poll_link *link = *cursor;
    if (node && link->node != node) {
      cursor = &link->file_next;
      continue;
    }
    *cursor = link->file_next;
    if (link->queue) {
      struct kpi_poll_link **queued =
          &link->queue->kpi_poll_links;
      while (*queued && *queued != link)
        queued = &(*queued)->queue_next;
      if (*queued)
        *queued = link->queue_next;
    }
    kfree(link);
  }
  spin_unlock_irqrestore(&kpi_poll_lock, flags);
}

void linuxkpi_poll_free(wait_queue_head_t *q) {
  unsigned long flags;
  spin_lock_irqsave(&kpi_poll_lock, flags);
  struct kpi_poll_link *link = q->kpi_poll_links;
  q->kpi_poll_links = NULL;
  while (link) {
    struct kpi_poll_link *next = link->queue_next;
    /* The file still owns the allocation; close must no longer touch q. */
    link->queue = NULL;
    link->queue_next = NULL;
    link = next;
  }
  spin_unlock_irqrestore(&kpi_poll_lock, flags);
}

static void kpi_poll_notify(wait_queue_head_t *q) {
  unsigned long flags;
  spin_lock_irqsave(&kpi_poll_lock, flags);
  for (struct kpi_poll_link *link = q->kpi_poll_links; link;
       link = link->queue_next)
    asc_vfs_poll_notify(link->node, link->native_wq);
  spin_unlock_irqrestore(&kpi_poll_lock, flags);
}

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

  spin_lock_irqsave(&q->lock, flags);
  if (list_empty(&wq_entry->entry))
    list_add(&wq_entry->entry, &q->head);
  set_current_state(state);
  spin_unlock_irqrestore(&q->lock, flags);
}

int prepare_to_wait_event(wait_queue_head_t *q,
                          struct wait_queue_entry *wq_entry, int state) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  if (list_empty(&wq_entry->entry))
    list_add(&wq_entry->entry, &q->head);
  set_current_state(state);
  spin_unlock_irqrestore(&q->lock, flags);

  if (___wait_is_interruptible(state) && signal_pending(current))
    return -ERESTARTSYS;
  return 0;
}

void finish_wait(wait_queue_head_t *q, struct wait_queue_entry *wq_entry) {
  unsigned long flags;

  __set_current_state(TASK_RUNNING);

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

  struct wait_queue_entry *entry, *next;

  list_for_each_entry_safe(entry, next, &q->head, entry) {
    unsigned int entry_flags = entry->flags;
    int ret = entry->func(entry, 0, flags, NULL);
    if (ret < 0)
      break;
    if (!ret)
      continue;
    woken++;
    /* Wake callbacks own removal. Persistent/custom entries must remain
     * registered, and nonexclusive wakes do not consume the exclusive quota. */
    if ((entry_flags & WQ_FLAG_EXCLUSIVE) && nr && !--nr)
      break;
  }

  return woken;
}

int __kpi_wake_up_locked(wait_queue_head_t *q, unsigned int nr, int flags) {
  int woken = __kpi_wake_up_common_locked(q, nr, flags);
  kpi_poll_notify(q);
  return woken;
}

int __kpi_wake_up(wait_queue_head_t *q, unsigned int nr, int flags) {
  unsigned long irqflags;
  int woken;

  /* Keep callbacks serialized against finish_wait() and stack-entry reuse. */
  spin_lock_irqsave(&q->lock, irqflags);
  woken = __kpi_wake_up_common_locked(q, nr, flags);
  /* Match the locked API: the wait-head owner cannot destroy q until this
   * notification completes, and close cannot free subscribed nodes/queues. */
  kpi_poll_notify(q);
  spin_unlock_irqrestore(&q->lock, irqflags);
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
