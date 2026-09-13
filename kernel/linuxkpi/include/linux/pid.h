#ifndef __AVORY_LINUXKPI_PID_H
#define __AVORY_LINUXKPI_PID_H

/* AvoryOS overlay for <linux/pid.h>.
 *
 * There is no struct pid allocator: task_tgid() returns an opaque token built
 * from the native tgid (see linux/sched.h).  These helpers keep the stock
 * signatures so imported code compiles and links.  The overlay exists so a
 * translation unit that includes both sched.h and pid.h cannot hit the
 * built-in/stock declaration conflict. */

#include <linux/types.h>

struct pid;

static inline struct pid *get_pid(struct pid *pid) { return pid; }
static inline void put_pid(struct pid *pid) { (void)pid; }
static inline pid_t pid_nr(struct pid *pid) {
  return pid ? (pid_t)((unsigned long)pid - 1) : 0;
}
static inline pid_t pid_vnr(struct pid *pid) { return pid_nr(pid); }

#endif /* __AVORY_LINUXKPI_PID_H */
