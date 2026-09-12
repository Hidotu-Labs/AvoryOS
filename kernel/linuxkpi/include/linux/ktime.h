#ifndef __AVORY_LINUXKPI_KTIME_H
#define __AVORY_LINUXKPI_KTIME_H

/* Linux <linux/ktime.h> overlay.  ktime_t is a signed 64-bit nanosecond
 * count; all clock reads route to the native monotonic TSC clock. */

#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/types.h>

typedef s64 ktime_t;

#define ktime_set(secs, nsecs)                                                \
  ((ktime_t)((s64)(secs) * NSEC_PER_SEC + (s64)(nsecs)))
#define ns_to_ktime(ns) ((ktime_t)(ns))
#define ms_to_ktime(ms) ns_to_ktime((s64)(ms) * NSEC_PER_MSEC)
#define ktime_to_ns(kt) ((s64)(kt))
#define ktime_to_us(kt) ((s64)(kt) / NSEC_PER_USEC)
#define ktime_to_ms(kt) ((s64)(kt) / NSEC_PER_MSEC)
#define ktime_add(a, b) ((ktime_t)((a) + (b)))
#define ktime_sub(a, b) ((ktime_t)((a) - (b)))
#define ktime_sub_ns(kt, ns) ((ktime_t)((kt) - (s64)(ns)))
#define ktime_add_us(kt, us) ktime_add_ns(kt, (s64)(us) * NSEC_PER_USEC)
#define ktime_sub_us(kt, us) ktime_sub_ns(kt, (s64)(us) * NSEC_PER_USEC)
#define ktime_ms_delta(a, b) (((a) - (b)) / (s64)NSEC_PER_MSEC)
#define ktime_add_ns(kt, ns) ((ktime_t)((kt) + (s64)(ns)))
#define ktime_add_ms(kt, ms) ktime_add_ns(kt, (s64)(ms) * NSEC_PER_MSEC)
#define ktime_after(kt1, kt2) ((kt1) > (kt2))
#define ktime_before(kt1, kt2) ((kt1) < (kt2))
#define ktime_compare(a, b) ((a) > (b) ? 1 : ((a) < (b) ? -1 : 0))
#define ktime_equal(a, b) ((a) == (b))
#define ktime_divns(kt, div) ((s64)(kt) / (s64)(div))

ktime_t ktime_get(void);
u64 ktime_get_ns(void);
ktime_t ktime_get_boottime(void);
u64 ktime_get_boottime_ns(void);
ktime_t ktime_get_raw(void);
u64 ktime_get_raw_ns(void);

/* time64 conversions (upstream ktime.h); ns_to_timespec64 is inline in the
 * imported <linux/time64.h>. */
static inline struct timespec64 ktime_to_timespec64(const ktime_t kt) {
  return ns_to_timespec64(kt);
}

static inline ktime_t timespec64_to_ktime(struct timespec64 ts) {
  return ktime_set(ts.tv_sec, ts.tv_nsec);
}

static inline struct timespec64 ktime_to_timespec(const ktime_t kt) {
  return ktime_to_timespec64(kt);
}

#endif /* __AVORY_LINUXKPI_KTIME_H */
