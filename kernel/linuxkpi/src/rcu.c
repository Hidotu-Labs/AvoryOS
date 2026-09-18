/* LinuxKPI RCU.  See linux/rcupdate.h.
 *
 * Read side is a global atomic reader count (preempt-disable plus counter):
 * the counter is shared, so readers on different CPUs do bounce its cache
 * line, but the preempt_disable() already required for correctness means the
 * lock/unlock pair is a full barrier and the semantics stay simple.
 *
 * A grace period used to poll that count every millisecond; it is now
 * event-driven - the reader that brings the count to zero wakes the waiter -
 * which removes the up-to-1 ms completion latency (and the starvation window
 * when readers keep re-entering: the poller could keep missing the gap).
 *
 * Callbacks are drained by a dedicated kthread after a grace period.
 *
 * Remaining known cost: the shared reader counter.  Sharding it per CPU
 * needs a grace-period engine driven by quiescent states (scheduler hooks +
 * resched IPIs), not just a counter, so it is deliberately left as future
 * work rather than approximating the semantics. */

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native.h>
#include <linuxkpi/native_sched.h>
#include <linuxkpi/service.h>

static volatile int rcu_readers;
static volatile int rcu_busy;
static struct rcu_head *rcu_callbacks;
static DEFINE_SPINLOCK(rcu_lock);
static struct wait_queue_head rcu_wait;
static struct wait_queue_head rcu_barrier_wait;
static struct kpi_service rcu_svc;

/* Set while a grace period is waiting for the reader count to reach zero.
 * The last reader to leave wakes it instead of a poller noticing at the next
 * millisecond tick; the flag keeps the read-side fast path free of waitqueue
 * traffic when nobody is waiting. */
static volatile int rcu_gp_waiting;

void __kpi_rcu_read_lock(void) {
  preempt_disable();
  __atomic_add_fetch(&rcu_readers, 1, __ATOMIC_ACQ_REL);
}

void __kpi_rcu_read_unlock(void) {
  if (__atomic_sub_fetch(&rcu_readers, 1, __ATOMIC_ACQ_REL) == 0 &&
      __atomic_load_n(&rcu_gp_waiting, __ATOMIC_ACQUIRE))
    __kpi_wake_up(&rcu_wait, 0, 0);
  preempt_enable();
}

/* Lockdep-less RCU probes: a kernel without PROVE_RCU reports "held" for all
 * of them (upstream debug_lockdep_rcu_enabled() == 0 path). */
int rcu_read_lock_held(void) { return 1; }
int rcu_read_lock_bh_held(void) { return 1; }
int rcu_read_lock_sched_held(void) { return 1; }
int rcu_read_lock_any_held(void) { return 1; }

/* Wait for the read side to drain.  The wait is event-driven: the reader that
 * brings the count to zero wakes us, so a grace period cannot be delayed by
 * up to a poll interval.  The exchange publishes "someone is waiting" as a
 * full barrier, which pairs with the reader's counter RMW: if the reader's
 * zero transition happens after that exchange it sees the waitqueue and
 * wakes us, and if it happens before, the condition check below already
 * observes zero and does not sleep.  Polling is gone either way. */
static void rcu_wait_grace_period(void) {
  if (__atomic_load_n(&rcu_readers, __ATOMIC_ACQUIRE) == 0) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return;
  }

  __atomic_exchange_n(&rcu_gp_waiting, 1, __ATOMIC_ACQ_REL);
  wait_event(rcu_wait, __atomic_load_n(&rcu_readers, __ATOMIC_ACQUIRE) == 0);
  __atomic_store_n(&rcu_gp_waiting, 0, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void synchronize_rcu(void) { rcu_wait_grace_period(); }

void synchronize_rcu_expedited(void) { rcu_wait_grace_period(); }

void call_rcu(struct rcu_head *head, rcu_callback_t func) {
  unsigned long flags;

  head->func = func;

  spin_lock_irqsave(&rcu_lock, flags);
  head->next = rcu_callbacks;
  rcu_callbacks = head;
  spin_unlock_irqrestore(&rcu_lock, flags);

  kpi_service_kick(&rcu_svc, true);
}

void rcu_barrier(void) {
  while (__atomic_load_n(&rcu_busy, __ATOMIC_ACQUIRE) ||
         __atomic_load_n(&rcu_readers, __ATOMIC_ACQUIRE) != 0 ||
         rcu_callbacks != NULL) {
    /* Let any in-flight callback batch finish. */
    synchronize_rcu();
    if (__atomic_load_n(&rcu_busy, __ATOMIC_ACQUIRE))
      wait_event(rcu_barrier_wait,
                 !__atomic_load_n(&rcu_busy, __ATOMIC_ACQUIRE));
    if (rcu_callbacks == NULL)
      break;
  }
}

static int rcu_kthread(void *arg) {
  (void)arg;

  for (;;) {
    unsigned int gen = kpi_service_gen(&rcu_svc);

    (void)wait_event_timeout(rcu_wait,
                             kthread_should_stop() || rcu_callbacks != NULL,
                             msecs_to_jiffies(KPI_SERVICE_IDLE_MS));

    if (kthread_should_stop()) {
      kpi_service_forget(&rcu_svc);
      return 0;
    }

    if (!rcu_callbacks) {
      if (kpi_service_retire(&rcu_svc, gen))
        return 0;
      continue;
    }

    rcu_wait_grace_period();

    unsigned long flags;
    spin_lock_irqsave(&rcu_lock, flags);
    struct rcu_head *head = rcu_callbacks;
    rcu_callbacks = NULL;
    if (head)
      __atomic_store_n(&rcu_busy, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&rcu_lock, flags);

    if (!head)
      continue;

    while (head) {
      struct rcu_head *next = head->next;
      head->func(head);
      head = next;
    }

    __atomic_store_n(&rcu_busy, 0, __ATOMIC_RELEASE);
    __kpi_wake_up(&rcu_barrier_wait, 0, 0);
  }
}

void linuxkpi_rcu_init(void) {
  init_waitqueue_head(&rcu_wait);
  init_waitqueue_head(&rcu_barrier_wait);
  kpi_service_init(&rcu_svc, "rcu_kpi", rcu_kthread, NULL);
}

/* kfree_rcu support: remember the object behind an embedded rcu_head. */
struct kfree_rcu_ent {
  struct rcu_head head;
  void *obj;
};

static void kfree_rcu_cb(struct rcu_head *head) {
  struct kfree_rcu_ent *ent =
      container_of(head, struct kfree_rcu_ent, head);

  kfree(ent->obj);
  kfree(ent);
}

void __kvfree_call_rcu(struct rcu_head *head, __SIZE_TYPE__ offset) {
  struct kfree_rcu_ent *ent = kmalloc(sizeof(*ent), GFP_KERNEL);

  if (!ent)
    return; /* leak rather than corrupt; never expected */
  ent->obj = (void *)((char *)head - offset);
  call_rcu(&ent->head, kfree_rcu_cb);
}

/* ── `rcu_bench=1`: read-side and grace-period benchmark ─────────────────────
 *
 * Measures the two costs the implementation trades against each other:
 *  - the read side: cycles for one rcu_read_lock()/rcu_read_unlock() pair,
 *    alone and with three other CPUs hammering the same path;
 *  - the grace period: synchronize_rcu() and call_rcu() latency while a
 *    reader keeps the period open.
 *
 * Runs in its own kthread (blocking calls are not safe in the BSP idle
 * context) and is gated on the kernel command line like the other boot
 * benchmarks.
 */

#define RCU_BENCH_ITERS 200000u
#define RCU_BENCH_THREADS 3
#define RCU_BENCH_SYNCS 100
#define RCU_BENCH_CALLS 5

struct rcu_bench_worker {
  volatile int done;
  volatile int stop;
  volatile int sections;             /* sections entered so far           */
  volatile unsigned long long last_exit; /* TSC at the last read_unlock  */
  uint64_t cycles;
};

static int rcu_bench_read_worker(void *arg) {
  struct rcu_bench_worker *w = (struct rcu_bench_worker *)arg;

  uint64_t t0 = linuxkpi_rdtsc_fence();
  for (unsigned int i = 0; i < RCU_BENCH_ITERS; i++) {
    __kpi_rcu_read_lock();
    __kpi_rcu_read_unlock();
  }
  w->cycles = linuxkpi_rdtsc_fence() - t0;
  __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
  return 0;
}

/* Holds a read-side section for ~RCU_BENCH_HOLD_SPINS pauses and stays out
 * for a tiny gap, so a grace period started at any time almost always has to
 * wait for a section to end and then measures only the completion latency
 * under test.  last_exit records exactly when that section ended. */
#define RCU_BENCH_HOLD_SPINS 400000u

static int rcu_bench_busy_worker(void *arg) {
  struct rcu_bench_worker *w = (struct rcu_bench_worker *)arg;

  while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
    __kpi_rcu_read_lock();
    __atomic_add_fetch(&w->sections, 1, __ATOMIC_RELAXED);
    for (unsigned int i = 0; i < RCU_BENCH_HOLD_SPINS; i++)
      linuxkpi_cpu_relax();
    w->last_exit = linuxkpi_rdtsc_fence();
    __kpi_rcu_read_unlock();
    for (unsigned int i = 0; i < 2000; i++)
      linuxkpi_cpu_relax();
  }
  __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
  return 0;
}

static uint64_t rcu_bench_read_loop(unsigned int iters) {
  uint64_t t0 = linuxkpi_rdtsc_fence();
  for (unsigned int i = 0; i < iters; i++) {
    __kpi_rcu_read_lock();
    __kpi_rcu_read_unlock();
  }
  return linuxkpi_rdtsc_fence() - t0;
}

static volatile int rcu_bench_cb_done;

static void rcu_bench_cb(struct rcu_head *head) {
  (void)head;
  __atomic_store_n(&rcu_bench_cb_done, 1, __ATOMIC_RELEASE);
}

/* Time from the busy reader's last unlock to the grace period completing.
 * That is the number the polling implementation cannot get below its 1 ms
 * sleep; an event-driven one completes within a wakeup. */
static void rcu_bench_sync_case(struct rcu_bench_worker *busy) {
  uint64_t sum = 0;
  uint64_t max = 0;

  for (unsigned int i = 0; i < RCU_BENCH_SYNCS; i++) {
    synchronize_rcu();
    uint64_t now = linuxkpi_rdtsc_fence();
    uint64_t exit = __atomic_load_n(&busy->last_exit, __ATOMIC_ACQUIRE);
    uint64_t d = (exit && now > exit) ? now - exit : 0;
    sum += d;
    if (d > max)
      max = d;
  }

  klogf("[RCU-BENCH] sync-after: avg %llu us, max %llu us"
        " after last reader (%u calls)\n",
        (unsigned long long)(linuxkpi_cycles_to_ns(sum / RCU_BENCH_SYNCS) /
                             1000),
        (unsigned long long)(linuxkpi_cycles_to_ns(max) / 1000),
        RCU_BENCH_SYNCS);
}

static void rcu_bench_call_case(struct rcu_bench_worker *busy) {
  struct rcu_head head;
  uint64_t sum = 0;
  uint64_t max = 0;
  unsigned int timeouts = 0;

  for (unsigned int i = 0; i < RCU_BENCH_CALLS; i++) {
    unsigned int spins;

    __atomic_store_n(&rcu_bench_cb_done, 0, __ATOMIC_RELEASE);
    uint64_t t0 = linuxkpi_rdtsc_fence();
    call_rcu(&head, rcu_bench_cb);

    for (spins = 0; spins < 5500; spins++) {
      if (__atomic_load_n(&rcu_bench_cb_done, __ATOMIC_ACQUIRE))
        break;
      msleep(1);
    }
    if (!__atomic_load_n(&rcu_bench_cb_done, __ATOMIC_ACQUIRE)) {
      timeouts++;
      continue;
    }

    uint64_t d = linuxkpi_rdtsc_fence() - t0;
    sum += d;
    if (d > max)
      max = d;
  }

  unsigned int ok = RCU_BENCH_CALLS - timeouts;
  klogf("[RCU-BENCH] cb-call  : avg %llu us, max %llu us from call_rcu"
        " (%u calls, %u timeouts >5.5s)\n",
        (unsigned long long)(linuxkpi_cycles_to_ns(sum / (ok ? ok : 1)) / 1000),
        (unsigned long long)(linuxkpi_cycles_to_ns(max) / 1000),
        RCU_BENCH_CALLS, timeouts);
}

static int rcu_bench_thread(void *arg) {
  struct rcu_bench_worker w[RCU_BENCH_THREADS];
  struct rcu_bench_worker busy;

  (void)arg;
  klog_puts("\n[RCU-BENCH] read-side and grace-period benchmark\n");

  /* 1. Read side, single CPU. */
  uint64_t solo = rcu_bench_read_loop(RCU_BENCH_ITERS);
  klogf("[RCU-BENCH] read-solo : %llu cycles/op\n",
        (unsigned long long)(solo / RCU_BENCH_ITERS));

  /* 2. Read side with three more CPUs on the same path. */
  for (unsigned int i = 0; i < RCU_BENCH_THREADS; i++) {
    w[i].done = 0;
    w[i].stop = 0;
    w[i].cycles = 0;
    kthread_run(rcu_bench_read_worker, &w[i], "rcu/bench%d", i);
  }
  uint64_t t0 = linuxkpi_rdtsc_fence();
  (void)rcu_bench_read_loop(RCU_BENCH_ITERS);
  for (unsigned int i = 0; i < RCU_BENCH_THREADS; i++)
    while (!__atomic_load_n(&w[i].done, __ATOMIC_ACQUIRE))
      linuxkpi_cpu_relax();
  uint64_t total = linuxkpi_rdtsc_fence() - t0;
  uint64_t ops = (uint64_t)RCU_BENCH_ITERS * (RCU_BENCH_THREADS + 1);
  uint64_t ns = linuxkpi_cycles_to_ns(total);
  klogf("[RCU-BENCH] read-4way: %llu cycles/op, %llu Mops/s (%u CPUs)\n",
        (unsigned long long)(total / ops),
        (unsigned long long)(ns ? ops * 1000ULL / ns : 0),
        (unsigned int)(RCU_BENCH_THREADS + 1));

  /* 3+4. Grace period and callback latency with a reader holding sections. */
  busy.done = 0;
  busy.stop = 0;
  busy.sections = 0;
  busy.last_exit = 0;
  kthread_run(rcu_bench_busy_worker, &busy, "rcu/busy");
  unsigned int wait_sections = 0;
  while (!__atomic_load_n(&busy.sections, __ATOMIC_ACQUIRE) &&
         wait_sections < 3000) {
    msleep(1);
    wait_sections++;
  }

  rcu_bench_sync_case(&busy);
  rcu_bench_call_case(&busy);

  __atomic_store_n(&busy.stop, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&busy.done, __ATOMIC_ACQUIRE))
    msleep(1);

  klog_puts("[RCU-BENCH] done\n");
  return 0;
}

void rcu_bench_maybe_run(void) {
  const char *cmd = linuxkpi_boot_cmdline();

  if (!cmd || !strstr(cmd, "rcu_bench"))
    return;
  kthread_run(rcu_bench_thread, NULL, "rcu/bench");
}
