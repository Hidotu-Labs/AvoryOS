/* Native DRM transaction mutexes. The ownership decision and publication
 * of lock->ctx must be atomic with respect to competing transactions. A
 * try/unlock probe followed by mutex_lock() loses that guarantee and can
 * sleep through a wound or a change to an older holder (ABBA deadlock).
 *
 * base.wait_lock protects owner, ctx and this private waiter list. No plain
 * mutex operation is used on base; it remains available to lock diagnostics.
 * Waiters with tickets are stamp ordered; NULL-context users remain FIFO.
 * All sleepers recheck the current owner/tickets after a wake.
 */
#include <linux/errno.h>
#include <linux/ww_mutex.h>
#include <linuxkpi/native_sched.h>

struct kpi_ww_waiter {
  struct list_head list;
  struct task_struct *task;
  struct ww_acquire_ctx *ctx;
};

void ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class) {
  mutex_init(&lock->base);
  lock->ww_class = ww_class;
  lock->ctx = NULL;
}

void ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class) {
  ctx->task = current;
  ctx->stamp = __atomic_add_fetch(&ww_class->stamp, 1, __ATOMIC_RELAXED);
  ctx->acquired = 0;
  ctx->wounded = false;
}
void ww_acquire_done(struct ww_acquire_ctx *ctx) { (void)ctx; }
void ww_acquire_fini(struct ww_acquire_ctx *ctx) { (void)ctx; }

static bool ww_older(struct ww_acquire_ctx *a, struct ww_acquire_ctx *b) {
  /* Signed distance also handles wraparound of the ticket counter. */
  return (long)(a->stamp - b->stamp) < 0;
}

static void ww_queue(struct ww_mutex *lock, struct kpi_ww_waiter *waiter) {
  struct kpi_ww_waiter *other;
  if (waiter->ctx) {
    list_for_each_entry(other, &lock->base.wait_list, list) {
      if (other->ctx && ww_older(waiter->ctx, other->ctx)) {
        /* list_add_tail before this entry, preserving NULL-context FIFO. */
        list_add_tail(&waiter->list, &other->list);
        return;
      }
    }
  }
  list_add_tail(&waiter->list, &lock->base.wait_list);
}

static void ww_publish(struct ww_mutex *lock, struct task_struct *task,
                       struct ww_acquire_ctx *ctx) {
  lock->ctx = ctx;
  if (ctx)
    __atomic_add_fetch(&ctx->acquired, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&lock->base.owner, task, __ATOMIC_RELEASE);
}

static void ww_wake_waiters(struct ww_mutex *lock) {
  struct kpi_ww_waiter *waiter;
  list_for_each_entry(waiter, &lock->base.wait_list, list)
    linuxkpi_wake_thread(task_struct_to_thread(waiter->task));
}

static int ww_check_contended(struct ww_mutex *lock,
                               struct ww_acquire_ctx *ctx) {
  struct kpi_ww_waiter *other;
  struct ww_acquire_ctx *holder = lock->ctx;
  if (!ctx)
    return 0;
  if (lock->ww_class->is_wait_die) {
    /* The first lock cannot form a cycle. Backoff must retain the ticket,
     * but drop all locks; acquired is decremented on every unlock. */
    if (!__atomic_load_n(&ctx->acquired, __ATOMIC_RELAXED))
      return 0;
    if (holder && ww_older(holder, ctx))
      return -EDEADLK;
    /* An older queued ticket may become the owner before us. Do not sleep
     * holding another BO behind it, which would create the same cycle. */
    list_for_each_entry(other, &lock->base.wait_list, list)
      if (other->ctx && other->ctx != ctx && ww_older(other->ctx, ctx))
        return -EDEADLK;
  } else {
    if (__atomic_load_n(&ctx->acquired, __ATOMIC_RELAXED) &&
        __atomic_load_n(&ctx->wounded, __ATOMIC_ACQUIRE))
      return -EDEADLK;
    if (holder && ww_older(ctx, holder)) {
      __atomic_store_n(&holder->wounded, true, __ATOMIC_RELEASE);
      /* The holder may already be asleep on a different transaction lock.
       * Wake it to observe the wound, even if our own lock stays occupied. */
      linuxkpi_wake_thread(task_struct_to_thread(holder->task));
    }
  }
  return 0;
}

static int ww_lock_common(struct ww_mutex *lock, struct ww_acquire_ctx *ctx,
                           bool interruptible, bool slow) {
  struct kpi_ww_waiter waiter;
  struct task_struct *task = current;
  unsigned long flags;
  int ret = 0;
  bool queued = false;
  INIT_LIST_HEAD(&waiter.list);
  waiter.task = task;
  waiter.ctx = ctx;
  /* Reset wounds only after dropping every lock, including the slow retry.
   * A wounded transaction holding BOs must still back off on contention. */
  if (ctx && !__atomic_load_n(&ctx->acquired, __ATOMIC_RELAXED))
    __atomic_store_n(&ctx->wounded, false, __ATOMIC_RELEASE);
  if (slow && ctx && __atomic_load_n(&ctx->acquired, __ATOMIC_RELAXED))
    return -EINVAL;

  spin_lock_irqsave(&lock->base.wait_lock, flags);
  if (ctx && lock->ctx == ctx) {
    ret = -EALREADY;
    goto out;
  }
  for (;;) {
    bool first = list_empty(&lock->base.wait_list) ||
                 (queued && lock->base.wait_list.next == &waiter.list);
    if (!__atomic_load_n(&lock->base.owner, __ATOMIC_RELAXED) && first) {
      if (queued)
        list_del_init(&waiter.list);
      ww_publish(lock, task, ctx);
      break;
    }
    ret = ww_check_contended(lock, ctx);
    if (ret)
      break;
    if (interruptible && signal_pending(task)) {
      ret = -ERESTARTSYS;
      break;
    }
    if (!queued) {
      ww_queue(lock, &waiter);
      queued = true;
      /* Stamp ordering can put us ahead of an already queued waiter after
       * the previous owner released the lock. Recheck before sleeping: we
       * may now be first with owner == NULL and no future unlock to wake us. */
      continue;
    }
    spin_unlock_irqrestore(&lock->base.wait_lock, flags);
    linuxkpi_mutex_wait_set(&lock->base);
    linuxkpi_thread_block();
    linuxkpi_mutex_wait_set(NULL);
    spin_lock_irqsave(&lock->base.wait_lock, flags);
  }
  if (queued) {
    list_del_init(&waiter.list);
    /* A canceled head may leave a free lock in front of sleeping waiters. */
    if (ret && !__atomic_load_n(&lock->base.owner, __ATOMIC_RELAXED))
      ww_wake_waiters(lock);
  }
out:
  spin_unlock_irqrestore(&lock->base.wait_lock, flags);
  return ret;
}

int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) {
  return ww_lock_common(lock, ctx, false, false);
}
int ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) {
  return ww_lock_common(lock, ctx, true, false);
}
int ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) {
  return ww_lock_common(lock, ctx, false, true);
}
int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) {
  return ww_lock_common(lock, ctx, true, true);
}
int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) {
  struct task_struct *task = current;
  unsigned long flags;
  int ret = 0;
  spin_lock_irqsave(&lock->base.wait_lock, flags);
  if (!__atomic_load_n(&lock->base.owner, __ATOMIC_RELAXED) &&
      list_empty(&lock->base.wait_list)) {
    if (ctx && !__atomic_load_n(&ctx->acquired, __ATOMIC_RELAXED))
      __atomic_store_n(&ctx->wounded, false, __ATOMIC_RELEASE);
    ww_publish(lock, task, ctx);
    ret = 1;
  }
  spin_unlock_irqrestore(&lock->base.wait_lock, flags);
  return ret;
}
void ww_mutex_unlock(struct ww_mutex *lock) {
  unsigned long flags;
  spin_lock_irqsave(&lock->base.wait_lock, flags);
  if (lock->ctx)
    __atomic_sub_fetch(&lock->ctx->acquired, 1, __ATOMIC_RELAXED);
  lock->ctx = NULL;
  __atomic_store_n(&lock->base.owner, NULL, __ATOMIC_RELEASE);
  /* Wake all: the head competes for ownership and the rest must reevaluate
   * deadlock/backoff against the newly published owner on the next pass. */
  ww_wake_waiters(lock);
  spin_unlock_irqrestore(&lock->base.wait_lock, flags);
}
void ww_mutex_destroy(struct ww_mutex *lock) { mutex_destroy(&lock->base); }
bool ww_mutex_is_locked(struct ww_mutex *lock) { return mutex_is_locked(&lock->base); }
