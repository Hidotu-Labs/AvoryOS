#ifndef __AVORY_LINUXKPI_TIME_H
#define __AVORY_LINUXKPI_TIME_H

/* Linux <linux/time.h> overlay: UAPI clock IDs/structures plus the kernel
 * time64 helpers.  The rest of upstream's time.h (timekeeping internals)
 * arrives with the timekeeping work. */

#include <linux/types.h>
#include <linux/time64.h>
#include <uapi/linux/time.h>
#include_next <linux/time.h>

#endif /* __AVORY_LINUXKPI_TIME_H */
