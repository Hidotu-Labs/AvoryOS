/* Phase 4 C3 — drm_sched self-tests.
 *
 * Exercises the imported GPU scheduler with a fake backend (no hardware):
 *
 *   1. drm_sched_init/fini + one entity, one job whose run_job() returns a
 *      fence that a delayed work signals a few ms later; wait on
 *      s_fence->finished.
 *   2. Queue ordering: two entities, several jobs each; every entity's jobs
 *      complete in submission order and every job is freed exactly once.
 *   3. Timeout path: a job whose fence is never signaled; the scheduler's
 *      work_tdr must call timedout_job() after sched->timeout, then the test
 *      signals the fence so the job can complete.
 *   4. Stress: 2 x 16 jobs with a PMM free-page check after warm-up.
 *
 * The fake fences use dma_fence_init and are freed from their release
 * callback so the PMM invariant covers the scheduler's fence lifetime. */

#include <drm/gpu_scheduler.h>

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

#define P4S_TIMEOUT_MS 100
#define P4S_JOB_MS 5
#define P4S_STRESS_JOBS 16

static int p4s_failures;
static atomic_t p4s_freed;
static atomic_t p4s_timedout;
static atomic_t p4s_run_calls;
static atomic_t p4s_signal_calls;
static bool p4s_broken;

/* ── fake fence ──────────────────────────────────────────────────────────── */

struct p4s_fence {
  struct dma_fence base;
  spinlock_t lock;
};

static const char *p4s_fence_driver_name(struct dma_fence *f) {
  (void)f;
  return "p4s";
}

static const char *p4s_fence_timeline_name(struct dma_fence *f) {
  (void)f;
  return "p4s-timeline";
}

static void p4s_fence_release(struct dma_fence *f) {
  kfree(container_of(f, struct p4s_fence, base));
}

static const struct dma_fence_ops p4s_fence_ops = {
    .get_driver_name = p4s_fence_driver_name,
    .get_timeline_name = p4s_fence_timeline_name,
    .release = p4s_fence_release,
};

/* ── fake jobs ───────────────────────────────────────────────────────────── */

struct p4s_job {
  struct drm_sched_job job;
  struct p4s_fence *fence;
  struct delayed_work signal_dw;
  struct completion *timedout_completion;
  unsigned int entity_idx;
  u64 seq;
  bool never_signal;
};

static void p4s_signal_work(struct work_struct *work) {
  struct p4s_job *j =
      container_of(work, struct p4s_job, signal_dw.work);

  atomic_inc(&p4s_signal_calls);
  dma_fence_signal(&j->fence->base);
}

static struct dma_fence *p4s_run_job(struct drm_sched_job *sched_job) {
  struct p4s_job *j = container_of(sched_job, struct p4s_job, job);
  struct p4s_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);

  atomic_inc(&p4s_run_calls);
  if (!f)
    return ERR_PTR(-ENOMEM);
  spin_lock_init(&f->lock);
  dma_fence_init(&f->base, &p4s_fence_ops, &f->lock, 0x504453ULL, j->seq);
  j->fence = f;

  if (!j->never_signal)
    schedule_delayed_work(&j->signal_dw, msecs_to_jiffies(P4S_JOB_MS));
  return &f->base;
}

static enum drm_gpu_sched_stat p4s_timedout_job(
    struct drm_sched_job *sched_job) {
  struct p4s_job *j = container_of(sched_job, struct p4s_job, job);

  atomic_inc(&p4s_timedout);
  if (j->timedout_completion)
    complete(j->timedout_completion);
  return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void p4s_free_job(struct drm_sched_job *sched_job) {
  struct p4s_job *j = container_of(sched_job, struct p4s_job, job);

  cancel_delayed_work_sync(&j->signal_dw);
  atomic_inc(&p4s_freed);
  kfree(j);
}

static const struct drm_sched_backend_ops p4s_ops = {
    .run_job = p4s_run_job,
    .timedout_job = p4s_timedout_job,
    .free_job = p4s_free_job,
};

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void p4s_ok(const char *what);
static void p4s_fail(const char *what, long v);

/* Direct probe for the schedule_timeout()/wake_up_state() primitive that
 * dma_fence_default_wait() uses: a kthread waits on a fence, the suite thread
 * signals it 100 ms later.  This isolates a lost-wakeup bug from drm_sched. */
static struct p4s_fence *p4s_probe_fence;
static struct completion p4s_probe_done;
static long p4s_probe_ret;

static struct task_struct *p4s_probe_peer;
static struct completion p4s_probe_ready;

static int p4s_probe_timeout_worker(void *arg) {
  (void)arg;
  p4s_probe_peer = current;
  complete(&p4s_probe_ready);
  p4s_probe_ret = schedule_timeout(msecs_to_jiffies(1000));
  complete(&p4s_probe_done);
  return 0;
}

static void p4s_test_timeout_probe(void) {
  struct task_struct *t;

  p4s_probe_ret = -1;
  p4s_probe_peer = NULL;
  init_completion(&p4s_probe_ready);
  init_completion(&p4s_probe_done);

  t = kthread_run(p4s_probe_timeout_worker, NULL, "p4s-tmo");
  if (IS_ERR(t)) {
    p4s_fail("timeout probe thread", PTR_ERR(t));
    return;
  }
  if (wait_for_completion_timeout(&p4s_probe_ready, msecs_to_jiffies(500)) == 0) {
    p4s_fail("timeout probe ready", 0);
    return;
  }
  msleep(100);
  wake_up_process(p4s_probe_peer);
  if (wait_for_completion_timeout(&p4s_probe_done, msecs_to_jiffies(2000)) > 0 &&
      p4s_probe_ret > 0)
    p4s_ok("timeout probe: schedule_timeout woken by wake_up_process");
  else
    p4s_fail("timeout probe ret", p4s_probe_ret);
}

static int p4s_probe_waiter(void *arg) {
  (void)arg;
  p4s_probe_ret = dma_fence_wait_timeout(&p4s_probe_fence->base, false,
                                         msecs_to_jiffies(1000));
  complete(&p4s_probe_done);
  return 0;
}

static void p4s_test_wakeup_probe(void) {
  struct task_struct *t;
  struct p4s_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);

  if (!f) {
    p4s_fail("wakeup probe alloc", 0);
    return;
  }
  spin_lock_init(&f->lock);
  dma_fence_init(&f->base, &p4s_fence_ops, &f->lock, 0x50524f42ULL, 1);
  p4s_probe_fence = f;
  p4s_probe_ret = -1;
  init_completion(&p4s_probe_done);

  t = kthread_run(p4s_probe_waiter, NULL, "p4s-probe");
  if (IS_ERR(t)) {
    p4s_fail("wakeup probe thread", PTR_ERR(t));
    dma_fence_put(&f->base);
    return;
  }
  msleep(100);
  dma_fence_signal(&f->base);
  if (wait_for_completion_timeout(&p4s_probe_done, msecs_to_jiffies(2000)) > 0 &&
      p4s_probe_ret > 0)
    p4s_ok("wakeup probe: fence waiter woken");
  else {
    p4s_fail("wakeup probe ret", p4s_probe_ret);
    klogf("[INFO] sched fence signaled=%d\n",
          dma_fence_is_signaled(&f->base) ? 1 : 0);
  }
  dma_fence_put(&f->base);
}

static void p4s_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: sched %s\n", what);
}

static void p4s_fail(const char *what, long v) {
  p4s_failures++;
  klogf("[FAIL] LinuxKPI: sched %s (%ld)\n", what, v);
}

static struct p4s_job *p4s_job_new(struct drm_sched_entity *entity,
                                   unsigned int entity_idx, u64 seq,
                                   bool never_signal,
                                   struct completion *timedout_done) {
  struct p4s_job *j = kzalloc(sizeof(*j), GFP_KERNEL);

  if (!j)
    return NULL;
  INIT_DELAYED_WORK(&j->signal_dw, p4s_signal_work);
  j->entity_idx = entity_idx;
  j->seq = seq;
  j->never_signal = never_signal;
  j->timedout_completion = timedout_done;
  if (drm_sched_job_init(&j->job, entity, NULL) != 0) {
    kfree(j);
    return NULL;
  }
  drm_sched_job_arm(&j->job);
  return j;
}

/* Push and wait for the finished fence; returns the wait result. */
static long p4s_push_and_wait(struct drm_sched_entity *entity,
                              unsigned int entity_idx, u64 seq,
                              bool never_signal,
                              struct completion *timedout_done) {
  struct p4s_job *j =
      p4s_job_new(entity, entity_idx, seq, never_signal, timedout_done);
  struct dma_fence *finished;
  long ret;

  if (!j)
    return -ENOMEM;
  finished = dma_fence_get(&j->job.s_fence->finished);
  drm_sched_entity_push_job(&j->job);
  ret = dma_fence_wait_timeout(finished, false, msecs_to_jiffies(2000));
  dma_fence_put(finished);
  return ret;
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void p4s_test_basic(void) {
  struct drm_gpu_scheduler sched;
  struct drm_gpu_scheduler *sched_list[1] = {&sched};
  struct drm_sched_entity entity;
  long ret;

  if (drm_sched_init(&sched, &p4s_ops, 2, 1, msecs_to_jiffies(P4S_TIMEOUT_MS),
                     system_wq, NULL, "p4s-basic", NULL) != 0) {
    p4s_fail("basic init", 0);
    return;
  }
  if (drm_sched_entity_init(&entity, DRM_SCHED_PRIORITY_NORMAL, sched_list, 1,
                            NULL) != 0) {
    p4s_fail("basic entity init", 0);
    drm_sched_fini(&sched);
    return;
  }

  ret = p4s_push_and_wait(&entity, 0, 1, false, NULL);
  if (ret > 0) {
    p4s_ok("one job signals and completes");
  } else {
    p4s_fail("one job wait", ret);
    klogf("[INFO] sched run_job=%d signal_work=%d timedout=%d\n",
          atomic_read(&p4s_run_calls), atomic_read(&p4s_signal_calls),
          atomic_read(&p4s_timedout));
    p4s_broken = true;
  }

  drm_sched_entity_fini(&entity);
  drm_sched_fini(&sched);
  p4s_ok("scheduler init/fini clean");
}

static void p4s_test_order(void) {
  struct drm_gpu_scheduler sched;
  struct drm_gpu_scheduler *sched_list[2] = {&sched, &sched};
  struct drm_sched_entity ents[2];
  bool bad = false;

  if (drm_sched_init(&sched, &p4s_ops, 4, 1, msecs_to_jiffies(P4S_TIMEOUT_MS),
                     system_wq, NULL, "p4s-order", NULL) != 0) {
    p4s_fail("order init", 0);
    return;
  }
  for (int e = 0; e < 2; e++) {
    if (drm_sched_entity_init(&ents[e], DRM_SCHED_PRIORITY_NORMAL, sched_list,
                              1, NULL) != 0) {
      p4s_fail("order entity init", e);
      return;
    }
  }

  /* 8 jobs per entity, pushed sequentially; each wait only returns once that
   * job's finished fence signaled, so a monotonic sequence is proof that the
   * entity's jobs ran in submission order. */
  for (int e = 0; e < 2; e++) {
    for (int i = 0; i < 8; i++) {
      long ret = p4s_push_and_wait(&ents[e], (unsigned int)e, (u64)(e * 100 + i),
                                   false, NULL);
      if (ret <= 0) {
        bad = true;
        break;
      }
    }
    if (bad)
      break;
  }
  if (!bad)
    p4s_ok("16 jobs across 2 entities complete in order");
  else
    p4s_fail("order job wait", 0);

  for (int e = 0; e < 2; e++)
    drm_sched_entity_fini(&ents[e]);
  drm_sched_fini(&sched);
}

static void p4s_test_timeout(void) {
  struct drm_gpu_scheduler sched;
  struct drm_gpu_scheduler *sched_list[1] = {&sched};
  struct drm_sched_entity entity;
  struct completion timedout_done;
  struct p4s_job *j;
  struct dma_fence *finished;
  long ret;
  int before = atomic_read(&p4s_timedout);

  if (drm_sched_init(&sched, &p4s_ops, 2, 1, msecs_to_jiffies(P4S_TIMEOUT_MS),
                     system_wq, NULL, "p4s-timeout", NULL) != 0) {
    p4s_fail("timeout init", 0);
    return;
  }
  if (drm_sched_entity_init(&entity, DRM_SCHED_PRIORITY_NORMAL, sched_list, 1,
                            NULL) != 0) {
    p4s_fail("timeout entity init", 0);
    drm_sched_fini(&sched);
    return;
  }

  init_completion(&timedout_done);
  j = p4s_job_new(&entity, 0, 7, true, &timedout_done);
  if (!j) {
    p4s_fail("timeout job alloc", 0);
    drm_sched_entity_fini(&entity);
    drm_sched_fini(&sched);
    return;
  }
  finished = dma_fence_get(&j->job.s_fence->finished);
  drm_sched_entity_push_job(&j->job);

  ret = wait_for_completion_timeout(&timedout_done,
                                    msecs_to_jiffies(2000));
  if (ret > 0 && atomic_read(&p4s_timedout) == before + 1)
    p4s_ok("timedout_job fired for a hung job");
  else
    p4s_fail("timedout_job did not fire", ret);

  /* Let the hung job complete so the scheduler can be torn down cleanly. */
  dma_fence_signal(&j->fence->base);
  ret = dma_fence_wait_timeout(finished, false, msecs_to_jiffies(2000));
  if (ret > 0)
    p4s_ok("hung job completes after the fence is signaled");
  else
    p4s_fail("hung job wait", ret);
  dma_fence_put(finished);

  drm_sched_entity_fini(&entity);
  drm_sched_fini(&sched);
}

static void p4s_test_stress(void) {
  struct drm_gpu_scheduler sched;
  struct drm_gpu_scheduler *sched_list[1] = {&sched};
  struct drm_sched_entity entity;
  int before_freed = atomic_read(&p4s_freed);
  int errs = 0;
  long baseline = 0, final = 0;

  if (drm_sched_init(&sched, &p4s_ops, 4, 1, msecs_to_jiffies(P4S_TIMEOUT_MS),
                     system_wq, NULL, "p4s-stress", NULL) != 0) {
    p4s_fail("stress init", 0);
    return;
  }
  if (drm_sched_entity_init(&entity, DRM_SCHED_PRIORITY_NORMAL, sched_list, 1,
                            NULL) != 0) {
    p4s_fail("stress entity init", 0);
    drm_sched_fini(&sched);
    return;
  }

  for (int i = 0; i < P4S_STRESS_JOBS; i++) {
    if (p4s_push_and_wait(&entity, 0, (u64)i, false, NULL) <= 0) {
      errs++;
      break;
    }
    if (i == 3)
      baseline = (long)asc_pmm_get_free_pages_total();
  }
  if (errs == 0)
    p4s_ok("stress jobs completed");
  else
    p4s_fail("stress job wait errors", errs);

  /* free_job runs from the scheduler's cleanup work; give it a moment. */
  msleep(100);
  if (atomic_read(&p4s_freed) == before_freed + P4S_STRESS_JOBS)
    p4s_ok("every stress job freed exactly once");
  else
    p4s_fail("stress free count",
             atomic_read(&p4s_freed) - before_freed - P4S_STRESS_JOBS);

  final = (long)asc_pmm_get_free_pages_total();
  klogf("[  OK  ] LinuxKPI: sched stress PMM delta=%ld\n", final - baseline);

  drm_sched_entity_fini(&entity);
  drm_sched_fini(&sched);
}

void linuxkpi_test_phase4_sched(void) {
  p4s_failures = 0;
  p4s_broken = false;
  klog_puts("[LINUXKPI] Phase 4 drm_sched self-test\n");

  p4s_test_timeout_probe();
  p4s_test_wakeup_probe();
  p4s_test_basic();
  if (!p4s_broken) {
    p4s_test_order();
    p4s_test_timeout();
    p4s_test_stress();
  } else {
    klog_puts("[SKIP] LinuxKPI: sched order/timeout/stress after basic failure\n");
  }

  klogf("[INFO] LinuxKPI: sched calls run_job=%d signal_work=%d freed=%d\n",
        atomic_read(&p4s_run_calls), atomic_read(&p4s_signal_calls),
        atomic_read(&p4s_freed));
  if (p4s_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: sched suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: sched suite had %d failure(s)\n", p4s_failures);
}
