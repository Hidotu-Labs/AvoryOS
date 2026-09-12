#ifndef __AVORY_LINUXKPI_TIMER_H
#define __AVORY_LINUXKPI_TIMER_H

/* Linux <linux/timer.h> overlay: timer_list on top of the LinuxKPI timer
 * wheel (linuxkpi/src/timer.c).  Callbacks run in the timer kthread, never in
 * hardirq context. */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

struct timer_list {
  struct list_head entry;
  unsigned long expires;
  void (*function)(struct timer_list *);
  u32 flags;
  volatile int running;
};

#define TIMER_CPUMASK 0
#define TIMER_MIGRATING 0x10000000
#define TIMER_PINNED 0x01000000
#define TIMER_DEFERRABLE 0x02000000
#define TIMER_IRQSAFE 0x04000000
#define TIMER_ARRAYSHIFT 32

#define TIMER_TRACE_FLAGMASK 0
#define TIMER_INIT_FLAGS 0

#define __TIMER_INITIALIZER(_function, _flags)                                \
  {                                                                           \
    .entry = {NULL, NULL}, .expires = 0, .function = (_function),              \
    .flags = (_flags)                                                         \
  }

#define DEFINE_TIMER(_name, _function)                                        \
  struct timer_list _name = __TIMER_INITIALIZER(_function, 0)

/* Stock __init_timer() memsets the timer before assigning the callback; do
 * the same so embedded/stack-allocated timers cannot inherit a stale
 * `running` value (del_timer_sync() waits on it). */
#define timer_setup(timer, callback, _flags)                                  \
  do {                                                                        \
    memset((timer), 0, sizeof(*(timer)));                                     \
    (timer)->function = (callback);                                           \
    (timer)->flags = (_flags);                                                \
    INIT_LIST_HEAD(&(timer)->entry);                                          \
  } while (0)

#define timer_setup_on_stack(timer, callback, _flags)                         \
  timer_setup(timer, callback, _flags)

#define from_timer(var, callback_timer, timer_fieldname)                      \
  container_of(callback_timer, typeof(*var), timer_fieldname)

void add_timer(struct timer_list *timer);
void add_timer_on(struct timer_list *timer, int cpu);
int mod_timer(struct timer_list *timer, unsigned long expires);
int mod_timer_pending(struct timer_list *timer, unsigned long expires);
int del_timer(struct timer_list *timer);
int del_timer_sync(struct timer_list *timer);
int timer_shutdown_sync(struct timer_list *timer);
int timer_pending(const struct timer_list *timer);
unsigned long round_jiffies(unsigned long j);
unsigned long round_jiffies_relative(unsigned long j);

/* Bring-up: initialize the wheel and start the timer kthread. */
void linuxkpi_timer_init(void);

#endif /* __AVORY_LINUXKPI_TIMER_H */
