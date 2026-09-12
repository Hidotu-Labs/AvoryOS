#ifndef __AVORY_LINUXKPI_SCHED_SIGNAL_H
#define __AVORY_LINUXKPI_SCHED_SIGNAL_H

/* AvoryOS overlay for <linux/sched/signal.h>.
 *
 * Imported code only asks whether the current task has a pending signal
 * (interruptible waits).  The real signal_struct/task fields do not exist
 * because task_struct is an opaque native thread handle. */

#include <linux/sched.h>

static inline bool fatal_signal_pending(struct task_struct *p) {
  (void)p;
  return false;
}

static inline int signal_pending_state(unsigned int state,
                                       struct task_struct *p) {
  return signal_pending(p) ? (int)state : 0;
}

static inline bool signal_pending_lock(void) { return false; }

#endif /* __AVORY_LINUXKPI_SCHED_SIGNAL_H */
