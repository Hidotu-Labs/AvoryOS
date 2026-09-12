/* LinuxKPI tasklets (workqueue-backed).  See linux/tasklet.h. */

#include <linux/tasklet.h>

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
                  unsigned long data) {
  INIT_WORK(&t->work, __kpi_tasklet_entry);
  t->func = func;
  t->data = data;
  t->count = 0;
}

void tasklet_schedule(struct tasklet_struct *t) { schedule_work(&t->work); }

void tasklet_hi_schedule(struct tasklet_struct *t) {
  schedule_work(&t->work);
}

void tasklet_disable_nosync(struct tasklet_struct *t) {
  __atomic_add_fetch(&t->count, 1, __ATOMIC_ACQ_REL);
}

void tasklet_disable(struct tasklet_struct *t) {
  tasklet_disable_nosync(t);
  cancel_work_sync(&t->work);
}

void tasklet_enable(struct tasklet_struct *t) {
  __atomic_sub_fetch(&t->count, 1, __ATOMIC_ACQ_REL);
}

void tasklet_kill(struct tasklet_struct *t) {
  cancel_work_sync(&t->work);
  while (__atomic_load_n(&t->count, __ATOMIC_ACQUIRE) > 0)
    yield();
}
