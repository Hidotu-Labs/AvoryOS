#ifndef __AVORY_LINUXKPI_KTHREAD_H
#define __AVORY_LINUXKPI_KTHREAD_H

/* Linux <linux/kthread.h> overlay.  Kernel threads are native scheduler
 * threads created through linuxkpi/native_sched.h; each carries a control
 * block holding the Linux threadfn/data and the cooperative stop flag.
 *
 * The kthread_worker/kthread_work FIFO is implemented on top of those
 * threads (linuxkpi/src/kthread.c): one worker thread runs one work item at
 * a time, flush_work()/flush_worker() wait for completion, cancel_work_sync()
 * removes a pending item or waits for a running one. */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#define KTHREAD_NODE_ANY (-1)

struct kthread_worker;
struct kthread_work;
typedef void (*kthread_work_func_t)(struct kthread_work *work);

struct kthread_work {
  struct list_head node;
  kthread_work_func_t func;
  struct kthread_worker *worker;
  struct completion done; /* signaled after each run */
  int queued;             /* internal: on a worker list or running */
};

struct kthread_worker {
  struct task_struct *task;
  spinlock_t lock;
  struct list_head work_list;
  struct kthread_work *current_work;
  wait_queue_head_t wait;      /* worker sleeps here when idle */
  wait_queue_head_t wait_idle; /* flush_worker() waits for idleness */
  int should_stop;
};

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
                                           void *data, int node,
                                           const char namefmt[], ...);

#define kthread_create(threadfn, data, namefmt, arg...)                       \
  kthread_create_on_node(threadfn, data, KTHREAD_NODE_ANY, namefmt, ##arg)

#define kthread_run(threadfn, data, namefmt, ...)                             \
  ({                                                                          \
    struct task_struct *__k =                                                 \
        kthread_create(threadfn, data, namefmt, ##__VA_ARGS__);               \
    if (!IS_ERR(__k))                                                         \
      wake_up_process(__k);                                                   \
    __k;                                                                      \
  })

struct task_struct *kthread_run_on_cpu(int (*threadfn)(void *data), void *data,
                                       unsigned int cpu, const char *namefmt);

int kthread_stop(struct task_struct *k);
bool kthread_should_stop(void);
void kthread_bind(struct task_struct *k, unsigned int cpu);

void kthread_init_work(struct kthread_work *work, kthread_work_func_t func);
void kthread_init_worker(struct kthread_worker *worker);
struct kthread_worker *kthread_create_worker(unsigned int flags,
                                             const char namefmt[], ...);
struct kthread_worker *kthread_create_worker_on_cpu(int cpu, unsigned int flags,
                                                    const char namefmt[], ...);
bool kthread_queue_work(struct kthread_worker *worker,
                        struct kthread_work *work);
void kthread_flush_work(struct kthread_work *work);
void kthread_flush_worker(struct kthread_worker *worker);
bool kthread_cancel_work_sync(struct kthread_work *work);
void kthread_destroy_worker(struct kthread_worker *worker);
int kthread_worker_fn(void *worker_ptr);

/* And the worker can park itself; no-ops until the scheduler exposes it. */
int kthread_park(struct task_struct *k);
void kthread_unpark(struct task_struct *k);
bool kthread_should_park(void);
int kthread_parkme(void);

#define KTHREAD_WORKER_FREEZABLE 0x0

#endif /* __AVORY_LINUXKPI_KTHREAD_H */
