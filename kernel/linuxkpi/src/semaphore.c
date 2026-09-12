/* LinuxKPI counting semaphore.  See linux/semaphore.h for the contract. */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/semaphore.h>

void sema_init(struct semaphore *sem, int val) {
  spin_lock_init(&sem->lock);
  init_waitqueue_head(&sem->wait);
  sem->count = (unsigned int)val;
}

static inline bool __sema_available(struct semaphore *sem) {
  return sem->count != 0;
}

void down(struct semaphore *sem) {
  for (;;) {
    unsigned long flags;

    spin_lock_irqsave(&sem->lock, flags);
    if (sem->count) {
      sem->count--;
      spin_unlock_irqrestore(&sem->lock, flags);
      return;
    }
    spin_unlock_irqrestore(&sem->lock, flags);

    wait_event(sem->wait, __sema_available(sem));
  }
}

int down_interruptible(struct semaphore *sem) {
  for (;;) {
    unsigned long flags;

    spin_lock_irqsave(&sem->lock, flags);
    if (sem->count) {
      sem->count--;
      spin_unlock_irqrestore(&sem->lock, flags);
      return 0;
    }
    spin_unlock_irqrestore(&sem->lock, flags);

    long ret = wait_event_interruptible(sem->wait, __sema_available(sem));
    if (ret)
      return ret;
  }
}

int down_killable(struct semaphore *sem) { return down_interruptible(sem); }

int down_trylock(struct semaphore *sem) {
  unsigned long flags;
  int ret = 1;

  spin_lock_irqsave(&sem->lock, flags);
  if (sem->count) {
    sem->count--;
    ret = 0;
  }
  spin_unlock_irqrestore(&sem->lock, flags);
  return ret;
}

int down_timeout(struct semaphore *sem, long timeout) {
  for (;;) {
    unsigned long flags;

    spin_lock_irqsave(&sem->lock, flags);
    if (sem->count) {
      sem->count--;
      spin_unlock_irqrestore(&sem->lock, flags);
      return 0;
    }
    spin_unlock_irqrestore(&sem->lock, flags);

    long ret = wait_event_timeout(sem->wait, __sema_available(sem), timeout);
    if (!ret)
      return -ETIME;
  }
}

void up(struct semaphore *sem) {
  unsigned long flags;

  spin_lock_irqsave(&sem->lock, flags);
  sem->count++;
  spin_unlock_irqrestore(&sem->lock, flags);

  wake_up(&sem->wait);
}
