#ifndef __AVORY_LINUXKPI_DELAY_H
#define __AVORY_LINUXKPI_DELAY_H

/* Linux <linux/delay.h> overlay.  Busy waits use the calibrated TSC through
 * the native bridge; sleeping waits go through the scheduler. */

#include <linux/types.h>

#include <linuxkpi/native_sched.h>

void msleep(unsigned int msecs);
unsigned long msleep_interruptible(unsigned int msecs);
void usleep_range(unsigned long min, unsigned long max);
void usleep_range_state(unsigned long min, unsigned long max,
                        unsigned int state);
void ssleep(unsigned int seconds);

#define ndelay(n) linuxkpi_udelay_ns((unsigned long long)(n))
#define udelay(n) linuxkpi_udelay_ns((unsigned long long)(n) * 1000ULL)
#define mdelay(n) linuxkpi_udelay_ns((unsigned long long)(n) * 1000000ULL)

/* Upstream delay.h's range-sleep helper: short delays busy-wait, medium ones
 * use a range sleep, long ones msleep.  Copied from the stock header. */
static inline void fsleep(unsigned long usecs) {
  if (usecs <= 10)
    udelay(usecs);
  else if (usecs <= 20000)
    usleep_range(usecs, 2 * usecs);
  else
    msleep((usecs + 999) / 1000);
}

#endif /* __AVORY_LINUXKPI_DELAY_H */
