/* Phase 1 — timers, deferred work, sleeping synchronization and RCU tests.
 * Compiled with the real Linux headers. */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include <linuxkpi/log.h>

/* ── jiffies / msleep ───────────────────────────────────────────────────── */

static bool test_jiffies(void) {
  unsigned long start = jiffies;
  msleep(20);
  unsigned long elapsed = jiffies - start;
  return elapsed >= 15 && elapsed < 200;
}

/* ── timer_list ─────────────────────────────────────────────────────────── */

static volatile int timer_fired;
static void test_timer_cb(struct timer_list *t) {
  (void)t;
  timer_fired = 1;
}

static bool test_timer_list(void) {
  static struct timer_list timer;

  timer_fired = 0;
  timer_setup(&timer, test_timer_cb, 0);
  mod_timer(&timer, jiffies + 20);

  for (int i = 0; i < 200 && !timer_fired; i++)
    msleep(1);

  del_timer_sync(&timer);
  return timer_fired == 1;
}

/* ── hrtimer ────────────────────────────────────────────────────────────── */

static volatile int hrtimer_fired;
static enum hrtimer_restart test_hrtimer_cb(struct hrtimer *t) {
  (void)t;
  hrtimer_fired = 1;
  return HRTIMER_NORESTART;
}

static bool test_hrtimer(void) {
  static struct hrtimer timer;

  hrtimer_fired = 0;
  hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  timer.function = test_hrtimer_cb;
  hrtimer_start(&timer, ms_to_ktime(20), HRTIMER_MODE_REL);

  for (int i = 0; i < 200 && !hrtimer_fired; i++)
    msleep(1);

  hrtimer_cancel(&timer);
  return hrtimer_fired == 1;
}

/* ── completion across a kernel thread ──────────────────────────────────── */

struct completion_test {
  struct completion done;
};

static int completion_thread(void *arg) {
  struct completion_test *c = arg;

  msleep(20);
  complete(&c->done);
  return 0;
}

static bool test_completion(void) {
  struct completion_test c;

  init_completion(&c.done);

  struct task_struct *task =
      kthread_run(completion_thread, &c, "kpi/ctest");
  if (IS_ERR(task))
    return false;

  unsigned long ret = wait_for_completion_timeout(&c.done, 1000);
  kthread_stop(task);
  if (!ret || completion_done(&c.done))
    return false;

  /* A zero timeout still claims a ready token and reports success. */
  complete(&c.done);
  if (wait_for_completion_timeout(&c.done, 0) != 1 ||
      completion_done(&c.done))
    return false;
  complete(&c.done);
  if (wait_for_completion_interruptible_timeout(&c.done, 0) != 1 ||
      completion_done(&c.done))
    return false;
  complete_all(&c.done);
  if (wait_for_completion_timeout(&c.done, 0) != 1 ||
      !try_wait_for_completion(&c.done))
    return false;
  reinit_completion(&c.done);
  return !try_wait_for_completion(&c.done);
}

struct completion_credit_waiter {
  struct completion *ready;
  struct completion *token;
  bool consumed;
};

static int completion_credit_thread(void *arg) {
  struct completion_credit_waiter *w = arg;
  complete(w->ready);
  w->consumed = wait_for_completion_timeout(w->token, 50) > 0;
  return 0;
}

static bool test_completion_credits(void) {
  struct completion ready, token;
  struct completion_credit_waiter w[2];
  struct task_struct *tasks[2];
  unsigned int started = 0;
  bool ok = true;

  init_completion(&ready);
  init_completion(&token);
  for (unsigned int i = 0; i < 2; i++) {
    w[i].ready = &ready;
    w[i].token = &token;
    w[i].consumed = false;
    tasks[i] = kthread_run(completion_credit_thread, &w[i], "kpi/credit");
    if (IS_ERR(tasks[i])) {
      ok = false;
      break;
    }
    started++;
  }
  for (unsigned int i = 0; i < started; i++)
    if (!wait_for_completion_timeout(&ready, 1000))
      ok = false;
  if (ok)
    complete(&token);
  for (unsigned int i = 0; i < started; i++)
    kthread_stop(tasks[i]);
  if (!ok)
    return false;

  unsigned int consumed = (unsigned int)w[0].consumed + (unsigned int)w[1].consumed;
  klogf("[KPI-COMPLETION] waiters=2 signals=1 consumed=%u remaining=%u\n",
        consumed, token.done);
  return consumed == 1 && token.done == 0;
}

/* ── wait_event/wake_up across a kernel thread ──────────────────────────── */

struct wait_test {
  struct wait_queue_head wq;
  volatile int flag;
};

static int wait_thread(void *arg) {
  struct wait_test *w = arg;

  msleep(20);
  w->flag = 1;
  wake_up(&w->wq);
  return 0;
}

static int wait_locked_thread(void *arg) {
  struct wait_test *w = arg;

  msleep(20);
  spin_lock(&w->wq.lock);
  w->flag = 2;
  wake_up_all_locked(&w->wq);
  spin_unlock(&w->wq.lock);
  return 0;
}

static bool test_wait_event(void) {
  struct wait_test w;

  init_waitqueue_head(&w.wq);
  w.flag = 0;

  struct task_struct *task = kthread_run(wait_thread, &w, "kpi/wtest");
  if (IS_ERR(task))
    return false;

  long ret = wait_event_timeout(w.wq, w.flag, 1000);
  kthread_stop(task);
  if (ret <= 0 || w.flag != 1)
    return false;

  /* Exercise wake_up_all_locked with wq.lock held */
  task = kthread_run(wait_locked_thread, &w, "kpi/wltest");
  if (IS_ERR(task))
    return false;

  ret = wait_event_timeout(w.wq, w.flag == 2, 1000);
  kthread_stop(task);
  return ret > 0 && w.flag == 2;
}

/* ── sleeping mutex under contention ────────────────────────────────────── */

struct mutex_test {
  struct mutex lock;
  volatile int counter;
  volatile int done;
};

static int mutex_thread(void *arg) {
  struct mutex_test *m = arg;

  for (int i = 0; i < 200; i++) {
    mutex_lock(&m->lock);
    m->counter++;
    mutex_unlock(&m->lock);
  }

  __atomic_add_fetch(&m->done, 1, __ATOMIC_RELEASE);
  return 0;
}

static bool test_mutex(void) {
  static struct mutex_test m;

  mutex_init(&m.lock);
  m.counter = 0;
  m.done = 0;

  struct task_struct *t1 = kthread_run(mutex_thread, &m, "kpi/mtx1");
  struct task_struct *t2 = kthread_run(mutex_thread, &m, "kpi/mtx2");
  if (IS_ERR(t1) || IS_ERR(t2))
    return false;

  for (int i = 0; i < 2000 && m.done < 2; i++)
    msleep(1);

  kthread_stop(t1);
  kthread_stop(t2);
  return m.done == 2 && m.counter == 400;
}

/* ── workqueue + delayed work ───────────────────────────────────────────── */

static volatile int work_ran;
static volatile int delayed_ran;

static void test_work_fn(struct work_struct *work) {
  (void)work;
  work_ran = 1;
}

static void test_delayed_fn(struct work_struct *work) {
  (void)work;
  delayed_ran = 1;
}

static bool test_workqueue(void) {
  static struct work_struct work;
  static struct delayed_work dwork;

  work_ran = 0;
  delayed_ran = 0;
  INIT_WORK(&work, test_work_fn);
  INIT_DELAYED_WORK(&dwork, test_delayed_fn);

  queue_work(system_wq, &work);
  flush_work(&work);
  if (!work_ran)
    return false;

  schedule_delayed_work(&dwork, 20);
  for (int i = 0; i < 200 && !delayed_ran; i++)
    msleep(1);
  flush_workqueue(system_wq);
  cancel_delayed_work_sync(&dwork);

  return delayed_ran == 1;
}

/* ── RCU ────────────────────────────────────────────────────────────────── */

static volatile int rcu_fired;
static void test_rcu_cb(struct rcu_head *head) {
  (void)head;
  rcu_fired = 1;
}

static bool test_rcu(void) {
  static struct rcu_head head;

  rcu_fired = 0;
  call_rcu(&head, test_rcu_cb);
  rcu_barrier();
  return rcu_fired == 1;
}

/* Retire published objects while two readers inspect them. Keep the storage
 * allocated until the test ends, so a premature callback is reported rather
 * than turning a failed lifetime test into a kernel use-after-free. */
#define RCU_LIFETIME_OBJECTS 512
struct rcu_lifetime_object {
  struct rcu_head head;
  unsigned int value;
  unsigned int retired;
};
static struct rcu_lifetime_object rcu_lifetime_objects[RCU_LIFETIME_OBJECTS];
static struct rcu_lifetime_object *rcu_lifetime_current;
static unsigned int rcu_lifetime_stop, rcu_lifetime_bad;
static unsigned int rcu_lifetime_reads, rcu_lifetime_retired;

static void rcu_lifetime_retire(struct rcu_head *head) {
  struct rcu_lifetime_object *obj =
      container_of(head, struct rcu_lifetime_object, head);
  WRITE_ONCE(obj->retired, 1);
  __atomic_add_fetch(&rcu_lifetime_retired, 1, __ATOMIC_RELAXED);
}

static int rcu_lifetime_reader(void *arg) {
  (void)arg;
  unsigned int iterations = 0;
  while (iterations < 8192 && !READ_ONCE(rcu_lifetime_stop) &&
         !kthread_should_stop()) {
    rcu_read_lock();
    struct rcu_lifetime_object *obj = rcu_dereference(rcu_lifetime_current);
    if (obj) {
      unsigned int value = READ_ONCE(obj->value);
      for (int i = 0; i < 32; i++) {
        if (READ_ONCE(obj->retired) || READ_ONCE(obj->value) != value)
          WRITE_ONCE(rcu_lifetime_bad, 1);
        cpu_relax();
      }
      __atomic_add_fetch(&rcu_lifetime_reads, 1, __ATOMIC_RELAXED);
    }
    rcu_read_unlock();
    if (!(++iterations & 63)) cond_resched();
  }
  return 0;
}

static bool test_rcu_lifetime(void) {
  struct task_struct *readers[2];
  memset(rcu_lifetime_objects, 0, sizeof(rcu_lifetime_objects));
  rcu_lifetime_current = NULL;
  rcu_lifetime_stop = rcu_lifetime_bad = 0;
  rcu_lifetime_reads = rcu_lifetime_retired = 0;
  /* Bound reader work even if the writer is delayed by scheduling. Publish
   * the first object before starting them so a single-CPU reader cannot use
   * up its budget inspecting an empty pointer before the writer runs. */
  rcu_lifetime_objects[0].value = 1;
  rcu_assign_pointer(rcu_lifetime_current, &rcu_lifetime_objects[0]);
  for (int i = 0; i < 2; i++) {
    readers[i] = kthread_run(rcu_lifetime_reader, NULL, "kpi/rcu-reader");
    if (IS_ERR(readers[i])) {
      WRITE_ONCE(rcu_lifetime_stop, 1);
      for (int j = 0; j < i; j++) kthread_stop(readers[j]);
      return false;
    }
  }
  for (unsigned int i = 0; i < RCU_LIFETIME_OBJECTS; i++) {
    rcu_lifetime_objects[i].value = i + 1;
    rcu_assign_pointer(rcu_lifetime_current, &rcu_lifetime_objects[i]);
    if (i) call_rcu(&rcu_lifetime_objects[i-1].head, rcu_lifetime_retire);
    if (!(i & 63)) msleep(1); /* Also lets readers execute on a single CPU. */
  }
  rcu_assign_pointer(rcu_lifetime_current, NULL);
  call_rcu(&rcu_lifetime_objects[RCU_LIFETIME_OBJECTS-1].head, rcu_lifetime_retire);
  WRITE_ONCE(rcu_lifetime_stop, 1);
  for (int i = 0; i < 2; i++) kthread_stop(readers[i]);
  rcu_barrier();
  klogf("[KPI-RCU] lifetime test: reads=%u retired=%u premature=%u\n",
        rcu_lifetime_reads, rcu_lifetime_retired, rcu_lifetime_bad);
  return rcu_lifetime_reads && rcu_lifetime_retired == RCU_LIFETIME_OBJECTS &&
         !rcu_lifetime_bad;
}

/* ── aggregator ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase1_time(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"jiffies/msleep", test_jiffies},
      {"timer_list", test_timer_list},
      {"hrtimer", test_hrtimer},
      {"completion", test_completion},
      {"completion credits", test_completion_credits},
      {"wait_event", test_wait_event},
      {"mutex", test_mutex},
      {"workqueue", test_workqueue},
      {"rcu", test_rcu},
      {"rcu lifetime", test_rcu_lifetime},
  };

  klog_puts("[LINUXKPI] Phase 1 time/sync self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s (upstream) correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s (upstream) wrong result\n", tests[i].name);
  }
}
