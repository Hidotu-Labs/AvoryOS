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
#include <linuxkpi/service.h>

#define KPI_WQ_MAX_WORKERS 4
static unsigned long workqueue_live_count;
unsigned long linuxkpi_workqueue_live_count(void) {
  return __atomic_load_n(&workqueue_live_count, __ATOMIC_ACQUIRE);
}

struct workqueue_struct {
  char *name;
  struct list_head pending;
  spinlock_t lock; /* Queue/retirement state is shared with hard IRQ producers. */
  struct wait_queue_head more_work;
  struct wait_queue_head flush_wait;
  unsigned int active; /* queued + running */
  unsigned int gen;    /* bumped on every enqueue */
  int max_workers;
  unsigned int nr_workers; /* live workers */
  struct task_struct *workers[KPI_WQ_MAX_WORKERS];
  /* A callback may free its containing object (TTM delayed BO deletion).
   * Keep execution state outside work_struct; never touch it after func(). */
  struct work_struct *current_work[KPI_WQ_MAX_WORKERS];
};

/* Worker start record: which queue and which slot this thread owns. */
struct wq_worker {
  struct workqueue_struct *wq;
  int idx;
};

struct workqueue_struct *system_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_power_efficient_wq;
struct workqueue_struct *system_freezable_wq;

/* Called with the queue lock held. Requeues on this queue must not execute
 * concurrently with the original invocation, even when pending was cleared. */
static bool kpi_work_running(struct workqueue_struct *wq,
                              struct work_struct *work) {
  for (int i = 0; i < KPI_WQ_MAX_WORKERS; i++) {
    if (wq->current_work[i] == work)
      return true;
  }
  return false;
}

static int worker_fn(void *arg) {
  unsigned long flags;
  struct wq_worker *wa = arg;
  struct workqueue_struct *wq = wa->wq;
  int idx = wa->idx;
  kfree(wa);

  for (;;) {
    unsigned int gen = __atomic_load_n(&wq->gen, __ATOMIC_ACQUIRE);

    (void)wait_event_timeout(wq->more_work,
                             kthread_should_stop() ||
                                 !list_empty(&wq->pending),
                             msecs_to_jiffies(KPI_SERVICE_IDLE_MS));

    if (kthread_should_stop())
      break;

    if (list_empty(&wq->pending)) {
      /* Idle timeout: retire unless work was published since the sample.
       * The slot clear and the generation check happen under the queue
       * lock, so a concurrent __queue_work() either sees this worker alive
       * or respawns one. */
      bool idle;

      spin_lock_irqsave(&wq->lock, flags);
      idle = list_empty(&wq->pending) &&
             __atomic_load_n(&wq->gen, __ATOMIC_ACQUIRE) == gen;
      if (idle) {
        wq->workers[idx] = NULL;
        if (wq->nr_workers)
          wq->nr_workers--;
      }
      spin_unlock_irqrestore(&wq->lock, flags);

      if (idle)
        return 0; /* the next queue_work() creates a replacement */
      continue;
    }

    /* Pending means queued, not executing. Clear it before the callback so
     * an interrupt or the callback itself can request a subsequent run. Keep
     * execution recorded under the same lock so another worker cannot run
     * the requeued item concurrently. Never move entries to an unlocked batch:
     * cancellation must own the queue lock when unlinking them. */
    struct work_struct *work = NULL;
    spin_lock_irqsave(&wq->lock, flags);
    struct work_struct *candidate;
    list_for_each_entry(candidate, &wq->pending, entry) {
      if (!kpi_work_running(wq, candidate)) {
        work = candidate;
        list_del_init(&work->entry);
        wq->current_work[idx] = work;
        __atomic_store_n(&work->pending, 0, __ATOMIC_RELEASE);
        break;
      }
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    if (!work) {
      /* Every queued item is still executing on another worker. */
      msleep(1);
      continue;
    }

    work->func(work);

    spin_lock_irqsave(&wq->lock, flags);
    /* work may already be freed. Only clear the worker's side record. */
    wq->current_work[idx] = NULL;
    wq->active--;
    spin_unlock_irqrestore(&wq->lock, flags);

    __kpi_wake_up(&wq->flush_wait, 0, 0);
    __kpi_wake_up(&wq->more_work, 1, 0);
  }

  spin_lock_irqsave(&wq->lock, flags);
  if (wq->workers[idx]) {
    wq->workers[idx] = NULL;
    if (wq->nr_workers)
      wq->nr_workers--;
  }
  spin_unlock_irqrestore(&wq->lock, flags);
  return 0;
}

/* Create a replacement worker when the queue has none.  Called after work is
 * enqueued; runs under the queue lock so a worker retiring concurrently
 * either observes the new work or leaves its slot empty for us. */
static void wq_ensure_worker(struct workqueue_struct *wq) {
  unsigned long flags;
  spin_lock_irqsave(&wq->lock, flags);

  if (wq->nr_workers > 0) {
    /* Spawn extra help only when the queued+running work outnumbers the
     * live workers; otherwise the existing ones already own it. */
    bool need_more = wq->active > wq->nr_workers &&
                     wq->nr_workers < (unsigned int)wq->max_workers;
    if (!need_more) {
      spin_unlock_irqrestore(&wq->lock, flags);
      return;
    }
  }

  int idx = -1;
  for (int i = 0; i < KPI_WQ_MAX_WORKERS; i++) {
    if (!wq->workers[i]) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    spin_unlock_irqrestore(&wq->lock, flags);
    return;
  }

  struct wq_worker *wa = kmalloc(sizeof(*wa), GFP_KERNEL);
  if (!wa) {
    spin_unlock_irqrestore(&wq->lock, flags);
    return;
  }
  wa->wq = wq;
  wa->idx = idx;

  struct task_struct *t =
      kthread_create(worker_fn, wa, "kworker/%s", wq->name);
  if (IS_ERR(t)) {
    kfree(wa);
  } else {
    wq->workers[idx] = t;
    kthread_detach(t);
    wq->nr_workers++;
    wake_up_process(t);
  }

  spin_unlock_irqrestore(&wq->lock, flags);
}

static bool __queue_work(struct workqueue_struct *wq, struct work_struct *work) {
  unsigned long flags;
  spin_lock_irqsave(&wq->lock, flags);

  if (work->pending) {
    spin_unlock_irqrestore(&wq->lock, flags);
    return false;
  }

  work->pending = 1;
  work->wq = wq;
  wq->active++;
  wq->gen++;
  list_add_tail(&work->entry, &wq->pending);

  spin_unlock_irqrestore(&wq->lock, flags);

  __kpi_wake_up(&wq->more_work, 1, 0);
  wq_ensure_worker(wq);
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
  unsigned long flags;
  bool busy;
  if (!wq)
    return false;
  do {
    spin_lock_irqsave(&wq->lock, flags);
    busy = work->pending || kpi_work_running(wq, work);
    spin_unlock_irqrestore(&wq->lock, flags);
    if (busy)
      msleep(1);
  } while (busy);
  return true;
}

void flush_workqueue(struct workqueue_struct *wq) {
  wait_event(wq->flush_wait, wq->active == 0);
}

/* A delayed invocation still armed on its timer must execute now before
 * flush returns. Merely flushing work misses the entire timer phase. */
bool flush_delayed_work(struct delayed_work *dwork) {
  if (del_timer_sync(&dwork->timer))
    __queue_work(dwork->work.wq ? dwork->work.wq : system_wq, &dwork->work);
  return flush_work(&dwork->work);
}

void drain_workqueue(struct workqueue_struct *wq) {
  /* Conservative equivalent of upstream's drain: wait until nothing is
   * pending/running.  New work queued after this returns simply runs; it is
   * not blocked the way upstream's draining state blocks it. */
  flush_workqueue(wq);
}

bool cancel_work_sync(struct work_struct *work) {
  unsigned long flags;
  struct workqueue_struct *wq = work->wq;
  bool cancelled = false, running;

  if (!wq)
    return false;
  /* The owner excludes external producers. Recheck under the queue lock
   * after each running instance: the callback may have queued another run
   * while cancellation was waiting. Never leave that pending node behind. */
  do {
    spin_lock_irqsave(&wq->lock, flags);
    if (work->pending) {
      list_del_init(&work->entry);
      work->pending = 0;
      wq->active--;
      cancelled = true;
    }
    running = kpi_work_running(wq, work);
    spin_unlock_irqrestore(&wq->lock, flags);
    __kpi_wake_up(&wq->flush_wait, 0, 0);
    if (running)
      msleep(1);
  } while (running);
  return cancelled;
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
  if (__atomic_load_n(&dwork->canceling, __ATOMIC_ACQUIRE))
    return false;
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
  if (__atomic_load_n(&dwork->canceling, __ATOMIC_ACQUIRE))
    return false;
  if (delay == 0) {
    del_timer(&dwork->timer);
    return __queue_work(wq, &dwork->work);
  }

  dwork->work.wq = wq;
  mod_timer(&dwork->timer, jiffies + delay);
  return true;
}

bool cancel_delayed_work(struct delayed_work *dwork) {
  bool pending = del_timer(&dwork->timer) != 0;
  struct work_struct *work = &dwork->work;
  struct workqueue_struct *wq = work->wq;
  unsigned long flags;
  if (wq) {
    spin_lock_irqsave(&wq->lock, flags);
    if (work->pending) {
      list_del_init(&work->entry);
      work->pending = 0;
      wq->active--;
      pending = true;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    __kpi_wake_up(&wq->flush_wait, 0, 0);
  }
  return pending;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork) {
  /* The owner excludes external producers, but the running callback may
   * rearm itself. Block both queue/mod paths until timer and work drain.
   * Waiting on work alone after deleting the timer leaves that rearm live. */
  __atomic_store_n(&dwork->canceling, 1, __ATOMIC_RELEASE);
  int ret = del_timer_sync(&dwork->timer);

  bool pending = cancel_work_sync(&dwork->work);
  __atomic_store_n(&dwork->canceling, 0, __ATOMIC_RELEASE);
  return ret != 0 || pending;
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
  wq->max_workers = max_active;
  /* Workers are created on demand by wq_ensure_worker() and retire after an
   * idle timeout, so an unused queue costs no thread. */

  __atomic_add_fetch(&workqueue_live_count, 1, __ATOMIC_RELEASE);
  return wq;
}

void destroy_workqueue(struct workqueue_struct *wq) {
  unsigned long flags;
  /* Stopping workers first abandons pending callbacks and leaves their wq
   * pointers dangling. The owner must exclude external submissions. */
  drain_workqueue(wq);
  for (int i = 0; i < KPI_WQ_MAX_WORKERS; i++) {
    struct task_struct *t;

    spin_lock_irqsave(&wq->lock, flags);
    t = linuxkpi_task_get(wq->workers[i]);
    wq->workers[i] = NULL;
    spin_unlock_irqrestore(&wq->lock, flags);

    if (t)
      kthread_stop(t);
  }
  kfree(wq->name);
  kfree(wq);
  __atomic_sub_fetch(&workqueue_live_count, 1, __ATOMIC_RELEASE);
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
