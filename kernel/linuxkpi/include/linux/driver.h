#ifndef __AVORY_LINUXKPI_DRIVER_H
#define __AVORY_LINUXKPI_DRIVER_H

/* Minimal Linux <linux/driver.h> / driver-core overlay.
 *
 * Registration is bookkeeping only: there is no bus matching yet, so
 * driver_register() records the driver and returns success.  probe/remove are
 * called by whoever owns the device binding until the real driver core lands
 * (Phase 3). */

#include <linux/device.h>
#include <linux/mod_devicetable.h>

/* struct device_driver is defined in the device.h overlay, matching
 * upstream's device.h -> device/driver.h chain. */

int driver_register(struct device_driver *drv);
void driver_unregister(struct device_driver *drv);
int driver_attach(struct device_driver *drv);

/* module_driver() is defined in the device.h overlay (as upstream's
 * device/driver.h). */

#endif /* __AVORY_LINUXKPI_DRIVER_H */
