/* Idle-exit services for LinuxKPI daemon threads; see service.h. */

#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#include <linuxkpi/service.h>

void kpi_service_init(struct kpi_service *svc, const char *name,
                      int (*fn)(void *), void *arg) {
  svc->name = name;
  svc->fn = fn;
  svc->arg = arg;
  svc->task = NULL;
  svc->gen = 0;
  spin_lock_init(&svc->lock);
}

void kpi_service_kick(struct kpi_service *svc, bool bump) {
  struct task_struct *task;

  if (bump)
    __atomic_add_fetch(&svc->gen, 1, __ATOMIC_RELEASE);

  spin_lock(&svc->lock);
  task = svc->task;
  if (!task) {
    /* Creation under the lock: a racing retire clears svc->task under the
     * same lock, so it either sees this thread or this kick sees NULL. */
    task = kthread_create(svc->fn, svc->arg, "%s", svc->name);
    if (IS_ERR(task))
      task = NULL;
    else
      svc->task = task;
  }
  if (task)
    wake_up_process(task);
  spin_unlock(&svc->lock);
}

bool kpi_service_retire(struct kpi_service *svc, unsigned int seen_gen) {
  bool retire;

  spin_lock(&svc->lock);
  retire = kpi_service_gen(svc) == seen_gen;
  if (retire)
    svc->task = NULL;
  spin_unlock(&svc->lock);
  return retire;
}

void kpi_service_forget(struct kpi_service *svc) {
  spin_lock(&svc->lock);
  svc->task = NULL;
  spin_unlock(&svc->lock);
}
