#ifndef __AVORY_LINUXKPI_SEMAPHORE_H
#define __AVORY_LINUXKPI_SEMAPHORE_H

/* Minimal Linux <linux/semaphore.h> overlay: a counting sleeping semaphore on
 * the native scheduler through <linux/wait.h>.  Implementation:
 * linuxkpi/src/semaphore.c. */

#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct semaphore {
  spinlock_t lock;
  unsigned int count;
  struct wait_queue_head wait;
};

#define __SEMAPHORE_INITIALIZER(name, n)                                      \
  {                                                                           \
    .lock = {0, 0}, .count = (n),                                             \
    .wait = __WAIT_QUEUE_HEAD_INITIALIZER((name).wait),                       \
  }

#define DEFINE_SEMAPHORE(name)                                                \
  struct semaphore name = __SEMAPHORE_INITIALIZER(name, 1)

void sema_init(struct semaphore *sem, int val);

void down(struct semaphore *sem);
int down_interruptible(struct semaphore *sem);
int down_killable(struct semaphore *sem);
int down_trylock(struct semaphore *sem);
int down_timeout(struct semaphore *sem, long timeout);
void up(struct semaphore *sem);

#endif /* __AVORY_LINUXKPI_SEMAPHORE_H */
