#ifndef __AVORY_LINUXKPI_DEVICE_DRIVER_H
#define __AVORY_LINUXKPI_DEVICE_DRIVER_H

/* AvoryOS overlay for <linux/device/driver.h>.
 *
 * struct device_driver lives in the linux/device.h overlay; the stock
 * split header would define a second, conflicting struct.  Redirect. */

#include <linux/device.h>

#endif /* __AVORY_LINUXKPI_DEVICE_DRIVER_H */
