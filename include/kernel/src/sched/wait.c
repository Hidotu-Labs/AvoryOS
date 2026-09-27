#include "wait.h"
#include "../lock/lockdiag.h"
#include "hal/hal.h"
#include "../mm/heap.h"
#include "sched.h"
#include <stddef.h>

void wait_queue_init(wait_queue_t *wq) {
  if (!wq)
    return;
  spinlock_init(&wq->lock);
  wq->head = NULL;
}

/* Wait-queue entry pointers must live in kernel space.  A user-space-looking
 * value in a list means the queue or an entry was reused after being freed
 * (destroy paths wake waiters and then free the queue), and chasing that
 * pointer would fault the kernel in the middle of process teardown.  Stop the
 * walk instead of dereferencing it. */
static inline bool wq_entry_ptr_sane(const void *p) {
  return (uint64_t)p >= 0xFFFF800000000000ULL;
}

void wait_queue_add(wait_queue_t *wq, wait_queue_entry_t *entry) {
  if (!wq || !entry)
    return;

  entry->wq = wq;
  struct thread *t = entry->thread;

  LOCKDIAG_WQ_LOCK(wq);
  entry->next = wq->head;
  wq->head = entry;
  LOCKDIAG_WQ_UNLOCK(wq);
  LOCKDIAG_STAT(wq_adds, 1);

  if (t) {
    spinlock_acquire(&t->wait_entries_lock);
    entry->thread_next = t->wait_entries_head;
    entry->thread_prev = NULL;
    if (t->wait_entries_head)
      t->wait_entries_head->thread_prev = entry;
    t->wait_entries_head = entry;
    spinlock_release(&t->wait_entries_lock);
  }
}

void wait_queue_remove(wait_queue_t *wq, wait_queue_entry_t *entry) {
  if (!entry)
    return;

  /* Trust entry->wq, not the caller's argument.  Wakers unlink entries and
   * clear entry->wq before the owning object can free the queue; the caller's
   * `wq` is frequently read after the wake, so it may already be dangling.
   * entry->wq == NULL means "not on any queue anymore": only the thread-list
   * link below is left to clear. */
  (void)wq;
  wait_queue_t *queue = entry->wq;

  if (queue) {
    LOCKDIAG_WQ_LOCK(queue);
    wait_queue_entry_t **pp = &queue->head;
    while (*pp) {
      wait_queue_entry_t *curr = *pp;
      if (!wq_entry_ptr_sane(curr))
        break; /* corrupt list: leave it alone rather than fault */
      if (curr == entry) {
        *pp = entry->next;
        break;
      }
      pp = &curr->next;
    }
    entry->next = NULL;
    entry->wq = NULL;
    LOCKDIAG_WQ_UNLOCK(queue);
  }

  struct thread *t = entry->thread;
  if (t) {
    spinlock_acquire(&t->wait_entries_lock);
    if (entry->thread_prev)
      entry->thread_prev->thread_next = entry->thread_next;
    else if (t->wait_entries_head == entry)
      t->wait_entries_head = entry->thread_next;

    if (entry->thread_next)
      entry->thread_next->thread_prev = entry->thread_prev;

    entry->thread_prev = NULL;
    entry->thread_next = NULL;
    spinlock_release(&t->wait_entries_lock);
  }
}

void wait_queue_cleanup_thread(struct thread *t) {
  if (!t)
    return;

  spinlock_acquire(&t->wait_entries_lock);
  wait_queue_entry_t *curr = t->wait_entries_head;
  t->wait_entries_head = NULL;
  spinlock_release(&t->wait_entries_lock);

  while (curr) {
    wait_queue_entry_t *next = curr->thread_next;
    curr->thread_prev = NULL;
    curr->thread_next = NULL;
    wait_queue_t *wq = curr->wq;
    if (wq) {
      LOCKDIAG_WQ_LOCK(wq);
      wait_queue_entry_t **pp = &wq->head;
      while (*pp) {
        wait_queue_entry_t *p = *pp;
        if (!wq_entry_ptr_sane(p))
          break; /* queue memory was reused after a free: stop walking it */
        if (p == curr) {
          *pp = curr->next;
          break;
        }
        pp = &p->next;
      }
      curr->next = NULL;
      curr->thread = NULL;
      curr->wq = NULL;
      LOCKDIAG_WQ_UNLOCK(wq);
    }
    curr = next;
  }
}

void wait_queue_wake_all(wait_queue_t *wq) {
  if (!wq)
    return;

  hal_irq_state_t rflags = hal_irq_save();
  LOCKDIAG_WQ_LOCK(wq);

  /* Detach every entry and clear its back-pointer before waking it: destroy
   * paths (e.g. socket_destroy(), eventfd_close()) free the queue as soon as
   * this returns, so a woken thread that reaches wait_queue_remove() later
   * must not touch this queue.  The wake still happens under the lock so a
   * concurrent sched_reap_thread()/wait_queue_cleanup_thread() cannot free the
   * thread between the detach and sched_wakeup(). */
  wait_queue_entry_t *curr = wq->head;
  wq->head = NULL;

  while (curr) {
    if (!wq_entry_ptr_sane(curr))
      break; /* corrupt/dangling list: stop rather than fault the kernel */
    wait_queue_entry_t *next = curr->next;
    curr->next = NULL;
    curr->wq = NULL;
    struct thread *t = curr->thread;
    if (t && t->state != THREAD_DEAD && t->state != THREAD_ZOMBIE) {
      t->wakeup_ticks = 0;
      sched_wakeup(t);
      LOCKDIAG_STAT(wq_wakes, 1);
    }
    curr = next;
  }

  LOCKDIAG_WQ_UNLOCK(wq);
  hal_irq_restore(rflags);
}

void wait_queue_wake_one(wait_queue_t *wq) {
  if (!wq)
    return;

  hal_irq_state_t rflags = hal_irq_save();
  LOCKDIAG_WQ_LOCK(wq);

  /* Unlink the entry being woken and clear its back-pointer before the lock is
   * dropped, for the same reason wake_all() does: the owner may free the queue
   * the moment we return.  Waking under the lock keeps the thread pointer safe
   * against a concurrent wait_queue_cleanup_thread(). */
  wait_queue_entry_t **pp = &wq->head;
  while (*pp) {
    wait_queue_entry_t *curr = *pp;
    if (!wq_entry_ptr_sane(curr))
      break; /* corrupt/dangling list: stop rather than fault the kernel */
    struct thread *t = curr->thread;
    if (t && t->state != THREAD_DEAD && t->state != THREAD_ZOMBIE) {
      *pp = curr->next;
      curr->next = NULL;
      curr->wq = NULL;
      t->wakeup_ticks = 0;
      sched_wakeup(t);
      LOCKDIAG_STAT(wq_wakes, 1);
      break;
    }
    pp = &curr->next;
  }

  LOCKDIAG_WQ_UNLOCK(wq);
  hal_irq_restore(rflags);
}
