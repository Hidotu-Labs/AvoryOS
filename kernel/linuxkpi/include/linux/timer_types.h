#ifndef _LINUX_TIMER_TYPES_H
#define _LINUX_TIMER_TYPES_H

/* Shared split definition for timer.h and imported Linux 6.18 callers. */
#include <linux/types.h>

struct timer_list {
  struct list_head entry;
  unsigned long expires;
  void (*function)(struct timer_list *);
  u32 flags;
};

#endif /* _LINUX_TIMER_TYPES_H */
