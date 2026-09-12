/* LinuxKPI sleeping mutex with FIFO handoff.  See linux/mutex.h. */

#include <linux/errno.h>
#include <linux/mutex.h>

#include <linuxkpi/native_sched.h>

struct mutex_waiter {
  struct list_head list;
  struct task_struct *task;
};

static inline bool __mutex_try_acquire(struct mutex *lock) {
  struct task_struct *owner =
      __atomic_load_n(&lock->owner, __ATOMIC_RELAXED);

  if (owner)
    return false;

  return __atomic_compare_exchange_n(&lock->owner, &owner, current, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline bool __mutex_try_acquire_locked(struct mutex *lock) {
  if (lock->owner)
    return false;
  lock->owner = current;
  return true;
}

void mutex_init(struct mutex *lock) {
  spin_lock_init(&lock->wait_lock);
  INIT_LIST_HEAD(&lock->wait_list);
  lock->owner = NULL;
}

static int __mutex_lock_common(struct mutex *lock, bool interruptible) {
  if (__mutex_try_acquire(lock))
    return 0;

  struct mutex_waiter waiter;
  waiter.task = current;
  INIT_LIST_HEAD(&waiter.list);

  spin_lock(&lock->wait_lock);

  if (__mutex_try_acquire_locked(lock)) {
    spin_unlock(&lock->wait_lock);
    return 0;
  }

  list_add_tail(&waiter.list, &lock->wait_list);

  for (;;) {
    if (interruptible && signal_pending(current)) {
      list_del_init(&waiter.list);
      spin_unlock(&lock->wait_lock);
      return -ERESTARTSYS;
    }

    spin_unlock(&lock->wait_lock);
    linuxkpi_thread_block();
    spin_lock(&lock->wait_lock);

    if (lock->owner == current) {
      /* Ownership was handed to us by the unlocker. */
      list_del_init(&waiter.list);
      break;
    }
    if (!lock->owner) {
      lock->owner = current;
      list_del_init(&waiter.list);
      break;
    }
  }

  spin_unlock(&lock->wait_lock);
  return 0;
}

void mutex_lock(struct mutex *lock) { (void)__mutex_lock_common(lock, false); }

int mutex_lock_interruptible(struct mutex *lock) {
  return __mutex_lock_common(lock, true);
}

int mutex_lock_killable(struct mutex *lock) {
  return __mutex_lock_common(lock, true);
}

void mutex_lock_nested(struct mutex *lock, unsigned int subclass) {
  (void)subclass;
  mutex_lock(lock);
}

int mutex_trylock(struct mutex *lock) {
  return __mutex_try_acquire(lock) ? 1 : 0;
}

void mutex_unlock(struct mutex *lock) {
  struct task_struct *next = NULL;

  spin_lock(&lock->wait_lock);
  if (!list_empty(&lock->wait_list)) {
    struct mutex_waiter *waiter =
        list_first_entry(&lock->wait_list, struct mutex_waiter, list);
    next = waiter->task;
    list_del_init(&waiter->list);
  }
  lock->owner = next;
  spin_unlock(&lock->wait_lock);

  if (next)
    linuxkpi_wake_thread(task_struct_to_thread(next));
}

bool mutex_is_locked(struct mutex *lock) {
  return __atomic_load_n(&lock->owner, __ATOMIC_RELAXED) != NULL;
}

void mutex_destroy(struct mutex *lock) { (void)lock; }
