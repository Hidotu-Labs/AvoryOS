#ifndef __AVORY_LINUXKPI_HRTIMER_H
#define __AVORY_LINUXKPI_HRTIMER_H

/* Linux <linux/hrtimer.h> overlay.
 *
 * hrtimers share the LinuxKPI timer wheel.  The native sleeping granularity
 * is one millisecond (LAPIC deadline list), so expiry is rounded up to that;
 * finer resolution arrives with a dedicated high-resolution timer source. */

#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/types.h>

enum hrtimer_restart {
  HRTIMER_NORESTART,
  HRTIMER_RESTART,
};

enum hrtimer_mode {
  HRTIMER_MODE_ABS = 0x00,
  HRTIMER_MODE_REL = 0x01,
  HRTIMER_MODE_PINNED = 0x02,
  HRTIMER_MODE_SOFT = 0x04,
  HRTIMER_MODE_HARD = 0x08,

  HRTIMER_MODE_ABS_PINNED = HRTIMER_MODE_ABS | HRTIMER_MODE_PINNED,
  HRTIMER_MODE_REL_PINNED = HRTIMER_MODE_REL | HRTIMER_MODE_PINNED,
  HRTIMER_MODE_ABS_SOFT = HRTIMER_MODE_ABS | HRTIMER_MODE_SOFT,
  HRTIMER_MODE_REL_SOFT = HRTIMER_MODE_REL | HRTIMER_MODE_SOFT,
  HRTIMER_MODE_ABS_PINNED_SOFT = HRTIMER_MODE_ABS_PINNED | HRTIMER_MODE_SOFT,
  HRTIMER_MODE_REL_PINNED_SOFT = HRTIMER_MODE_REL_PINNED | HRTIMER_MODE_SOFT,
};

/* Upstream's node is an rbtree node (timerqueue_node).  AvoryOS keeps the
 * expiry list as a sorted list, but keeps the `node.expires` member path that
 * callers such as vkms use. */
struct timerqueue_node {
  struct list_head node; /* Avory: sorted-list link, not an rb_node */
  ktime_t expires;
};

struct hrtimer {
  struct timerqueue_node node;
  enum hrtimer_restart (*function)(struct hrtimer *);
  int queued;
  int clock_id;
  volatile int running;
};

#define hrtimer_cb_get_time(timer) ktime_get()

void hrtimer_init(struct hrtimer *timer, clockid_t which_clock,
                  enum hrtimer_mode mode);
int hrtimer_start(struct hrtimer *timer, ktime_t tim,
                  const enum hrtimer_mode mode);
int hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
                           u64 delta_ns, const enum hrtimer_mode mode);
int hrtimer_cancel(struct hrtimer *timer);
int hrtimer_try_to_cancel(struct hrtimer *timer);
int hrtimer_active(const struct hrtimer *timer);
int hrtimer_is_queued(struct hrtimer *timer);
ktime_t hrtimer_get_expires(const struct hrtimer *timer);
void hrtimer_set_expires(struct hrtimer *timer, ktime_t time);
void hrtimer_set_expires_range_ns(struct hrtimer *timer, ktime_t time,
                                  u64 delta_ns);
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval);
#define hrtimer_forward_now(timer, interval)                                  \
  hrtimer_forward(timer, ktime_get(), (interval))

#endif /* __AVORY_LINUXKPI_HRTIMER_H */
