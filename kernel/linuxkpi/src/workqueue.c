/* LinuxKPI workqueues.  See linux/workqueue.h for the contract and the
 * currently unsupported Linux features. */

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stdarg.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <linuxkpi/log.h>

#define KPI_WQ_MAX_WORKERS 4

struct workqueue_struct {
  char *name;
  struct list_head pending;
  spinlock_t lock;
  struct wait_queue_head more_work;
  struct wait_queue_head flush_wait;
  unsigned int active; /* queued + running */
  int nr_workers;
  struct task_struct *workers[KPI_WQ_MAX_WORKERS];
};

struct workqueue_struct *system_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_power_efficient_wq;
struct workqueue_struct *system_freezable_wq;

static int worker_fn(void *arg) {
  struct workqueue_struct *wq = arg;

  for (;;) {
    wait_event(wq->more_work,
               kthread_should_stop() || !list_empty(&wq->pending));

    if (kthread_should_stop())
      return 0;

    LIST_HEAD(batch);

    spin_lock(&wq->lock);
    list_splice_init(&wq->pending, &batch);
    spin_unlock(&wq->lock);

    while (!list_empty(&batch)) {
      struct work_struct *work =
          list_first_entry(&batch, struct work_struct, entry);
      list_del_init(&work->entry);

      __atomic_store_n(&work->running, 1, __ATOMIC_RELEASE);
      work->func(work);
      __atomic_store_n(&work->running, 0, __ATOMIC_RELEASE);

      spin_lock(&wq->lock);
      work->pending = 0;
      wq->active--;
      bool wake = (wq->active == 0);
      spin_unlock(&wq->lock);

      if (wake)
        __kpi_wake_up(&wq->flush_wait, 0, 0);
    }
  }
}

static bool __queue_work(struct workqueue_struct *wq, struct work_struct *work) {
  spin_lock(&wq->lock);

  if (work->pending) {
    spin_unlock(&wq->lock);
    return false;
  }

  work->pending = 1;
  work->wq = wq;
  wq->active++;
  list_add_tail(&work->entry, &wq->pending);

  spin_unlock(&wq->lock);

  __kpi_wake_up(&wq->more_work, 1, 0);
  return true;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work) {
  return __queue_work(wq, work);
}

bool queue_work_on(int cpu, struct workqueue_struct *wq,
                   struct work_struct *work) {
  (void)cpu;
  return __queue_work(wq, work);
}

bool schedule_work(struct work_struct *work) {
  return __queue_work(system_wq, work);
}

bool schedule_work_on(int cpu, struct work_struct *work) {
  (void)cpu;
  return __queue_work(system_wq, work);
}

bool flush_work(struct work_struct *work) {
  struct workqueue_struct *wq = work->wq;

  if (!wq)
    return false;

  while (__atomic_load_n(&work->pending, __ATOMIC_ACQUIRE))
    wait_event(wq->flush_wait,
               !__atomic_load_n(&work->pending, __ATOMIC_ACQUIRE));

  return true;
}

void flush_workqueue(struct workqueue_struct *wq) {
  wait_event(wq->flush_wait, wq->active == 0);
}

void drain_workqueue(struct workqueue_struct *wq) {
  /* Conservative equivalent of upstream's drain: wait until nothing is
   * pending/running.  New work queued after this returns simply runs; it is
   * not blocked the way upstream's draining state blocks it. */
  flush_workqueue(wq);
}

bool cancel_work_sync(struct work_struct *work) {
  struct workqueue_struct *wq = work->wq;

  if (wq) {
    spin_lock(&wq->lock);
    if (work->pending && !work->running) {
      list_del_init(&work->entry);
      work->pending = 0;
      wq->active--;
      bool wake = (wq->active == 0);
      spin_unlock(&wq->lock);
      if (wake)
        __kpi_wake_up(&wq->flush_wait, 0, 0);
    } else {
      spin_unlock(&wq->lock);
    }

    while (__atomic_load_n(&work->running, __ATOMIC_ACQUIRE))
      wait_event(wq->flush_wait,
                 !__atomic_load_n(&work->running, __ATOMIC_ACQUIRE));
  }

  return true;
}

/* ── delayed work ───────────────────────────────────────────────────────── */

void delayed_work_timer_fn(struct timer_list *timer) {
  struct delayed_work *dwork =
      container_of(timer, struct delayed_work, timer);
  struct workqueue_struct *wq = dwork->work.wq ? dwork->work.wq : system_wq;

  __queue_work(wq, &dwork->work);
}

bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *dwork, unsigned long delay) {
  if (delay == 0)
    return __queue_work(wq, &dwork->work);

  dwork->work.wq = wq;
  mod_timer(&dwork->timer, jiffies + delay);
  return true;
}

bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay) {
  return queue_delayed_work(system_wq, dwork, delay);
}

bool schedule_delayed_work_on(int cpu, struct delayed_work *dwork,
                              unsigned long delay) {
  (void)cpu;
  return schedule_delayed_work(dwork, delay);
}

bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
                      unsigned long delay) {
  if (delay == 0) {
    del_timer(&dwork->timer);
    return __queue_work(wq, &dwork->work);
  }

  dwork->work.wq = wq;
  mod_timer(&dwork->timer, jiffies + delay);
  return true;
}

bool cancel_delayed_work(struct delayed_work *dwork) {
  return del_timer(&dwork->timer) != 0;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork) {
  int ret = del_timer_sync(&dwork->timer);

  cancel_work_sync(&dwork->work);
  return ret != 0;
}

/* ── allocation ─────────────────────────────────────────────────────────── */

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags,
                                         int max_active, ...) {
  char tmp[WQ_NAME_LEN];
  va_list args;

  (void)flags;

  va_start(args, max_active);
  vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);

  struct workqueue_struct *wq = kmalloc(sizeof(*wq), GFP_KERNEL);
  if (!wq)
    return NULL;

  __builtin_memset(wq, 0, sizeof(*wq));

  size_t name_len = strlen(tmp) + 1;
  wq->name = kmalloc(name_len, GFP_KERNEL);
  if (!wq->name) {
    kfree(wq);
    return NULL;
  }
  __builtin_memcpy(wq->name, tmp, name_len);

  INIT_LIST_HEAD(&wq->pending);
  spin_lock_init(&wq->lock);
  init_waitqueue_head(&wq->more_work);
  init_waitqueue_head(&wq->flush_wait);

  if (max_active < 1)
    max_active = 1;
  if (max_active > KPI_WQ_MAX_WORKERS)
    max_active = KPI_WQ_MAX_WORKERS;
  wq->nr_workers = max_active;

  for (int i = 0; i < wq->nr_workers; i++) {
    wq->workers[i] = kthread_run(worker_fn, wq, "kworker/%s", wq->name);
  }

  return wq;
}

void destroy_workqueue(struct workqueue_struct *wq) {
  for (int i = 0; i < wq->nr_workers; i++) {
    if (wq->workers[i])
      kthread_stop(wq->workers[i]);
  }
  kfree(wq->name);
  kfree(wq);
}

void linuxkpi_workqueue_init(void) {
  system_wq = alloc_workqueue("events", 0, 2);
  system_long_wq = alloc_workqueue("events_long", 0, 1);
  system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND, 2);
  system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 1);
  system_power_efficient_wq =
      alloc_workqueue("events_power_efficient", 0, 1);
  system_freezable_wq = alloc_workqueue("events_freezable", 0, 1);
}
