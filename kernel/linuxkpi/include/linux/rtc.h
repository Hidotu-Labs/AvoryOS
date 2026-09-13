#ifndef __AVORY_LINUXKPI_RTC_H
#define __AVORY_LINUXKPI_RTC_H

/* AvoryOS overlay for <linux/rtc.h>.
 *
 * The stock header embeds a struct hrtimer but only includes timerqueue.h;
 * upstream relies on the includer for the hrtimer type.  Pull it in first. */

#include <linux/hrtimer.h>

#include_next <linux/rtc.h>

#endif /* __AVORY_LINUXKPI_RTC_H */
