/* LinuxKPI timer wheel: timer_list and hrtimer on top of one kernel thread.
 * See linux/timer.h and linux/hrtimer.h.
 *
 * All callbacks run in the `ktimers` thread; expiry resolution is one
 * millisecond (the native LAPIC deadline granularity).  Callbacks must not
 * block on the exclusivity of the timer lock; they run outside it. */

#include <linux/hrtimer.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_sched.h>

static LIST_HEAD(timer_wheel);
static LIST_HEAD(hrtimer_wheel);
static DEFINE_SPINLOCK(timer_lock);
static struct wait_queue_head timer_wait;
static struct wait_queue_head timer_done_wq;
static volatile unsigned int timer_gen;
static struct task_struct *timer_task;

static void timer_signal_change(void) {
  __atomic_add_fetch(&timer_gen, 1, __ATOMIC_RELEASE);
  __kpi_wake_up(&timer_wait, 0, 0);
}

int timer_pending(const struct timer_list *timer) {
  return !list_empty(&timer->entry);
}

static void __timer_enqueue_locked(struct timer_list *timer) {
  struct list_head *pos = &timer_wheel;

  while (pos->next != &timer_wheel) {
    struct timer_list *cur =
        list_entry(pos->next, struct timer_list, entry);
    if (time_before(timer->expires, cur->expires))
      break;
    pos = pos->next;
  }
  list_add(&timer->entry, pos);
}

void add_timer(struct timer_list *timer) {
  unsigned long flags;

  spin_lock_irqsave(&timer_lock, flags);
  if (!timer_pending(timer))
    __timer_enqueue_locked(timer);
  spin_unlock_irqrestore(&timer_lock, flags);

  timer_signal_change();
}

void add_timer_on(struct timer_list *timer, int cpu) {
  (void)cpu;
  add_timer(timer);
}

int mod_timer(struct timer_list *timer, unsigned long expires) {
  unsigned long flags;
  int pending;

  spin_lock_irqsave(&timer_lock, flags);
  pending = timer_pending(timer);
  if (pending)
    list_del_init(&timer->entry);
  timer->expires = expires;
  __timer_enqueue_locked(timer);
  spin_unlock_irqrestore(&timer_lock, flags);

  timer_signal_change();
  return pending;
}

int mod_timer_pending(struct timer_list *timer, unsigned long expires) {
  return mod_timer(timer, expires);
}

int del_timer(struct timer_list *timer) {
  unsigned long flags;
  int pending;

  spin_lock_irqsave(&timer_lock, flags);
  pending = timer_pending(timer);
  if (pending)
    list_del_init(&timer->entry);
  spin_unlock_irqrestore(&timer_lock, flags);

  return pending;
}

int del_timer_sync(struct timer_list *timer) {
  int ret = del_timer(timer);

  while (__atomic_load_n(&timer->running, __ATOMIC_ACQUIRE))
    wait_event(timer_done_wq,
               !__atomic_load_n(&timer->running, __ATOMIC_ACQUIRE));
  return ret;
}

int timer_shutdown_sync(struct timer_list *timer) { return del_timer_sync(timer); }

unsigned long round_jiffies(unsigned long j) { return j; }
unsigned long round_jiffies_relative(unsigned long j) { return j; }

/* ── hrtimer ────────────────────────────────────────────────────────────── */

static void __hrtimer_enqueue_locked(struct hrtimer *timer) {
  struct list_head *pos = &hrtimer_wheel;

  while (pos->next != &hrtimer_wheel) {
    struct hrtimer *cur = list_entry(pos->next, struct hrtimer, node.node);
    if (timer->node.expires < cur->node.expires)
      break;
    pos = pos->next;
  }
  list_add(&timer->node.node, pos);
}

void hrtimer_init(struct hrtimer *timer, clockid_t which_clock,
                  enum hrtimer_mode mode) {
  (void)mode;
  timer->node.node.next = &timer->node.node;
  timer->node.node.prev = &timer->node.node;
  timer->node.expires = 0;
  timer->function = NULL;
  timer->queued = 0;
  timer->clock_id = which_clock;
  timer->running = 0;
}

int hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim, u64 delta_ns,
                           const enum hrtimer_mode mode) {
  unsigned long flags;
  ktime_t when = (mode & HRTIMER_MODE_REL) ? ktime_get() + tim : tim;

  (void)delta_ns;

  spin_lock_irqsave(&timer_lock, flags);
  if (timer->queued) {
    list_del_init(&timer->node.node);
    timer->queued = 0;
  }
  timer->node.expires = when;
  __hrtimer_enqueue_locked(timer);
  timer->queued = 1;
  spin_unlock_irqrestore(&timer_lock, flags);

  timer_signal_change();
  return 0;
}

int hrtimer_start(struct hrtimer *timer, ktime_t tim,
                  const enum hrtimer_mode mode) {
  return hrtimer_start_range_ns(timer, tim, 0, mode);
}

int hrtimer_try_to_cancel(struct hrtimer *timer) {
  unsigned long flags;
  int ret = 0;

  spin_lock_irqsave(&timer_lock, flags);
  if (timer->queued) {
    list_del_init(&timer->node.node);
    timer->queued = 0;
    ret = 1;
  }
  spin_unlock_irqrestore(&timer_lock, flags);

  return ret;
}

int hrtimer_cancel(struct hrtimer *timer) {
  int ret = hrtimer_try_to_cancel(timer);

  while (__atomic_load_n(&timer->running, __ATOMIC_ACQUIRE))
    wait_event(timer_done_wq,
               !__atomic_load_n(&timer->running, __ATOMIC_ACQUIRE));
  return ret;
}

int hrtimer_active(const struct hrtimer *timer) {
  return timer->queued ||
         __atomic_load_n(&timer->running, __ATOMIC_ACQUIRE);
}

int hrtimer_is_queued(struct hrtimer *timer) { return timer->queued; }

ktime_t hrtimer_get_expires(const struct hrtimer *timer) {
  return timer->node.expires;
}

void hrtimer_set_expires(struct hrtimer *timer, ktime_t time) {
  timer->node.expires = time;
}

void hrtimer_set_expires_range_ns(struct hrtimer *timer, ktime_t time,
                                  u64 delta_ns) {
  (void)delta_ns;
  timer->node.expires = time;
}

u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval) {
  u64 orun = 1;

  if (interval <= 0)
    return 0;

  if (now + interval >= timer->node.expires) {
    ktime_t delta = now - timer->node.expires;
    if (delta >= 0) {
      u64 n = (u64)(delta / interval) + 1;
      timer->node.expires += (ktime_t)(n * interval);
      orun = n;
    }
  }

  return orun;
}

/* ── the timer thread ───────────────────────────────────────────────────── */

static int timer_kthread(void *arg) {
  (void)arg;

  for (;;) {
    if (kthread_should_stop())
      return 0;

    linuxkpi_jiffies_sync();

    unsigned int gen = __atomic_load_n(&timer_gen, __ATOMIC_ACQUIRE);
    unsigned long now = jiffies;
    ktime_t now_ns = ktime_get();

    LIST_HEAD(expired_timers);
    LIST_HEAD(expired_hrtimers);
    unsigned long flags;

    spin_lock_irqsave(&timer_lock, flags);
    while (!list_empty(&timer_wheel)) {
      struct timer_list *t =
          list_first_entry(&timer_wheel, struct timer_list, entry);
      if (time_after(t->expires, now))
        break;
      list_move_tail(&t->entry, &expired_timers);
    }
    while (!list_empty(&hrtimer_wheel)) {
      struct hrtimer *h =
          list_first_entry(&hrtimer_wheel, struct hrtimer, node.node);
      if (h->node.expires > now_ns)
        break;
      list_move_tail(&h->node.node, &expired_hrtimers);
    }

    unsigned long wait_ms = 0;
    if (!list_empty(&timer_wheel)) {
      struct timer_list *t =
          list_first_entry(&timer_wheel, struct timer_list, entry);
      wait_ms = time_after(t->expires, now) ? t->expires - now : 1;
    }
    if (!list_empty(&hrtimer_wheel)) {
      struct hrtimer *h =
          list_first_entry(&hrtimer_wheel, struct hrtimer, node.node);
      if (h->node.expires > now_ns) {
        u64 delta_ns = (u64)(h->node.expires - now_ns);
        unsigned long ms = (unsigned long)((delta_ns + 999999ULL) / 1000000ULL);
        if (!wait_ms || ms < wait_ms)
          wait_ms = ms;
      } else {
        wait_ms = 1;
      }
    }
    spin_unlock_irqrestore(&timer_lock, flags);

    while (!list_empty(&expired_timers)) {
      struct timer_list *t =
          list_first_entry(&expired_timers, struct timer_list, entry);
      list_del_init(&t->entry);

      __atomic_store_n(&t->running, 1, __ATOMIC_RELEASE);
      t->function(t);
      __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
      __kpi_wake_up(&timer_done_wq, 0, 0);
    }

    while (!list_empty(&expired_hrtimers)) {
      struct hrtimer *h =
          list_first_entry(&expired_hrtimers, struct hrtimer, node.node);
      list_del_init(&h->node.node);
      h->queued = 0;

      __atomic_store_n(&h->running, 1, __ATOMIC_RELEASE);
      enum hrtimer_restart restart = h->function(h);
      __atomic_store_n(&h->running, 0, __ATOMIC_RELEASE);

      if (restart == HRTIMER_RESTART) {
        ktime_t when = h->node.expires;
        if (when <= ktime_get())
          when = ktime_get() + 1000000; /* 1 ms floor */

        spin_lock_irqsave(&timer_lock, flags);
        h->node.expires = when;
        __hrtimer_enqueue_locked(h);
        h->queued = 1;
        spin_unlock_irqrestore(&timer_lock, flags);
        timer_signal_change();
      }

      __kpi_wake_up(&timer_done_wq, 0, 0);
    }

    if (wait_ms == 0) {
      wait_event(timer_wait,
                 __atomic_load_n(&timer_gen, __ATOMIC_ACQUIRE) != gen ||
                     kthread_should_stop());
    } else {
      wait_event_timeout(
          timer_wait,
          __atomic_load_n(&timer_gen, __ATOMIC_ACQUIRE) != gen ||
              kthread_should_stop(),
          wait_ms);
    }
  }
}

void linuxkpi_timer_init(void) {
  init_waitqueue_head(&timer_wait);
  init_waitqueue_head(&timer_done_wq);
  timer_task = kthread_run(timer_kthread, NULL, "ktimers");
  if (IS_ERR(timer_task))
    klog_puts("[WARN] LinuxKPI: ktimers thread creation failed\n");
}
