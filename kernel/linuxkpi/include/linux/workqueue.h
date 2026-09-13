#ifndef __AVORY_LINUXKPI_WORKQUEUE_H
#define __AVORY_LINUXKPI_WORKQUEUE_H

/* Linux <linux/workqueue.h> overlay.
 *
 * Workqueues are served by kernel threads created with the LinuxKPI kthread
 * API.  Guarantees implemented here:
 *   - a work item queued while pending is not queued twice;
 *   - the same work item never runs concurrently with itself;
 *   - flush_work()/flush_workqueue() wait for queued and running work;
 *   - delayed work is driven by the LinuxKPI timer wheel.
 * Concurrency-managed pools, CPU affinity and WQ_* policy are accepted but
 * not yet honored (see docs/linuxkpi-progress.md). */

#include <linux/completion.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/types.h>

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
  struct list_head entry;
  work_func_t func;
  volatile int pending;
  volatile int running;
  struct workqueue_struct *wq;
};

#define work_pending(work) ((work)->pending)
#define work_data_bits(work) (0UL)

#define INIT_WORK(_work, _func)                                               \
  do {                                                                        \
    INIT_LIST_HEAD(&(_work)->entry);                                          \
    (_work)->func = (_func);                                                  \
    (_work)->pending = 0;                                                     \
    (_work)->running = 0;                                                     \
    (_work)->wq = NULL;                                                       \
  } while (0)

#define DECLARE_WORK(n, f)                                                    \
  struct work_struct n = {{NULL, NULL}, (f), 0, 0, NULL}

/* On-stack work: the native first-party implementation has no stack-tracking
 * machinery, so these are the plain macros plus no-op destroy hooks. */
#define INIT_WORK_ONSTACK(_work, _func) INIT_WORK(_work, _func)
#define destroy_work_on_stack(work) do { (void)(work); } while (0)

struct workqueue_struct;

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_power_efficient_wq;
extern struct workqueue_struct *system_freezable_wq;

bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_work_on(int cpu, struct workqueue_struct *wq,
                   struct work_struct *work);
bool schedule_work(struct work_struct *work);
bool schedule_work_on(int cpu, struct work_struct *work);

bool flush_work(struct work_struct *work);
void flush_workqueue(struct workqueue_struct *wq);
/* Upstream drain_workqueue() also blocks new submissions; this build has no
 * workqueue "draining" state, so it is equivalent to flush_workqueue(). */
void drain_workqueue(struct workqueue_struct *wq);
bool cancel_work_sync(struct work_struct *work);

/* Upstream returns the work item the current task is running (for
 * concurrency-managed pools).  AvoryOS workers are plain kthreads, so this is
 * always NULL; callers only compare it against a work pointer. */
static inline struct work_struct *current_work(void) { return NULL; }

struct delayed_work {
  struct work_struct work;
  struct timer_list timer;
};

void delayed_work_timer_fn(struct timer_list *t);

static inline struct delayed_work *to_delayed_work(struct work_struct *work) {
  return container_of(work, struct delayed_work, work);
}

#define INIT_DELAYED_WORK(_work, _func)                                       \
  do {                                                                        \
    INIT_WORK(&(_work)->work, (_func));                                       \
    timer_setup(&(_work)->timer, delayed_work_timer_fn, 0);                   \
  } while (0)
#define INIT_DEFERRABLE_WORK(_work, _func) INIT_DELAYED_WORK(_work, _func)
#define INIT_DELAYED_WORK_ONSTACK(_work, _func) INIT_DELAYED_WORK(_work, _func)
#define destroy_delayed_work_on_stack(dwork) do { (void)(dwork); } while (0)

bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay);
bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *dwork, unsigned long delay);
bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
                      unsigned long delay);

/* Additions used by amdgpu (P6 C2). */
bool flush_delayed_work(struct delayed_work *dwork);
static inline bool cancel_work(struct work_struct *work) {
  return cancel_work_sync(work);
}
#define __DELAYED_WORK_INITIALIZER(n, f, t)                                   \
  {                                                                           \
    .work = {.entry = {&(n).work.entry, &(n).work.entry}, .func = (f)},        \
    .timer = {.entry = {&(n).timer.entry, &(n).timer.entry},                   \
              .function = delayed_work_timer_fn},                              \
  }
#define create_singlethread_workqueue(name) alloc_ordered_workqueue(name, 0)
#define create_workqueue(name) alloc_workqueue(name, 0, 0)
bool cancel_delayed_work(struct delayed_work *dwork);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
bool schedule_delayed_work_on(int cpu, struct delayed_work *dwork,
                              unsigned long delay);
#define queue_delayed_work_on(cpu, wq, dwork, delay)                          \
  queue_delayed_work((wq), (dwork), (delay))

#define WQ_MEM_RECLAIM 0x00000001
#define WQ_UNBOUND 0x00000002
#define WQ_FREEZABLE 0x00000004
#define WQ_HIGHPRI 0x00000008
#define WQ_CPU_INTENSIVE 0x00000010
#define WQ_POWER_EFFICIENT 0x00000020
#define WQ_SYSFS 0x00000040
#define WQ_NAME_LEN 24

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags,
                                         int max_active, ...);
#define alloc_ordered_workqueue(fmt, flags, args...)                          \
  alloc_workqueue(fmt, (flags), 1, ##args)
void destroy_workqueue(struct workqueue_struct *wq);

/* Bring-up: create system workqueues and their worker threads. */
void linuxkpi_workqueue_init(void);

#endif /* __AVORY_LINUXKPI_WORKQUEUE_H */
