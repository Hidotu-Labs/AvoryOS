#ifndef __AVORY_LINUXKPI_SCHED_H
#define __AVORY_LINUXKPI_SCHED_H

/* Minimal Linux <linux/sched.h> overlay.
 *
 * `current` is an opaque handle: it is the native thread pointer cast to
 * struct task_struct*.  This is enough for sleeping/waking and for passing
 * "the current task" around; fields (comm/pid) arrive with the real task
 * model.  The upstream header cannot be used here yet (per-CPU/thread_info,
 * scheduling classes, mm). */

#include <linux/capability.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/preempt.h>
#include <linux/rbtree.h>
#include <linux/refcount.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <asm/vdso/processor.h>
#include <asm/processor.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/signal.h>

#ifndef NUMA_NO_NODE
#define NUMA_NO_NODE (-1)
#endif

#include <linuxkpi/native_sched.h>

struct pid;
struct mm_struct;
/* File-scope declaration so headers that mention `struct seq_file *` in a
 * prototype (e.g. dma-fence.h) do not create a prototype-scoped tag. */
struct seq_file;

/* Upstream defines this as an enum before struct task_struct. */
#define TASK_COMM_LEN 16

/* Linux-facing per-thread task.  AvoryOS allocates one lazily for every
 * native thread (linuxkpi/src/task.c) and `current` points at it.  The first
 * field MUST be the native thread pointer: native_sched.c reads it without
 * knowing this layout.  Keep the struct small; add fields only when imported
 * code uses them. */
struct task_struct {
  void *kpi_thread; /* native `struct thread *` (must stay first) */
  volatile long state;
  unsigned int flags;
  pid_t pid;  /* native tid */
  pid_t tgid; /* native tgid */
  char comm[TASK_COMM_LEN];
  /* Scheduler (drm_sched) reads these on submission/exit paths.  Because
   * AvoryOS has no shared signal_struct yet, every thread is its own group
   * leader and exit_code stays 0 unless a native exit hook sets it. */
  struct task_struct *group_leader;
  int exit_code;
  /* Fields stock <linux/sched/task.h> touches: the RCU free head and the
   * arch thread block (x86 thread_struct). */
  struct rcu_head rcu;
  struct thread_struct thread;
  /* mm/oom/reclaim surface amdgpu touches (amdgpu_gem.c reads current->mm).
   * There is no Linux mm, so mm/active_mm stay NULL and reclaim_state is a
   * placeholder; usage is initialized so refcount helpers do not underflow. */
  struct mm_struct *mm;
  struct mm_struct *active_mm;
  struct reclaim_state *reclaim_state;
  refcount_t usage;
  spinlock_t alloc_lock;
  /* LinuxKPI kernel-thread control block (linuxkpi/src/kthread.c) for
   * kthread_create()'d threads, else NULL.  It lives in the shadow so
   * kthread_stop() can still resolve it after the native thread exited and
   * its kpi_thread back-pointer was dropped (see linuxkpi_thread_exiting). */
  void *kpi_control;
};

/* Process flags stock headers reference; only the one amdgpu uses is defined
 * (upstream sched.h has the full PF_* set). */
#define PF_KSWAPD 0x00040000

/* Upstream sched.h: can this context sleep?  preempt_count() is the whole
 * story here (no irqs_disabled() tracking in this predicate). */
static inline int preemptible(void) { return preempt_count() == 0; }

#define current ((struct task_struct *)linuxkpi_current_task())

static inline struct task_struct *task_struct_from_thread(void *thread) {
  return (struct task_struct *)linuxkpi_task_for_thread(thread);
}

static inline void *task_struct_to_thread(const struct task_struct *p) {
  return p ? p->kpi_thread : NULL;
}

/* PID bridge: AvoryOS threads carry a tgid, but there is no struct pid.  The
 * DRM core only compares pids (file->pid vs current) and stores/puts them, so
 * task_tgid() returns an opaque token built from the shadow's tgid. */
static inline struct pid *task_tgid(const struct task_struct *p) {
  return (struct pid *)(unsigned long)((p ? p->tgid : 0) + 1);
}

static inline pid_t task_pid_nr(const struct task_struct *p) {
  return p ? p->pid : 0;
}
static inline pid_t task_tgid_nr(const struct task_struct *p) {
  return p ? p->tgid : 0;
}
static inline pid_t task_pid_vnr(const struct task_struct *p) {
  return task_pid_nr(p);
}
static inline pid_t task_tgid_vnr(const struct task_struct *p) {
  return task_tgid_nr(p);
}
static inline void get_task_comm(char *buf, struct task_struct *p) {
  const char *src = p ? p->comm : "";
  unsigned int i;

  for (i = 0; i < TASK_COMM_LEN - 1 && src[i]; i++)
    buf[i] = src[i];
  buf[i] = '\0';
}


#define TASK_RUNNING 0x00000000
#define TASK_INTERRUPTIBLE 0x00000001
#define TASK_UNINTERRUPTIBLE 0x00000002
#define TASK_NORMAL (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
#define TASK_STOPPED 0x00000004
#define TASK_TRACED 0x00000008
#define TASK_WAKEKILL 0x00000080
#define TASK_KILLABLE (TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)

/* Per-process flags (upstream values).  These are informational here: the
 * native thread has its own state and kind fields, and only code that tests
 * current->flags would care (vtime, which is compiled out). */
#define PF_VCPU 0x00000001
#define PF_IDLE 0x00000002
#define PF_EXITING 0x00000004
#define PF_POSTCOREDUMP 0x00000008
#define PF_IO_WORKER 0x00000010
#define PF_WQ_WORKER 0x00000020
#define PF_FORKNOEXEC 0x00000040
#define PF_MCE_PROCESS 0x00000080
#define PF_SUPERPRIV 0x00000100
#define PF_DUMPCORE 0x00000200
#define PF_SIGNALED 0x00000400
#define PF_MEMALLOC 0x00000800
#define PF_NPROC_EXCEEDED 0x00001000
#define PF_USED_MATH 0x00002000
#define PF_USER_WORKER 0x00004000
#define PF_NOFREEZE 0x00008000
#define PF_KTHREAD 0x00200000
#define PF_RANDOMIZE 0x00400000
#define PF_NO_SETAFFINITY 0x04000000
#define PF_SUSPEND_TASK 0x80000000

#define MAX_SCHEDULE_TIMEOUT LONG_MAX

/* State changes are implicit in the sleep helpers (the task model is not
 * exported yet); keep the API shape so upstream code compiles. */
#define set_current_state(state_value)                                        \
  do {                                                                        \
    (void)(state_value);                                                      \
  } while (0)
#define __set_current_state(state_value) set_current_state(state_value)

void schedule(void);
long schedule_timeout(long timeout);
long schedule_timeout_interruptible(long timeout);
long schedule_timeout_uninterruptible(long timeout);
long schedule_timeout_killable(long timeout);

int wake_up_process(struct task_struct *p);
bool signal_pending(struct task_struct *p);
void yield(void);

/* Priority-class helpers: the native scheduler has no scheduling classes, so
 * these are advisory no-ops (documented divergence). */
static inline void sched_set_fifo(struct task_struct *p) { (void)p; }
static inline void sched_set_fifo_low(struct task_struct *p) { (void)p; }
static inline void sched_set_normal(struct task_struct *p, int nice) {
  (void)p;
  (void)nice;
}
static inline int sched_setattr_nocheck(struct task_struct *p, void *attr) {
  (void)p;
  (void)attr;
  return 0;
}

#endif /* __AVORY_LINUXKPI_SCHED_H */
