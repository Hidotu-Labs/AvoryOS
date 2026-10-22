#ifndef __AVORY_LINUXKPI_TIMEKEEPING_H
#define __AVORY_LINUXKPI_TIMEKEEPING_H

#include <linux/ktime.h>
/* Keep upstream auxiliary definitions while retaining native exported clock
 * accessors. Linux's static convenience accessors otherwise conflict with
 * the native declarations already provided by our ktime overlay. */
#define ktime_get_boottime linuxkpi_upstream_ktime_get_boottime
#define ktime_get_ns linuxkpi_upstream_ktime_get_ns
#define ktime_get_boottime_ns linuxkpi_upstream_ktime_get_boottime_ns
#define ktime_get_raw_ns linuxkpi_upstream_ktime_get_raw_ns
#include_next <linux/timekeeping.h>
#undef ktime_get_boottime
#undef ktime_get_ns
#undef ktime_get_boottime_ns
#undef ktime_get_raw_ns

#endif
