#ifndef __AVORY_LINUXKPI_RWSEM_H
#define __AVORY_LINUXKPI_RWSEM_H

/* Minimal Linux <linux/rwsem.h> overlay.
 *
 * A queued reader/writer sleeping lock built on the native scheduler through
 * <linux/wait.h>.  Readers share the semaphore; a writer needs exclusive
 * access.  Wakeups are "wake all waiters", so it is not strictly FIFO, and
 * there is no optimistic spinning or lockdep; correctness over speed.
 * Implementation: linuxkpi/src/rwsem.c. */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct rw_semaphore {
  spinlock_t wait_lock;
  struct wait_queue_head wait;
  struct task_struct *owner; /* current writer, NULL when free */
  unsigned int readers;
};

#define __RWSEM_INITIALIZER(name)                                            \
  {                                                                          \
    .wait_lock = {0, 0},                                                     \
    .wait = __WAIT_QUEUE_HEAD_INITIALIZER((name).wait),                      \
    .owner = NULL,                                                           \
    .readers = 0,                                                            \
  }

#define DECLARE_RWSEM(name) struct rw_semaphore name = __RWSEM_INITIALIZER(name)
#define init_rwsem(sem) __init_rwsem(sem)

void __init_rwsem(struct rw_semaphore *sem);

void down_read(struct rw_semaphore *sem);
int down_read_trylock(struct rw_semaphore *sem);
int down_read_killable(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);

void down_write(struct rw_semaphore *sem);
int down_write_trylock(struct rw_semaphore *sem);
int down_write_killable(struct rw_semaphore *sem);
void up_write(struct rw_semaphore *sem);
void downgrade_write(struct rw_semaphore *sem);

static inline bool rwsem_is_locked(const struct rw_semaphore *sem) {
  return sem->owner != NULL || sem->readers != 0;
}

#endif /* __AVORY_LINUXKPI_RWSEM_H */
