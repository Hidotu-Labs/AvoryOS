#ifndef __AVORY_LINUXKPI_JIFFIES_H
#define __AVORY_LINUXKPI_JIFFIES_H

/* Minimal Linux <linux/jiffies.h> overlay.
 *
 * HZ is 1000 (CONFIG_HZ), so one jiffy is one millisecond and jiffies is
 * advanced from the LAPIC timer tick (native lapic_timer.c calls the
 * linuxkpi_timer_tick() weak hook). */

#include <linux/limits.h>
#include <linux/types.h>

#define HZ CONFIG_HZ
#define USER_HZ 100

extern unsigned long volatile jiffies;
extern u64 jiffies_64;

/* Resynchronize jiffies with the monotonic clock.  The native LAPIC timer is
 * one-shot (armed only at scheduling events), so a plain "increment per tick"
 * clock would run slow; the imported-code clock tracks wall time instead. */
void linuxkpi_jiffies_sync(void);

unsigned long msecs_to_jiffies(const unsigned int m);
unsigned long usecs_to_jiffies(const unsigned int u);
unsigned int jiffies_to_msecs(const unsigned long j);
unsigned int jiffies_to_usecs(const unsigned long j);
unsigned long nsecs_to_jiffies(u64 n);
static inline u64 nsecs_to_jiffies64(u64 n) {
  return (u64)nsecs_to_jiffies(n);
}

/* 64-bit tick count for code that must not worry about the 32-bit wrap
 * (drm_dp_mst_topology uses it for AUX timeout bookkeeping).  Upstream
 * exports this as a function; ours is the tracked counter. */
static inline u64 get_jiffies_64(void) { return jiffies_64; }
u64 jiffies_to_nsecs(const unsigned long j);

#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#define time_in_range(a, b, c)                                                \
  (time_after_eq(a, b) && time_before_eq(a, c))
#define time_is_after_jiffies(a) time_before(jiffies, a)
#define time_is_before_jiffies(a) time_after(jiffies, a)
#define time_is_after_eq_jiffies(a) time_before_eq(jiffies, a)
#define time_is_before_eq_jiffies(a) time_after_eq(jiffies, a)

#endif /* __AVORY_LINUXKPI_JIFFIES_H */
