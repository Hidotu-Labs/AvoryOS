/* Phase 1 — timers, deferred work, sleeping synchronization and RCU tests.
 * Compiled with the real Linux headers. */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
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
  return ret > 0;
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
      {"wait_event", test_wait_event},
      {"mutex", test_mutex},
      {"workqueue", test_workqueue},
      {"rcu", test_rcu},
  };

  klog_puts("[LINUXKPI] Phase 1 time/sync self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s (upstream) correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s (upstream) wrong result\n", tests[i].name);
  }
}
