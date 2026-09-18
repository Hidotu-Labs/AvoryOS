/* Per-thread Linux task_struct shadow.
 *
 * `current` (and task_struct pointers handed out by kthread_create()) point at
 * one of these.  The objects are allocated lazily by native_sched.c through
 * the weak linuxkpi_task_shadow_new() hook.  The native exit path
 * (linuxkpi_thread_exiting()) clears kpi_thread so stale handles cannot reach
 * the freed native thread, but the shadow object itself is retained: a
 * kthread control block lives in it (kpi_control) and kthread_stop() may run
 * after the thread has been reaped.  Shadows with no control block (user
 * threads, plain kernel threads) are still leaked with their thread; freeing
 * them needs reference counting against imported code that stores task
 * pointers, recorded in docs/linuxkpi-gaps.md. */

#include <linux/sched.h>
#include <linux/slab.h>

#include <linuxkpi/native_sched.h>

void *linuxkpi_task_shadow_new(void *thread) {
  struct task_struct *tsk;

  tsk = kzalloc(sizeof(*tsk), GFP_KERNEL);
  if (!tsk)
    return NULL;

  tsk->kpi_thread = thread;
  tsk->state = TASK_RUNNING;
  tsk->pid = (pid_t)linuxkpi_thread_pid(thread);
  tsk->tgid = (pid_t)linuxkpi_thread_tgid(thread);
  linuxkpi_thread_comm(thread, tsk->comm, sizeof(tsk->comm));
  /* Kernel-thread shadows are not refcounted yet; start at 1 so imported
   * refcount helpers (get_task_struct/put_task_struct) cannot underflow. */
  refcount_set(&tsk->usage, 1);
  /* No shared signal_struct yet: each thread is its own group leader.  The
   * only consumer is drm_sched's per-user submission tracking, which keys on
   * group_leader identity and compares it against itself. */
  tsk->group_leader = tsk;
  return tsk;
}
