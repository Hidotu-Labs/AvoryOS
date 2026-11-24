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
enum { WORK_IDLE, WORK_QUEUED, WORK_DELAYED };
/* Serializes work ownership across queues. Lock order is submission lock
 * then queue lock. */
static DEFINE_SPINLOCK(work_submit_lock);
static LIST_HEAD(work_executions);
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

/* Worker-owned records stay valid even if a callback frees its work item. */
struct wq_execution {
  struct list_head link;
  struct work_struct *work;
  work_func_t func;
  struct workqueue_struct *wq;
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
  unsigned long flags, submit_flags;
  struct wq_worker *wa = arg;
  struct workqueue_struct *wq = wa->wq;
  int idx = wa->idx;
  struct wq_execution execution = {.wq = wq};
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
    spin_lock_irqsave(&work_submit_lock, submit_flags);
    spin_lock_irqsave(&wq->lock, flags);
    struct work_struct *candidate;
    list_for_each_entry(candidate, &wq->pending, entry) {
      if (!kpi_work_running(wq, candidate)) {
        work = candidate;
        list_del_init(&work->entry);
        wq->current_work[idx] = work;
        execution.work = work;
        execution.func = work->func;
        list_add(&execution.link, &work_executions);
        WRITE_ONCE(work->pending, WORK_IDLE);
        break;
      }
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    spin_unlock_irqrestore(&work_submit_lock, submit_flags);
    if (!work) {
      /* Every queued item is still executing on another worker. */
      msleep(1);
      continue;
    }

    execution.func(work);

    spin_lock_irqsave(&work_submit_lock, submit_flags);
    spin_lock_irqsave(&wq->lock, flags);
    /* work may already be freed. Only clear the worker's side record. */
    list_del(&execution.link);
    wq->current_work[idx] = NULL;
    wq->active--;
    spin_unlock_irqrestore(&wq->lock, flags);
    spin_unlock_irqrestore(&work_submit_lock, submit_flags);

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

static struct workqueue_struct *executing_queue(struct work_struct *work) {
  struct wq_execution *execution;
  list_for_each_entry(execution, &work_executions, link) {
    if (execution->work == work && execution->func == work->func)
      return execution->wq;
  }
  return NULL;
}

/* Called with work_submit_lock held and IRQs disabled. */
static void enqueue_work(struct workqueue_struct *wq, struct work_struct *work) {
  spin_lock(&wq->lock);
  work->wq = wq;
  WRITE_ONCE(work->pending, WORK_QUEUED);
  wq->active++;
  wq->gen++;
  list_add_tail(&work->entry, &wq->pending);
  spin_unlock(&wq->lock);
}

static void kick_workqueue(struct workqueue_struct *wq) {
  __kpi_wake_up(&wq->more_work, 1, 0);
  wq_ensure_worker(wq);
}

static bool __queue_work(struct workqueue_struct *wq, struct work_struct *work) {
  unsigned long flags;
  spin_lock_irqsave(&work_submit_lock, flags);
  if (work->pending != WORK_IDLE || READ_ONCE(work->disabled)) {
    spin_unlock_irqrestore(&work_submit_lock, flags);
    return false;
  }
  struct workqueue_struct *running = executing_queue(work);
  if (running)
    wq = running;
  enqueue_work(wq, work);
  spin_unlock_irqrestore(&work_submit_lock, flags);
  kick_workqueue(wq);
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
  unsigned long flags;
  bool busy, waited = false;
  do {
    spin_lock_irqsave(&work_submit_lock, flags);
    busy = work->pending == WORK_QUEUED || executing_queue(work);
    spin_unlock_irqrestore(&work_submit_lock, flags);
    if (busy) {
      waited = true;
      msleep(1);
    }
  } while (busy);
  return waited;
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

/* Called with work_submit_lock held. Only a queue-owned item has a link. */
static bool dequeue_work(struct work_struct *work) {
  if (work->pending != WORK_QUEUED)
    return false;
  struct workqueue_struct *wq = work->wq;

  spin_lock(&wq->lock);
  list_del_init(&work->entry);
  WRITE_ONCE(work->pending, WORK_IDLE);
  wq->active--;
  spin_unlock(&wq->lock);
  __kpi_wake_up(&wq->flush_wait, 0, 0);
  return true;
}

bool cancel_work_sync(struct work_struct *work) {
  unsigned long flags;
  bool cancelled = false, running;
  /* The owner excludes external producers. Recheck under the queue lock
   * after each running instance: the callback may have queued another run
   * while cancellation was waiting. Never leave that pending node behind. */
  do {
    spin_lock_irqsave(&work_submit_lock, flags);
    cancelled |= dequeue_work(work);
    running = executing_queue(work) != NULL;
    spin_unlock_irqrestore(&work_submit_lock, flags);
    if (running)
      msleep(1);
  } while (running);
  return cancelled;
}

bool disable_work(struct work_struct *work) {
  if (!work)
    return false;
  unsigned long flags;
  spin_lock_irqsave(&work_submit_lock, flags);
  __atomic_fetch_add(&work->disabled, 1, __ATOMIC_ACQ_REL);
  bool cancelled = dequeue_work(work);
  spin_unlock_irqrestore(&work_submit_lock, flags);
  return cancelled;
}

bool disable_work_sync(struct work_struct *work) {
  if (!work)
    return false;
  bool cancelled = disable_work(work);
  (void)cancel_work_sync(work);
  return cancelled;
}

bool enable_work(struct work_struct *work) {
  if (!work)
    return false;
  int depth = __atomic_load_n(&work->disabled, __ATOMIC_ACQUIRE);
  while (depth > 0) {
    if (__atomic_compare_exchange_n(&work->disabled, &depth, depth - 1, false,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
      return depth == 1;
  }
  return false;
}

/* ── delayed work ───────────────────────────────────────────────────────── */

/* Promote a timer-owned item while holding work_submit_lock. */
static struct workqueue_struct *promote_delayed_work(struct work_struct *work) {
  if (READ_ONCE(work->disabled)) {
    WRITE_ONCE(work->pending, WORK_IDLE);
    return NULL;
  }
  struct workqueue_struct *wq = executing_queue(work);
  if (!wq)
    wq = work->wq;
  enqueue_work(wq, work);
  return wq;
}

void delayed_work_timer_fn(struct timer_list *timer) {
  struct delayed_work *dwork =
      container_of(timer, struct delayed_work, timer);
  struct workqueue_struct *wq = NULL;
  unsigned long flags;

  spin_lock_irqsave(&work_submit_lock, flags);
  /* A modifier can rearm this timer after it was taken off the wheel but
   * before this callback acquired the lock. The newer deadline owns it. */
  if (!dwork->canceling && dwork->work.pending == WORK_DELAYED &&
      !timer_pending(timer))
    wq = promote_delayed_work(&dwork->work);
  spin_unlock_irqrestore(&work_submit_lock, flags);
  if (wq)
    kick_workqueue(wq);
}

static bool submit_delayed_work(struct workqueue_struct *wq,
                                struct delayed_work *dwork,
                                unsigned long delay, bool modify) {
  struct work_struct *work = &dwork->work;
  unsigned long flags;

  spin_lock_irqsave(&work_submit_lock, flags);
  bool was_pending = work->pending != WORK_IDLE;
  if (dwork->canceling || READ_ONCE(work->disabled) ||
      (!modify && was_pending)) {
    spin_unlock_irqrestore(&work_submit_lock, flags);
    return false;
  }
  if (work->pending == WORK_DELAYED)
    del_timer(&dwork->timer);
  else
    dequeue_work(work);

  if (delay) {
    work->wq = wq;
    WRITE_ONCE(work->pending, WORK_DELAYED);
    mod_timer(&dwork->timer, jiffies + delay);
  } else {
    struct workqueue_struct *running = executing_queue(work);
    if (running)
      wq = running;
    enqueue_work(wq, work);
  }
  spin_unlock_irqrestore(&work_submit_lock, flags);
  if (!delay)
    kick_workqueue(wq);
  return modify ? was_pending : true;
}

bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *dwork, unsigned long delay) {
  return submit_delayed_work(wq, dwork, delay, false);
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
  return submit_delayed_work(wq, dwork, delay, true);
}

bool flush_delayed_work(struct delayed_work *dwork) {
  unsigned long flags;
  struct workqueue_struct *wq = NULL;

  spin_lock_irqsave(&work_submit_lock, flags);
  if (!dwork->canceling && dwork->work.pending == WORK_DELAYED) {
    del_timer(&dwork->timer);
    wq = promote_delayed_work(&dwork->work);
  }
  spin_unlock_irqrestore(&work_submit_lock, flags);
  if (wq)
    kick_workqueue(wq);
  /* A timer callback superseded by a rearm can still be returning. */
  linuxkpi_timer_wait_running(&dwork->timer);
  return flush_work(&dwork->work) || wq != NULL;
}

bool cancel_delayed_work(struct delayed_work *dwork) {
  unsigned long flags;
  struct work_struct *work = &dwork->work;

  spin_lock_irqsave(&work_submit_lock, flags);
  bool cancelled = dequeue_work(work);
  if (work->pending == WORK_DELAYED && del_timer(&dwork->timer)) {
    WRITE_ONCE(work->pending, WORK_IDLE);
    cancelled = true;
  }
  spin_unlock_irqrestore(&work_submit_lock, flags);
  return cancelled;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork) {
  unsigned long flags;
  struct work_struct *work = &dwork->work;

  /* Close submissions before draining the timer. The callback may try to
   * rearm itself, and the timer callback also takes the submission lock. */
  spin_lock_irqsave(&work_submit_lock, flags);
  dwork->canceling++;
  spin_unlock_irqrestore(&work_submit_lock, flags);

  del_timer_sync(&dwork->timer);
  spin_lock_irqsave(&work_submit_lock, flags);
  bool cancelled = work->pending == WORK_DELAYED;
  if (cancelled)
    WRITE_ONCE(work->pending, WORK_IDLE);
  spin_unlock_irqrestore(&work_submit_lock, flags);

  cancelled |= cancel_work_sync(work);
  spin_lock_irqsave(&work_submit_lock, flags);
  dwork->canceling--;
  spin_unlock_irqrestore(&work_submit_lock, flags);
  return cancelled;
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
