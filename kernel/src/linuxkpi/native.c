/* Native bridge helpers for LinuxKPI implementation files.  Compiled with
 * native headers only; see linuxkpi/native.h for the contract. */

#include "hal/hal.h"
#include "smp/cpu.h"

int linuxkpi_cpu_id(void) {
  struct cpu_info *ci = cpu_get_current();
  if (!ci || ci->cpu_id >= MAX_CPUS)
    return 0;
  return (int)ci->cpu_id;
}

int linuxkpi_max_cpus(void) { return MAX_CPUS; }

void linuxkpi_cpu_relax(void) {
  /* x86 PAUSE hint, the native equivalent of cpu_relax(). */
  __asm__ volatile("pause" ::: "memory");
}

unsigned long linuxkpi_irq_flags(void) {
  unsigned long flags;
  __asm__ volatile("pushfq; popq %0" : "=r"(flags));
  return flags;
}

extern const char *kernel_boot_cmdline;

const char *linuxkpi_boot_cmdline(void) {
  return kernel_boot_cmdline ? kernel_boot_cmdline : "";
}
