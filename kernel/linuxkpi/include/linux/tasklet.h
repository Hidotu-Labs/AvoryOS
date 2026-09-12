#ifndef __AVORY_LINUXKPI_TASKLET_H
#define __AVORY_LINUXKPI_TASKLET_H

/* Minimal Linux <linux/tasklet.h> overlay.
 *
 * AvoryOS has no softirq layer; tasklets are emulated by scheduling a work
 * item on system_wq.  A disabled tasklet (count != 0) is dropped when it runs,
 * matching Linux's "schedule is lost while disabled" semantics.
 * Implementation: linuxkpi/src/tasklet.c. */

#include <linux/container_of.h>
#include <linux/workqueue.h>

struct tasklet_struct {
  struct work_struct work;
  void (*func)(unsigned long data);
  unsigned long data;
  int count;
};

static inline void __kpi_tasklet_entry(struct work_struct *work) {
  struct tasklet_struct *t =
      container_of(work, struct tasklet_struct, work);

  if (__atomic_load_n(&t->count, __ATOMIC_ACQUIRE) == 0)
    t->func(t->data);
}

#define DECLARE_TASKLET(_name, _func, _data)                                  \
  struct tasklet_struct _name = {                                             \
    .work = {{NULL, NULL}, __kpi_tasklet_entry, 0, 0, NULL},                  \
    .func = (_func), .data = (_data), .count = 0,                             \
  }

#define DECLARE_TASKLET_DISABLED(_name, _func, _data)                         \
  struct tasklet_struct _name = {                                             \
    .work = {{NULL, NULL}, __kpi_tasklet_entry, 0, 0, NULL},                  \
    .func = (_func), .data = (_data), .count = 1,                             \
  }

#define from_tasklet(var, callback_tasklet, tasklet_fieldname)                \
  container_of(callback_tasklet, typeof(*var), tasklet_fieldname)

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
                  unsigned long data);
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_hi_schedule(struct tasklet_struct *t);
void tasklet_disable(struct tasklet_struct *t);
void tasklet_disable_nosync(struct tasklet_struct *t);
void tasklet_enable(struct tasklet_struct *t);
void tasklet_kill(struct tasklet_struct *t);

#endif /* __AVORY_LINUXKPI_TASKLET_H */
