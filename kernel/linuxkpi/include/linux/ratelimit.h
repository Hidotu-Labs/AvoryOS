#ifndef __AVORY_LINUXKPI_RATELIMIT_H
#define __AVORY_LINUXKPI_RATELIMIT_H

/* Minimal Linux <linux/ratelimit.h> overlay.
 *
 * The upstream exit path prints current->comm; there is no complete task
 * struct yet, so the report is dropped.  The macro surface mirrors upstream
 * so imported code (asm-generic/bug.h reaches this header) compiles. */

#include <linux/ratelimit_types.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/atomic.h>

static inline void ratelimit_state_init(struct ratelimit_state *rs,
                                        int interval, int burst) {
  memset(rs, 0, sizeof(*rs));
  raw_spin_lock_init(&rs->lock);
  rs->interval = interval;
  rs->burst = burst;
}

static inline void ratelimit_default_init(struct ratelimit_state *rs) {
  ratelimit_state_init(rs, DEFAULT_RATELIMIT_INTERVAL,
                       DEFAULT_RATELIMIT_BURST);
}

static inline void ratelimit_state_reset_interval(struct ratelimit_state *rs,
                                                 int interval) {
  unsigned long flags;
  raw_spin_lock_irqsave(&rs->lock, flags);
  rs->interval = interval;
  rs->begin = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  rs->flags &= ~RATELIMIT_INITIALIZED;
  atomic_set(&rs->rs_n_left, rs->burst);
  atomic_set(&rs->missed, 0);
#else
  rs->printed = rs->missed = 0;
#endif
  raw_spin_unlock_irqrestore(&rs->lock, flags);
}

static inline void ratelimit_state_exit(struct ratelimit_state *rs) {
  /* Nothing to report without a task name. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  atomic_set(&rs->missed, 0);
#else
  rs->missed = 0;
#endif
}

static inline void ratelimit_set_flags(struct ratelimit_state *rs,
                                       unsigned long flags) {
  rs->flags = flags;
}

extern struct ratelimit_state printk_ratelimit_state;

#define WARN_ON_RATELIMIT(condition, state) WARN_ON(condition)

#define WARN_RATELIMIT(condition, format, ...)                                \
  ({                                                                          \
    int rtn = !!(condition);                                                  \
    if (rtn)                                                                  \
      WARN(rtn, format, ##__VA_ARGS__);                                       \
    rtn;                                                                      \
  })

#endif /* __AVORY_LINUXKPI_RATELIMIT_H */
