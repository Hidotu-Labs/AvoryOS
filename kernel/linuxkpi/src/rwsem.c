/* LinuxKPI reader/writer semaphore.  See linux/rwsem.h for the contract. */

#include <linux/rwsem.h>

static inline bool __rwsem_can_read(struct rw_semaphore *sem) {
  return sem->owner == NULL;
}

static inline bool __rwsem_can_write(struct rw_semaphore *sem) {
  return sem->owner == NULL && sem->readers == 0;
}

void __init_rwsem(struct rw_semaphore *sem) {
  spin_lock_init(&sem->wait_lock);
  init_waitqueue_head(&sem->wait);
  sem->owner = NULL;
  sem->readers = 0;
}

void down_read(struct rw_semaphore *sem) {
  for (;;) {
    unsigned long flags;

    spin_lock_irqsave(&sem->wait_lock, flags);
    if (__rwsem_can_read(sem)) {
      sem->readers++;
      spin_unlock_irqrestore(&sem->wait_lock, flags);
      return;
    }
    spin_unlock_irqrestore(&sem->wait_lock, flags);

    wait_event(sem->wait, __rwsem_can_read(sem));
  }
}

int down_read_trylock(struct rw_semaphore *sem) {
  unsigned long flags;
  int ret = 0;

  spin_lock_irqsave(&sem->wait_lock, flags);
  if (__rwsem_can_read(sem)) {
    sem->readers++;
    ret = 1;
  }
  spin_unlock_irqrestore(&sem->wait_lock, flags);
  return ret;
}

int down_read_killable(struct rw_semaphore *sem) {
  for (;;) {
    unsigned long flags;

    spin_lock_irqsave(&sem->wait_lock, flags);
    if (__rwsem_can_read(sem)) {
      sem->readers++;
      spin_unlock_irqrestore(&sem->wait_lock, flags);
      return 0;
    }
    spin_unlock_irqrestore(&sem->wait_lock, flags);

    long ret = wait_event_interruptible(sem->wait, __rwsem_can_read(sem));
    if (ret)
      return ret;
  }
}

void up_read(struct rw_semaphore *sem) {
  unsigned long flags;
  bool wake = false;

  spin_lock_irqsave(&sem->wait_lock, flags);
  if (sem->readers && --sem->readers == 0)
    wake = true;
  spin_unlock_irqrestore(&sem->wait_lock, flags);

  if (wake)
    wake_up_all(&sem->wait);
}

void down_write(struct rw_semaphore *sem) {
  for (;;) {
    unsigned long flags;

    spin_lock_irqsave(&sem->wait_lock, flags);
    if (__rwsem_can_write(sem)) {
      sem->owner = current;
      spin_unlock_irqrestore(&sem->wait_lock, flags);
      return;
    }
    spin_unlock_irqrestore(&sem->wait_lock, flags);

    wait_event(sem->wait, __rwsem_can_write(sem));
  }
}

int down_write_trylock(struct rw_semaphore *sem) {
  unsigned long flags;
  int ret = 0;

  spin_lock_irqsave(&sem->wait_lock, flags);
  if (__rwsem_can_write(sem)) {
    sem->owner = current;
    ret = 1;
  }
  spin_unlock_irqrestore(&sem->wait_lock, flags);
  return ret;
}

int down_write_killable(struct rw_semaphore *sem) {
  for (;;) {
    unsigned long flags;

    spin_lock_irqsave(&sem->wait_lock, flags);
    if (__rwsem_can_write(sem)) {
      sem->owner = current;
      spin_unlock_irqrestore(&sem->wait_lock, flags);
      return 0;
    }
    spin_unlock_irqrestore(&sem->wait_lock, flags);

    long ret = wait_event_interruptible(sem->wait, __rwsem_can_write(sem));
    if (ret)
      return ret;
  }
}

void up_write(struct rw_semaphore *sem) {
  unsigned long flags;

  spin_lock_irqsave(&sem->wait_lock, flags);
  sem->owner = NULL;
  spin_unlock_irqrestore(&sem->wait_lock, flags);

  wake_up_all(&sem->wait);
}

void downgrade_write(struct rw_semaphore *sem) {
  unsigned long flags;

  spin_lock_irqsave(&sem->wait_lock, flags);
  sem->owner = NULL;
  sem->readers = 1;
  spin_unlock_irqrestore(&sem->wait_lock, flags);

  wake_up_all(&sem->wait);
}
