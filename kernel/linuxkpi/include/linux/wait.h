#ifndef __AVORY_LINUXKPI_WAIT_H
#define __AVORY_LINUXKPI_WAIT_H

/* Linux <linux/wait.h> overlay: waitqueues and wait_event* families.
 *
 * Implemented over the native scheduler through linuxkpi/native_sched.h.
 * Waiter entries are made non-intrusive (a list_head in the entry) and
 * `private` holds the native thread handle to wake.  Implementations:
 * linuxkpi/src/wait.c. */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct wait_queue_entry;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry,
                                 unsigned mode, int flags, void *key);

struct wait_queue_entry {
  unsigned int flags;
  void *private; /* native thread handle */
  wait_queue_func_t func;
  struct list_head entry;
};
typedef struct wait_queue_entry wait_queue_entry_t;

struct wait_queue_head {
  spinlock_t lock;
  struct list_head head;
  /* Avory: native poll wait queue bridged to the scheduler.  A device's poll
   * callback calls poll_wait() with this head; the KPI file bridge records
   * the native queue here so wake_up() also wakes poll(2)/select(2) waiters
   * registered by the native sys_poll(). */
  void *kpi_poll_wq;
};
typedef struct wait_queue_head wait_queue_head_t;

#define WQ_FLAG_EXCLUSIVE 0x01
#define WQ_FLAG_WOKEN 0x02

#define ___WAITQUEUE_INITIALIZER(name, tsk)                                   \
  {0, (void *)(tsk), autoremove_wake_function, LIST_HEAD_INIT((name).entry)}
#define DECLARE_WAITQUEUE(name, tsk)                                          \
  struct wait_queue_entry name = ___WAITQUEUE_INITIALIZER(name, tsk)

#define __WAIT_QUEUE_HEAD_INITIALIZER(name)                                   \
  {{0, 0}, LIST_HEAD_INIT((name).head), 0}
#define DECLARE_WAIT_QUEUE_HEAD(name)                                         \
  wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

#define init_waitqueue_head(wq)                                               \
  do {                                                                        \
    spin_lock_init(&(wq)->lock);                                              \
    INIT_LIST_HEAD(&(wq)->head);                                              \
    (wq)->kpi_poll_wq = NULL;                                                 \
  } while (0)

int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
                          int flags, void *key);
int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
                             int flags, void *key);
void init_wait_entry(struct wait_queue_entry *wq_entry, int flags);
void add_wait_queue(wait_queue_head_t *q, struct wait_queue_entry *wq_entry);
void remove_wait_queue(wait_queue_head_t *q,
                       struct wait_queue_entry *wq_entry);
void prepare_to_wait(wait_queue_head_t *q, struct wait_queue_entry *wq_entry,
                     int state);
int prepare_to_wait_event(wait_queue_head_t *q,
                          struct wait_queue_entry *wq_entry, int state);
void finish_wait(wait_queue_head_t *q, struct wait_queue_entry *wq_entry);

int __kpi_wake_up(wait_queue_head_t *q, unsigned int nr, int flags);
int __kpi_wake_up_locked(wait_queue_head_t *q, unsigned int nr, int flags);
int wake_up_state(struct task_struct *p, unsigned int state);

/* Wake poll(2)/select(2) waiters blocked on a native wait queue that the KPI
 * poll bridge attached to a wait_queue_head (see wait_queue_head::kpi_poll_wq).
 * `native_wq` is an opaque `wait_queue_t *`. */
void linuxkpi_wake_poll_queue(void *native_wq);
#define wake_up(q) __kpi_wake_up((q), 0, 0)
#define wake_up_nr(q, nr) __kpi_wake_up((q), (nr), 0)
#define wake_up_all(q) __kpi_wake_up((q), 0, 0)
#define wake_up_interruptible(q) __kpi_wake_up((q), 0, 0)
#define wake_up_interruptible_nr(q, nr) __kpi_wake_up((q), (nr), 0)
#define wake_up_interruptible_all(q) __kpi_wake_up((q), 0, 0)
#define wake_up_interruptible_sync(q) __kpi_wake_up((q), 0, 0)
#define wake_up_locked(q) __kpi_wake_up_locked((q), 0, 0)
#define wake_up_all_locked(q) __kpi_wake_up_locked((q), 0, 0)
#define wake_up_locked_poll(q, mode) __kpi_wake_up_locked((q), 0, 0)
#define wake_up_interruptible_sync_poll_locked(q, mode)                        \
  __kpi_wake_up_locked((q), 0, 0)
#define wake_up_interruptible_poll(q, mode) __kpi_wake_up((q), 0, 0)
#define wake_up_poll(q, mode) __kpi_wake_up((q), 0, 0)
#define wake_up_pollfree(q) __kpi_wake_up((q), 0, 0)
#define __wake_up_pollfree(q) __kpi_wake_up((q), 0, 0)
#define __wake_up(q, mode, nr, key) __kpi_wake_up((q), (nr), 0)
#define __wake_up_locked(q, mode, nr) __kpi_wake_up_locked((q), (nr), 0)
#define __wake_up_locked_key(q, mode, key) __kpi_wake_up_locked((q), 0, 0)
#define __wake_up_locked_sync_key(q, mode, key) __kpi_wake_up_locked((q), 0, 0)
#define __wake_up_locked_key_bookmark(q, mode, key, bm)                        \
  __kpi_wake_up_locked((q), 0, 0)

static inline void __add_wait_queue(wait_queue_head_t *wq_head,
                                    struct wait_queue_entry *wq_entry) {
  list_add(&wq_entry->entry, &wq_head->head);
}

static inline void __add_wait_queue_entry_tail(wait_queue_head_t *wq_head,
                                               struct wait_queue_entry *wq_entry) {
  list_add_tail(&wq_entry->entry, &wq_head->head);
}

/* ── wait_event family (ported macro shapes from upstream) ──────────────── */

#define ___wait_is_interruptible(state)                                       \
  (!__builtin_constant_p(state) ||                                            \
   ((state) & (TASK_INTERRUPTIBLE | TASK_WAKEKILL)))

/* Exits the ___wait_event loop on timeout as well as on the condition; skips
 * the condition evaluation when the timeout already expired. */
#define ___wait_cond_timeout(condition)                                       \
  ({                                                                          \
    bool __cond = (condition);                                                \
    if (__cond && !__ret)                                                     \
      __ret = 1;                                                              \
    __cond || !__ret;                                                         \
  })

#define ___wait_event(wq_head, condition, state, exclusive, ret, cmd)         \
  ({                                                                          \
    __label__ __out;                                                          \
    struct wait_queue_entry __wq_entry;                                       \
    long __ret = (long)(ret);                                                 \
    init_wait_entry(&__wq_entry, exclusive ? WQ_FLAG_EXCLUSIVE : 0);          \
    for (;;) {                                                                \
      long __int = prepare_to_wait_event(&(wq_head), &__wq_entry, state);     \
      if (condition)                                                          \
        break;                                                                \
      if (___wait_is_interruptible(state) && __int) {                         \
        __ret = (long)__int;                                                  \
        goto __out;                                                           \
      }                                                                       \
      cmd;                                                                    \
    }                                                                         \
  __out:                                                                      \
    finish_wait(&(wq_head), &__wq_entry);                                     \
    __ret;                                                                    \
  })

#define __wait_event(wq_head, condition)                                      \
  ___wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, 0, 0, schedule())

#define wait_event(wq_head, condition)                                        \
  do {                                                                        \
    (void)__wait_event(wq_head, condition);                                   \
  } while (0)

#define wait_event_interruptible(wq_head, condition)                          \
  ___wait_event(wq_head, condition, TASK_INTERRUPTIBLE, 0, 0, schedule())

#define wait_event_killable(wq_head, condition)                               \
  ___wait_event(wq_head, condition, TASK_KILLABLE, 0, 0, schedule())

/* The _lock_irq variants drop the caller's lock around schedule() and
 * re-acquire it; AvoryOS notes the lock but does not reorder it because the
 * native wait path does not sleep with locks held (documented in
 * docs/linuxkpi-gaps.md).  Kept API-compatible. */
#define wait_event_lock_irq(wq_head, condition, lock)                          \
  do {                                                                        \
    (void)(lock);                                                             \
    wait_event(wq_head, condition);                                           \
  } while (0)
#define wait_event_interruptible_lock_irq(wq_head, condition, lock)            \
  do {                                                                        \
    (void)(lock);                                                             \
    wait_event_interruptible(wq_head, condition);                             \
  } while (0)

#define __wait_event_timeout(wq_head, condition, timeout)                     \
  ___wait_event(wq_head, ___wait_cond_timeout(condition),                     \
                TASK_UNINTERRUPTIBLE, 0, timeout,                             \
                __ret = schedule_timeout(__ret))

#define wait_event_timeout(wq_head, condition, timeout)                       \
  ({                                                                          \
    long __ret = (long)(timeout);                                             \
    if (!(condition))                                                         \
      __ret = __wait_event_timeout(wq_head, condition, timeout);              \
    __ret;                                                                    \
  })

#define __wait_event_interruptible_timeout(wq_head, condition, timeout)       \
  ___wait_event(wq_head, ___wait_cond_timeout(condition), TASK_INTERRUPTIBLE, \
                0, timeout, __ret = schedule_timeout(__ret))

#define wait_event_interruptible_timeout(wq_head, condition, timeout)         \
  ({                                                                          \
    long __ret = (long)(timeout);                                             \
    if (!(condition))                                                         \
      __ret = __wait_event_interruptible_timeout(wq_head, condition, timeout);\
    __ret;                                                                    \
  })

#define wait_event_cmd(wq_head, condition, cmd1, cmd2)                        \
  do {                                                                        \
    if (!(condition)) {                                                       \
      DEFINE_WAIT(__wq_entry);                                                \
      prepare_to_wait(&(wq_head), &__wq_entry, TASK_UNINTERRUPTIBLE);          \
      for (;;) {                                                              \
        cmd1;                                                                 \
        if (condition)                                                        \
          break;                                                              \
        schedule();                                                           \
        cmd2;                                                                 \
      }                                                                       \
      finish_wait(&(wq_head), &__wq_entry);                                   \
    }                                                                         \
  } while (0)

#define DEFINE_WAIT_FUNC(name, function)                                      \
  struct wait_queue_entry name = {0, linuxkpi_current_thread(), function,     \
                                  LIST_HEAD_INIT((name).entry)}
#define DEFINE_WAIT(name) DEFINE_WAIT_FUNC(name, autoremove_wake_function)

/* ── locked wait_event family ─────────────────────────────────────────────
 *
 * The caller holds @wq.lock; the helper drops it around schedule() and
 * re-acquires it before returning (upstream include/linux/wait.h).  The
 * implementation lives in linuxkpi/src/wait.c and manipulates the waitqueue
 * list directly under the caller's lock. */
extern int do_wait_intr(wait_queue_head_t *, wait_queue_entry_t *);
extern int do_wait_intr_irq(wait_queue_head_t *, wait_queue_entry_t *);

#define __remove_wait_queue(wq_head, wq_entry) list_del_init(&(wq_entry)->entry)

#define __wait_event_interruptible_locked(wq, condition, exclusive, fn)       \
  ({                                                                          \
    int __ret;                                                                \
    DEFINE_WAIT(__wait);                                                      \
    if (exclusive)                                                            \
      __wait.flags |= WQ_FLAG_EXCLUSIVE;                                      \
    do {                                                                      \
      __ret = fn(&(wq), &__wait);                                             \
      if (__ret)                                                              \
        break;                                                                \
    } while (!(condition));                                                   \
    __remove_wait_queue(&(wq), &__wait);                                      \
    __set_current_state(TASK_RUNNING);                                        \
    __ret;                                                                    \
  })

#define wait_event_interruptible_locked(wq, condition)                        \
  ((condition) ? 0                                                            \
               : __wait_event_interruptible_locked(wq, condition, 0,          \
                                                   do_wait_intr))

#define wait_event_interruptible_locked_irq(wq, condition)                    \
  ((condition) ? 0                                                            \
               : __wait_event_interruptible_locked(wq, condition, 0,          \
                                                   do_wait_intr_irq))

#define wait_event_interruptible_exclusive_locked(wq, condition)              \
  ((condition) ? 0                                                            \
               : __wait_event_interruptible_locked(wq, condition, 1,          \
                                                   do_wait_intr))

#define wait_event_interruptible_exclusive_locked_irq(wq, condition)          \
  ((condition) ? 0                                                            \
               : __wait_event_interruptible_locked(wq, condition, 1,          \
                                                   do_wait_intr_irq))

#endif /* __AVORY_LINUXKPI_WAIT_H */
