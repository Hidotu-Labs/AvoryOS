#include "acpi/acpi.h"
#include "console/klog.h"
#include "hal/hal.h"
#include <linuxkpi/native_power.h>

void asc_emergency_poweroff(void) {
  /* A thermal worker must not wait for its own GPU's workqueue during
   * PCI shutdown. Emergency power-off deliberately skips driver cleanup. */
  klog_puts("[POWER] LinuxKPI emergency guest power-off (no userspace helper)\n");
  acpi_poweroff();
  hal_irq_disable();
  for (;;) hal_cpu_halt();
}
void asc_emergency_restart(void) {
  klog_puts("[POWER] LinuxKPI emergency guest restart\n");
  acpi_reboot();
  hal_irq_disable();
  for (;;) hal_cpu_halt();
}
