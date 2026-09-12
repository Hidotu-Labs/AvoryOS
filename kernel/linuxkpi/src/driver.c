/* LinuxKPI driver-core stubs.  See linux/driver.h. */

#include <linux/driver.h>

int driver_register(struct device_driver *drv) {
  (void)drv;
  return 0;
}

void driver_unregister(struct device_driver *drv) { (void)drv; }

int driver_attach(struct device_driver *drv) {
  (void)drv;
  return 0;
}
