/* LinuxKPI kernel threads.  See linux/kthread.h and the native bridge
 * linuxkpi/native_sched.h.
 *
 * Each thread carries a control block (magic-tagged, stored in the native
 * thread's kpi_data) with the Linux threadfn/data and the cooperative stop
 * flag.  Blocks are freed by kthread_stop(); threads that are never stopped
 * keep their (small) control block for the thread's lifetime. */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stdarg.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_sched.h>

#define KPI_KTHREAD_MAGIC 0x4b544852u /* "KTHR" */

struct kpi_kthread {
  unsigned int magic;
  int (*threadfn)(void *data);
  void *data;
  volatile int should_stop;
  struct completion exited;
};

static void kpi_kthread_trampoline(void *arg) {
  struct kpi_kthread *k = arg;

  k->threadfn(k->data);
  complete(&k->exited);
}

static struct kpi_kthread *kthread_control(struct task_struct *task) {
  struct kpi_kthread *k = linuxkpi_thread_data(task_struct_to_thread(task));

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

  void *thread = linuxkpi_kthread_create(kpi_kthread_trampoline, k, name);
  if (!thread) {
    kfree(k);
    return ERR_PTR(-ENOMEM);
  }

  /* Hand out the Linux task shadow, not the native pointer: every other
   * task_struct in the system is a shadow too (see linuxkpi/src/task.c). */
  return (struct task_struct *)linuxkpi_task_for_thread(thread);
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

  if (!k)
    return -EINVAL;

  k->should_stop = 1;
  linuxkpi_wake_thread(task_struct_to_thread(task));

  wait_for_completion(&k->exited);
  kfree(k);
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
}

static bool kthread_worker_idle(struct kthread_worker *worker) {
  return list_empty(&worker->work_list) && !worker->current_work;
}

int kthread_worker_fn(void *worker_ptr) {
  struct kthread_worker *worker = worker_ptr;
  struct kthread_work *work;

  for (;;) {
    wait_event(worker->wait,
               worker->should_stop || !list_empty(&worker->work_list));

    spin_lock(&worker->lock);
    if (worker->should_stop && list_empty(&worker->work_list)) {
      spin_unlock(&worker->lock);
      break;
    }
    if (list_empty(&worker->work_list)) {
      spin_unlock(&worker->lock);
      continue;
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

  spin_lock(&worker->lock);
  if (!work->queued) {
    work->queued = 1;
    work->worker = worker;
    reinit_completion(&work->done);
    list_add_tail(&work->node, &worker->work_list);
    ret = true;
  }
  spin_unlock(&worker->lock);

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
  struct task_struct *task;
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

  task = kthread_create(kthread_worker_fn, worker, "%s", name);
  if (IS_ERR(task)) {
    kfree(worker);
    return ERR_CAST(task);
  }
  worker->task = task;
  wake_up_process(task);
  return worker;
}

struct kthread_worker *kthread_create_worker(unsigned int flags,
                                             const char namefmt[], ...) {
  struct kthread_worker *worker;
  struct task_struct *task;
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

  task = kthread_create(kthread_worker_fn, worker, "%s", name);
  if (IS_ERR(task)) {
    kfree(worker);
    return ERR_CAST(task);
  }
  worker->task = task;
  wake_up_process(task);
  return worker;
}

void kthread_destroy_worker(struct kthread_worker *worker) {
  if (!worker)
    return;
  if (worker->task) {
    kthread_flush_worker(worker);
    spin_lock(&worker->lock);
    worker->should_stop = 1;
    spin_unlock(&worker->lock);
    kthread_stop(worker->task);
  }
  kfree(worker);
}

int kthread_park(struct task_struct *k) {
  (void)k;
  return 0;
}

void kthread_unpark(struct task_struct *k) { (void)k; }

bool kthread_should_park(void) { return false; }

int kthread_parkme(void) { return 0; }
