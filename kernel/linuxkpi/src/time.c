/* LinuxKPI time base: jiffies, ktime, delay and schedule_timeout.
 * See linux/jiffies.h, linux/ktime.h, linux/delay.h and linux/sched.h. */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/timer.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_sched.h>

volatile unsigned long jiffies;
u64 jiffies_64;

/* jiffies tracks the monotonic millisecond clock; the native one-shot LAPIC
 * timer is too sparse to count ticks.  Called from the tick hook and from the
 * timer kthread so the clock never lags behind the timer wheel.  The CAS loop
 * keeps it monotonic when CPUs race to update it. */
void linuxkpi_jiffies_sync(void) {
  unsigned long now = (unsigned long)linuxkpi_monotonic_ms();
  unsigned long cur = __atomic_load_n(&jiffies, __ATOMIC_RELAXED);

  while (now > cur) {
    if (__atomic_compare_exchange_n(&jiffies, &cur, now, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      break;
  }

  jiffies_64 = __atomic_load_n(&jiffies, __ATOMIC_RELAXED);
}

void linuxkpi_timer_tick(void) { linuxkpi_jiffies_sync(); }

/* Bring-up: seed jiffies from the monotonic clock and start the timer
 * thread.  Called from kernel.c once the scheduler is up. */
void linuxkpi_time_init(void) {
  jiffies = (unsigned long)linuxkpi_monotonic_ms();
  jiffies_64 = jiffies;
  linuxkpi_timer_init();
}

unsigned long msecs_to_jiffies(const unsigned int m) { return m; }

unsigned long usecs_to_jiffies(const unsigned int u) {
  return (u + (1000000 / HZ) - 1) / (1000000 / HZ);
}

unsigned int jiffies_to_msecs(const unsigned long j) { return (unsigned int)j; }

unsigned int jiffies_to_usecs(const unsigned long j) {
  return (unsigned int)(j * 1000UL);
}

unsigned long nsecs_to_jiffies(u64 n) {
  return (unsigned long)((n + 999999ULL) / 1000000ULL);
}

u64 jiffies_to_nsecs(const unsigned long j) { return (u64)j * 1000000ULL; }

ktime_t ktime_get(void) { return (ktime_t)linuxkpi_monotonic_ns(); }
u64 ktime_get_ns(void) { return linuxkpi_monotonic_ns(); }
ktime_t ktime_get_boottime(void) { return ktime_get(); }
u64 ktime_get_boottime_ns(void) { return ktime_get_ns(); }
ktime_t ktime_get_raw(void) { return ktime_get(); }
u64 ktime_get_raw_ns(void) { return ktime_get_ns(); }

/* Fast/mono and wall-clock variants amdgpu timestamps with.  There is no
 * clocksource split here: both return the monotonic nanosecond clock, and the
 * wall clock is derived from it (no RTC offset). */
u64 ktime_get_mono_fast_ns(void) { return ktime_get_ns(); }

time64_t ktime_get_real_seconds(void) {
  return (time64_t)(ktime_get_ns() / 1000000000ULL);
}

void schedule(void) { linuxkpi_thread_block(); }

void yield(void) { schedule(); }

long schedule_timeout(long timeout) {
  if (timeout == MAX_SCHEDULE_TIMEOUT) {
    linuxkpi_thread_block();
    return MAX_SCHEDULE_TIMEOUT;
  }
  if (timeout <= 0)
    return 0;

  unsigned long start = jiffies;
  int woken = linuxkpi_schedule_timeout_ms((unsigned long)timeout);
  if (!woken)
    return 0;

  long elapsed = (long)(jiffies - start);
  long remaining = timeout - elapsed;
  return remaining > 0 ? remaining : 1;
}

long schedule_timeout_uninterruptible(long timeout) {
  return schedule_timeout(timeout);
}

long schedule_timeout_interruptible(long timeout) {
  if (signal_pending(current))
    return -ERESTARTSYS;
  return schedule_timeout(timeout);
}

long schedule_timeout_killable(long timeout) {
  if (signal_pending(current))
    return -ERESTARTSYS;
  return schedule_timeout(timeout);
}

int wake_up_process(struct task_struct *p) {
  void *thread = task_struct_to_thread(p);

  if (thread)
    linuxkpi_wake_thread(thread);
  return 1;
}

bool signal_pending(struct task_struct *p) {
  void *thread = task_struct_to_thread(p);

  return thread ? linuxkpi_thread_has_pending_signal(thread) : false;
}

void msleep(unsigned int msecs) {
  (void)schedule_timeout((long)msecs_to_jiffies(msecs));
}

unsigned long msleep_interruptible(unsigned int msecs) {
  unsigned long deadline = jiffies + msecs_to_jiffies(msecs);

  while (time_before(jiffies, deadline) && !signal_pending(current)) {
    long remaining = (long)(deadline - jiffies);
    if (remaining > 0)
      schedule_timeout(remaining);
  }

  return time_before(jiffies, deadline) ? (deadline - jiffies) : 0;
}

/* usleep_range() precision: the native tick is 1 ms (HZ=1000), so the old
 * shape - schedule_timeout(usecs_to_jiffies(min)) - rounded every
 * sub-millisecond request UP to one jiffy.  That over-slept (e.g. the PSP
 * fence loop's usleep_range(60, 100) slept ~1 ms per iteration, 10x its
 * max) but never returned early, so it was not the AUTOLOAD_RLC failure.
 * Sleep the whole-millisecond part on the scheduler, then spin out the tail
 * on the calibrated TSC: the call now returns after `min` us (never before),
 * matching upstream's range-sleep semantics. */
void usleep_range(unsigned long min, unsigned long max) {
  unsigned long long start, deadline, now;

  (void)max;
  if (min == 0)
    return;

  start = linuxkpi_monotonic_ns();
  if (!start) {
    /* TSC not calibrated yet (cannot happen once the initcalls run). */
    msleep((min + 999) / 1000);
    return;
  }

  deadline = start + (unsigned long long)min * 1000ULL;

  if (min >= 1000)
    schedule_timeout((long)msecs_to_jiffies((unsigned int)(min / 1000)));

  for (;;) {
    now = linuxkpi_monotonic_ns();
    if (now >= deadline)
      break;
    linuxkpi_udelay_ns(deadline - now);
  }
}

void usleep_range_state(unsigned long min, unsigned long max,
                        unsigned int state) {
  (void)state;
  usleep_range(min, max);
}

void ssleep(unsigned int seconds) { msleep(seconds * 1000U); }
