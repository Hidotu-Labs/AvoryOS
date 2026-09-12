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

static inline void ratelimit_state_exit(struct ratelimit_state *rs) {
  /* Nothing to report without a task name. */
  rs->missed = 0;
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
