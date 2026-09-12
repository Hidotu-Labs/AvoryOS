/* Initcall walker, Phase 0.  See linuxkpi/initcall.h for the mechanism.
 *
 * This is the raw walker (linuxkpi_run_initcalls_inline()).  The public
 * linuxkpi_run_initcalls() entry point lives in linuxkpi/src/initcalls.c and
 * runs this from a kernel thread, as Linux runs module_init in kernel_init's
 * thread rather than the BSP idle context. */

#include <stddef.h>
#include <stdint.h>

#include "console/klog.h"
#include "linuxkpi/initcall.h"

extern linuxkpi_initcall_fn_t __initcall_start[];
extern linuxkpi_initcall_fn_t __initcall_end[];

void linuxkpi_run_initcalls_inline(void) {
  size_t total = (size_t)(__initcall_end - __initcall_start);

  if (total == 0) {
    klog_puts("[KERNEL] LinuxKPI: no initcalls registered\n");
    return;
  }

  klogf("[KERNEL] LinuxKPI: running %llu initcall(s)...\n",
        (unsigned long long)total);

  size_t failed = 0;
  for (size_t i = 0; i < total; i++) {
    linuxkpi_initcall_fn_t fn = __initcall_start[i];
    if (!fn)
      continue;
    klog_puts("[KERNEL] LinuxKPI: initcall #");
    klog_uint64(i);
    klog_puts(" @ ");
    klog_hex64((uint64_t)(uintptr_t)fn);
    klog_puts("\n");
    int rc = fn();
    if (rc != 0) {
      failed++;
      klogf("[KERNEL] LinuxKPI: initcall #%llu returned %d\n",
            (unsigned long long)i, rc);
    }
  }

  if (failed) {
    klogf("[KERNEL] LinuxKPI: %llu initcall(s) failed\n",
          (unsigned long long)failed);
  } else {
    klog_puts("[KERNEL] LinuxKPI: all initcalls completed\n");
  }
}
