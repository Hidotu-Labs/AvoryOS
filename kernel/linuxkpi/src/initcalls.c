/* Initcall orchestration: run the Phase 0 walker in a kernel thread.
 *
 * Linux runs level-6 module_init from kernel_init's thread, and imported
 * drivers may sleep during probe.  kmain_high_half occupies the BSP idle
 * context, where sched_schedule() treats the idle thread as the always
 * available fallback and resumes it regardless of state or wakeup deadline,
 * so blocking there behaves differently from a real driver thread.  Spawn a
 * kthread instead and only return once the walker has finished, because the
 * boot self-tests exercise the drivers the initcalls install. */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kthread.h>

#include <linuxkpi/log.h>

extern void linuxkpi_run_initcalls_inline(void);
extern void linuxkpi_pci_scan(void);

static struct completion initcalls_done;

static int linuxkpi_initcalls_thread(void *arg) {
  (void)arg;

  /* Wrap the native PCI devices before any driver initcall can register with
   * the LinuxKPI PCI bus (Phase 4 C4), so a driver's probe sees them. */
  linuxkpi_pci_scan();
  linuxkpi_run_initcalls_inline();
  complete(&initcalls_done);
  return 0;
}

void linuxkpi_run_initcalls(void) {
  struct task_struct *task;

  init_completion(&initcalls_done);

  task = kthread_run(linuxkpi_initcalls_thread, NULL, "kpi/initcalls");
  if (IS_ERR(task)) {
    klog_puts("[WARN] LinuxKPI: could not create the initcall thread; "
              "running initcalls inline (blocking may misbehave)\n");
    linuxkpi_pci_scan();
    linuxkpi_run_initcalls_inline();
    return;
  }

  /* Bounded wait (this shim's jiffy is 1 ms): a wedged driver must not hold
   * up the rest of the boot forever, but the timeout is reported loudly. */
  if (wait_for_completion_timeout(&initcalls_done, 30000) == 0)
    klog_puts("[WARN] LinuxKPI: initcalls timed out\n");
}
