#ifndef __AVORY_LINUXKPI_TIMERQUEUE_H
#define __AVORY_LINUXKPI_TIMERQUEUE_H

/* AvoryOS overlay for <linux/timerqueue.h>.
 *
 * The hrtimer deadline queue is a sorted list (linuxkpi/src/timer.c), while
 * stock timerqueue.h uses an rbtree.  Imported code only needs the type shape
 * (node.expires, the head type), so keep the list-based node that the hrtimer
 * implementation uses.  Overlaying the stock header keeps rtc.h and hrtimer.h
 * from defining the same struct twice. */

#include <linux/ktime.h>
#include <linux/list.h>

struct timerqueue_node {
  struct list_head node; /* Avory: sorted-list link, not an rb_node */
  ktime_t expires;
};

struct timerqueue_head {
  struct list_head head;
};

#endif /* __AVORY_LINUXKPI_TIMERQUEUE_H */
