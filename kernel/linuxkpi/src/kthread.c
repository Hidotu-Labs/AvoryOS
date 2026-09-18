/* LinuxKPI kernel threads.  See linux/kthread.h and the native bridge
 * linuxkpi/native_sched.h.
 *
 * Each thread carries a control block (magic-tagged, stored in the native
 * thread's kpi_data) with the Linux threadfn/data and the cooperative stop
 * flag.  The block pointer is also cached in the task shadow (kpi_control),
 * so kthread_stop() can still find it after the native thread exited and was
 * reaped; kthread_stop() frees the block.  A self-exiting thread that nobody
 * stops leaves its (small) block and shadow behind - the documented
 * behaviour for kernel threads the boot never joins. */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stdarg.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_sched.h>
#include <linuxkpi/service.h>

#define KPI_KTHREAD_MAGIC 0x4b544852u /* "KTHR" */

struct kpi_kthread {
  unsigned int magic;
  int (*threadfn)(void *data);
  void *data;
  volatile int should_stop;
  struct completion exited;
  /* Native thread pointer while the thread runs; set when the trampoline
   * starts and cleared before it completes `exited`, so kthread_stop() can
   * wake only a live thread and never races the reaper that frees it.
   * Guarded by `lock`. */
  void *native;
  spinlock_t lock;
};

static void kpi_kthread_trampoline(void *arg) {
  struct kpi_kthread *k = arg;

  /* The thread is unquestionably alive here; publish its native handle for
   * kthread_stop().  A stop requested before this point needs no wake (the
   * thread has not started), and after this point native is valid until the
   * epilogue clears it under the same lock. */
  spin_lock(&k->lock);
  k->native = linuxkpi_current_thread();
  spin_unlock(&k->lock);

  k->threadfn(k->data);

  spin_lock(&k->lock);
  k->native = NULL;
  spin_unlock(&k->lock);
  complete(&k->exited);
}

static struct kpi_kthread *kthread_control(struct task_struct *task) {
  struct kpi_kthread *k;

  if (!task)
    return NULL;

  /* Resolve through the shadow, not through the native thread: by the time
   * kthread_stop() runs the thread may already have exited and been reaped
   * (its shadow keeps the control pointer for exactly this reason). */
  k = task->kpi_control;
  if (k && k->magic == KPI_KTHREAD_MAGIC)
    return k;
  return NULL;
}

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
                                           void *data, int node,
                                           const char namefmt[], ...) {
  char name[16];
  va_list args;

  (void)node;

  va_start(args, namefmt);
  vsnprintf(name, sizeof(name), namefmt, args);
  va_end(args);

  struct kpi_kthread *k = kmalloc(sizeof(*k), GFP_KERNEL);
  if (!k)
    return ERR_PTR(-ENOMEM);

  k->magic = KPI_KTHREAD_MAGIC;
  k->threadfn = threadfn;
  k->data = data;
  k->should_stop = 0;
  init_completion(&k->exited);
  k->native = NULL;
  spin_lock_init(&k->lock);

  void *thread = linuxkpi_kthread_create(kpi_kthread_trampoline, k, name);
  if (!thread) {
    kfree(k);
    return ERR_PTR(-ENOMEM);
  }

  /* Hand out the Linux task shadow, not the native pointer: every other
   * task_struct in the system is a shadow too (see linuxkpi/src/task.c).
   * kthread_stop() resolves the control block through this field, so it
   * survives the native thread. */
  struct task_struct *task = linuxkpi_task_for_thread(thread);
  if (task)
    task->kpi_control = k;
  return task;
}

struct task_struct *kthread_run_on_cpu(int (*threadfn)(void *data), void *data,
                                       unsigned int cpu, const char *namefmt) {
  (void)cpu;
  struct task_struct *task = kthread_create(threadfn, data, "%s", namefmt);
  if (!IS_ERR(task))
    wake_up_process(task);
  return task;
}

int kthread_stop(struct task_struct *task) {
  struct kpi_kthread *k = kthread_control(task);
  void *native;

  if (!k)
    return -EINVAL;

  /* Setting the flag and waking the thread happen together under the lock
   * the trampoline uses to retire `native`: from here, either the wake is
   * delivered while the thread is alive or the handle is already NULL
   * because the thread ran its epilogue (and its completion is done). */
  spin_lock(&k->lock);
  k->should_stop = 1;
  native = k->native;
  if (native)
    linuxkpi_wake_thread(native);
  spin_unlock(&k->lock);

  wait_for_completion(&k->exited);
  kfree(k);
  if (task)
    task->kpi_control = NULL;
  return 0;
}

bool kthread_should_stop(void) {
  struct kpi_kthread *k = linuxkpi_thread_self_data();

  return k && k->magic == KPI_KTHREAD_MAGIC && k->should_stop;
}

void kthread_bind(struct task_struct *k, unsigned int cpu) {
  (void)k;
  (void)cpu;
}

/* ── kthread_worker: a FIFO of kthread_work run by one kthread ──────────── */

void kthread_init_work(struct kthread_work *work, kthread_work_func_t func) {
  INIT_LIST_HEAD(&work->node);
  work->func = func;
  work->worker = NULL;
  init_completion(&work->done);
  work->queued = 0;
}

void kthread_init_worker(struct kthread_worker *worker) {
  spin_lock_init(&worker->lock);
  INIT_LIST_HEAD(&worker->work_list);
  worker->current_work = NULL;
  init_waitqueue_head(&worker->wait);
  init_waitqueue_head(&worker->wait_idle);
  worker->should_stop = 0;
  worker->task = NULL;
  worker->name[0] = '\0';
}

static bool kthread_worker_idle(struct kthread_worker *worker) {
  return list_empty(&worker->work_list) && !worker->current_work;
}

/* Create the worker thread for @worker if none is live; called from
 * kthread_queue_work() after the work is linked, under worker->lock so a
 * concurrently retiring worker either sees the work or leaves task NULL. */
static void kthread_worker_spawn(struct kthread_worker *worker) {
  spin_lock(&worker->lock);
  if (!worker->task) {
    struct task_struct *t =
        kthread_create(kthread_worker_fn, worker, "%s", worker->name);
    if (!IS_ERR(t)) {
      worker->task = t;
      wake_up_process(t);
    }
  }
  spin_unlock(&worker->lock);
}

int kthread_worker_fn(void *worker_ptr) {
  struct kthread_worker *worker = worker_ptr;
  struct kthread_work *work;

  for (;;) {
    (void)wait_event_timeout(worker->wait,
                             worker->should_stop ||
                                 !list_empty(&worker->work_list),
                             msecs_to_jiffies(KPI_SERVICE_IDLE_MS));

    spin_lock(&worker->lock);
    if (worker->should_stop && list_empty(&worker->work_list)) {
      worker->task = NULL;
      spin_unlock(&worker->lock);
      break;
    }
    if (list_empty(&worker->work_list)) {
      /* Idle timeout with nothing queued: retire; the next
       * kthread_queue_work() spawns a replacement. */
      worker->task = NULL;
      spin_unlock(&worker->lock);
      return 0;
    }
    work = list_first_entry(&worker->work_list, struct kthread_work, node);
    list_del_init(&work->node);
    worker->current_work = work;
    spin_unlock(&worker->lock);

    work->func(work);

    /* Mark it done before the completion so flush_work() cannot observe a
     * completed completion with queued still set. */
    spin_lock(&worker->lock);
    work->queued = 0;
    worker->current_work = NULL;
    spin_unlock(&worker->lock);
    complete(&work->done);
    wake_up(&worker->wait_idle);
  }
  return 0;
}

bool kthread_queue_work(struct kthread_worker *worker,
                        struct kthread_work *work) {
  bool ret = false;
  bool spawn;

  spin_lock(&worker->lock);
  if (!work->queued) {
    work->queued = 1;
    work->worker = worker;
    reinit_completion(&work->done);
    list_add_tail(&work->node, &worker->work_list);
    ret = true;
  }
  spawn = (worker->task == NULL);
  spin_unlock(&worker->lock);

  if (spawn)
    kthread_worker_spawn(worker);

  if (ret)
    wake_up(&worker->wait);
  return ret;
}

void kthread_flush_work(struct kthread_work *work) {
  if (work->queued)
    wait_for_completion(&work->done);
}

void kthread_flush_worker(struct kthread_worker *worker) {
  wait_event(worker->wait_idle, kthread_worker_idle(worker));
}

bool kthread_cancel_work_sync(struct kthread_work *work) {
  struct kthread_worker *worker = work->worker;
  bool canceled = false;

  if (!worker)
    return false;

  spin_lock(&worker->lock);
  if (work->queued && !list_empty(&work->node)) {
    list_del_init(&work->node);
    work->queued = 0;
    canceled = true;
  }
  spin_unlock(&worker->lock);

  if (!canceled && work->queued)
    wait_for_completion(&work->done);
  return canceled;
}

struct kthread_worker *kthread_create_worker_on_cpu(int cpu, unsigned int flags,
                                                    const char namefmt[], ...) {
  struct kthread_worker *worker;
  char name[16];
  va_list args;

  (void)cpu;
  (void)flags;

  worker = kmalloc(sizeof(*worker), GFP_KERNEL);
  if (!worker)
    return ERR_PTR(-ENOMEM);
  kthread_init_worker(worker);

  va_start(args, namefmt);
  vsnprintf(name, sizeof(name), namefmt, args);
  va_end(args);
  __builtin_strncpy(worker->name, name, sizeof(worker->name) - 1);
  worker->name[sizeof(worker->name) - 1] = '\0';

  /* No thread until the first kthread_queue_work(): an unused worker costs
   * nothing, and an idle one retires after KPI_SERVICE_IDLE_MS. */
  return worker;
}

struct kthread_worker *kthread_create_worker(unsigned int flags,
                                             const char namefmt[], ...) {
  struct kthread_worker *worker;
  char name[16];
  va_list args;

  (void)flags;

  worker = kmalloc(sizeof(*worker), GFP_KERNEL);
  if (!worker)
    return ERR_PTR(-ENOMEM);
  kthread_init_worker(worker);

  va_start(args, namefmt);
  vsnprintf(name, sizeof(name), namefmt, args);
  va_end(args);
  __builtin_strncpy(worker->name, name, sizeof(worker->name) - 1);
  worker->name[sizeof(worker->name) - 1] = '\0';

  return worker;
}

void kthread_destroy_worker(struct kthread_worker *worker) {
  struct task_struct *task;

  if (!worker)
    return;

  /* Stop first and detach the task so a concurrent queue cannot mistake the
   * retiring worker for a live one. */
  spin_lock(&worker->lock);
  worker->should_stop = 1;
  task = worker->task;
  worker->task = NULL;
  spin_unlock(&worker->lock);

  if (task)
    kthread_stop(task);
  kfree(worker);
}

int kthread_park(struct task_struct *k) {
  (void)k;
  return 0;
}

void kthread_unpark(struct task_struct *k) { (void)k; }

bool kthread_should_park(void) { return false; }

int kthread_parkme(void) { return 0; }
