#ifndef __AVORY_LINUXKPI_NOTIFIER_H
#define __AVORY_LINUXKPI_NOTIFIER_H

/* AvoryOS overlay for <linux/notifier.h>.
 *
 * The upstream header defines the full notifier family in terms of srcu and
 * rwsem internals whose layouts AvoryOS does not model.  Imported DRM code
 * uses struct notifier_block (privacy-screen hooks) and the head variants in
 * prototypes only, so this keeps the upstream names and constants with a
 * minimal, self-contained layout.  Chain registration is implemented in
 * linuxkpi/src/notifier.c when something actually calls it. */

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/srcu.h>

/* Forward declaration is required before the typedef: a tag first named in a
 * function-pointer typedef's parameter list would get prototype scope and no
 * longer match the struct defined below. */
struct notifier_block;

typedef int (*notifier_fn_t)(struct notifier_block *nb,
                             unsigned long action, void *data);

struct notifier_block {
  notifier_fn_t notifier_call;
  struct notifier_block *next;
  int priority;
};

#define NOTIFY_DONE 0x0000
#define NOTIFY_OK 0x0001
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD (NOTIFY_STOP_MASK | 0x0002)
#define NOTIFY_STOP (NOTIFY_OK | NOTIFY_STOP_MASK)

struct atomic_notifier_head {
  spinlock_t lock;
  struct notifier_block *head;
};

struct blocking_notifier_head {
  struct rw_semaphore rwsem;
  struct notifier_block *head;
};

struct raw_notifier_head {
  struct notifier_block *head;
};

struct srcu_notifier_head {
  struct mutex mutex;
  struct srcu_struct srcu;
  struct notifier_block *head;
};

#define ATOMIC_INIT_NOTIFIER_HEAD(name)                                       \
  do {                                                                        \
    spin_lock_init(&(name)->lock);                                            \
    (name)->head = NULL;                                                      \
  } while (0)
#define BLOCKING_INIT_NOTIFIER_HEAD(name)                                     \
  do {                                                                        \
    init_rwsem(&(name)->rwsem);                                               \
    (name)->head = NULL;                                                      \
  } while (0)
#define RAW_INIT_NOTIFIER_HEAD(name) do { (name)->head = NULL; } while (0)

void srcu_init_notifier_head(struct srcu_notifier_head *nh);

int atomic_notifier_chain_register(struct atomic_notifier_head *nh,
                                   struct notifier_block *nb);
int blocking_notifier_chain_register(struct blocking_notifier_head *nh,
                                     struct notifier_block *nb);
int raw_notifier_chain_register(struct raw_notifier_head *nh,
                                struct notifier_block *nb);

int atomic_notifier_chain_unregister(struct atomic_notifier_head *nh,
                                     struct notifier_block *nb);
int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh,
                                       struct notifier_block *nb);
int raw_notifier_chain_unregister(struct raw_notifier_head *nh,
                                  struct notifier_block *nb);

int atomic_notifier_call_chain(struct atomic_notifier_head *nh,
                               unsigned long val, void *v);
int blocking_notifier_call_chain(struct blocking_notifier_head *nh,
                                 unsigned long val, void *v);
int raw_notifier_call_chain(struct raw_notifier_head *nh, unsigned long val,
                            void *v);
int srcu_notifier_call_chain(struct srcu_notifier_head *nh, unsigned long val,
                             void *v);

#endif /* __AVORY_LINUXKPI_NOTIFIER_H */
