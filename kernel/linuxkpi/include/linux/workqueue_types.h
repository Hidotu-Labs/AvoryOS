#ifndef _LINUX_WORKQUEUE_TYPES_H
#define _LINUX_WORKQUEUE_TYPES_H

/* Shared LinuxKPI work item layout for workqueue.h and imported split-type
 * users. Keep the native worker state here so either include order gets the
 * same object representation. */
#include <linux/timer_types.h>
#include <linux/types.h>

struct workqueue_struct;
struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
  struct list_head entry;
  work_func_t func;
  volatile int pending;
  volatile int running;
  volatile int disabled;
  struct workqueue_struct *wq;
};

void delayed_work_timer_fn(struct timer_list *t);

#endif /* _LINUX_WORKQUEUE_TYPES_H */
