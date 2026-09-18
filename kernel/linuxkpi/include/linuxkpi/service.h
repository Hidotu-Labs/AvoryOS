#ifndef LINUXKPI_SERVICE_H
#define LINUXKPI_SERVICE_H

/* Idle-exit helper for LinuxKPI daemon threads.
 *
 * These daemons (ktimers, rcu_kpi, irq_work) used to stay alive for the whole
 * boot even when their queues were empty.  A service thread now retires after
 * KPI_SERVICE_IDLE_MS of inactivity and is respawned by the next producer.
 *
 * Protocol:
 *   producer:  publish the work under its own lock, then call
 *              kpi_service_kick(svc, true).  kick bumps the generation and
 *              either wakes the live thread or creates a new one.
 *   worker:    sample kpi_service_gen(svc) before waiting; when the wait
 *              times out, call kpi_service_retire(svc, sampled_gen).  A true
 *              return means no work has been published since the sample and
 *              the task pointer was cleared, so the threadfn must return.
 *
 * The generation is bumped before the task check, and the retire check runs
 * under the same lock as that check: either the retire wins and the next kick
 * spawns a replacement, or the kick wins and the worker observes the new
 * generation and keeps running.  Never a stranded queue. */

#define KPI_SERVICE_IDLE_MS 5000

struct task_struct;

struct kpi_service {
  const char *name;
  int (*fn)(void *arg);
  void *arg;
  struct task_struct *task;
  spinlock_t lock;
  unsigned int gen;
};

void kpi_service_init(struct kpi_service *svc, const char *name,
                      int (*fn)(void *), void *arg);

/* Publish side: bump the generation when @bump and make sure a thread runs. */
void kpi_service_kick(struct kpi_service *svc, bool bump);

/* Worker side: sampled generation, checked after an idle timeout.  Returns
 * true when the service retired (the caller must return from its threadfn). */
bool kpi_service_retire(struct kpi_service *svc, unsigned int seen_gen);

/* Clear the task pointer unconditionally (stop path); does not wait. */
void kpi_service_forget(struct kpi_service *svc);

static inline unsigned int kpi_service_gen(struct kpi_service *svc) {
  return __atomic_load_n(&svc->gen, __ATOMIC_ACQUIRE);
}

#endif /* LINUXKPI_SERVICE_H */
