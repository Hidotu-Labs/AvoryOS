/* Per-thread Linux task shadow. The native thread owns its initial reference.
 * Imported get_task_struct users and joined kthreads hold separate references;
 * native exit detaches the back-pointer and drops only the native reference.
 * The last holder releases the shadow and its kthread control block. */

#include <linux/sched.h>
#include <linux/slab.h>

#include <linuxkpi/native_sched.h>

struct task_struct *pid_task(struct pid *pid, enum pid_type type) {
  struct task_struct *task = current;
  pid_t nr = pid_nr(pid);
  /* There is no native RCU task index yet. Report the current task when
   * its identity matches, and let diagnostic callers print "Unset" for
   * unresolvable tokens rather than attributing them to another process. */
  if (!pid || !task)
    return NULL;
  if ((type == PIDTYPE_PID && task->pid == nr) ||
      (type == PIDTYPE_TGID && task->tgid == nr))
    return task;
  return NULL;
}

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
  /* Initial reference belongs to the native thread. */
  refcount_set(&tsk->usage, 1);
  /* No shared signal_struct yet: each thread is its own group leader.  The
   * only consumer is drm_sched's per-user submission tracking, which keys on
   * group_leader identity and compares it against itself. */
  tsk->group_leader = tsk;
  return tsk;
}

void __put_task_struct(struct task_struct *tsk) {
  /* No native thread can still use the completion/control block at the
   * final put: reaping drops its reference only after it is off-CPU. */
  kfree(tsk->kpi_control);
  kfree(tsk);
}

void __put_task_struct_rcu_cb(struct rcu_head *head) {
  __put_task_struct(container_of(head, struct task_struct, rcu));
}

struct task_struct *linuxkpi_task_get(struct task_struct *tsk) {
  if (tsk)
    refcount_inc(&tsk->usage);
  return tsk;
}

void linuxkpi_task_put(struct task_struct *tsk) {
  if (tsk && refcount_dec_and_test(&tsk->usage))
    __put_task_struct(tsk);
}

void linuxkpi_task_shadow_release(void *shadow) {
  struct task_struct *tsk = shadow;
  WRITE_ONCE(tsk->kpi_thread, NULL);
  linuxkpi_task_put(tsk);
}
