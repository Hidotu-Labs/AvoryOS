#ifndef __AVORY_LINUXKPI_DEVICE_BUS_H
#define __AVORY_LINUXKPI_DEVICE_BUS_H

/* AvoryOS overlay for <linux/device/bus.h>.
 *
 * struct bus_type lives in the linux/device.h overlay; the stock split header
 * would define a second, conflicting struct (and redefines pm_message_t).
 * Redirect. */

#include <linux/device.h>

#endif /* __AVORY_LINUXKPI_DEVICE_BUS_H */
