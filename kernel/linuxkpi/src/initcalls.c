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
#include <linuxkpi/native_sched.h>

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
  unsigned long long start, elapsed;
  unsigned int waited_ms = 0;
  bool completed = false;

  init_completion(&initcalls_done);
  start = linuxkpi_monotonic_ms();

  task = kthread_run(linuxkpi_initcalls_thread, NULL, "kpi/initcalls");
  if (IS_ERR(task)) {
    klog_puts("[WARN] LinuxKPI: could not create the initcall thread; "
              "running initcalls inline (blocking may misbehave)\n");
    linuxkpi_pci_scan();
    linuxkpi_run_initcalls_inline();
    return;
  }

  /* Bounded wait (this shim's jiffy is 1 ms): a wedged driver must not hold
   * up the rest of the boot forever, but the timeout is reported loudly.
   * 10 minutes because a single amdgpu probe legitimately runs minutes on
   * the passed-through GPU (PSP firmware loads, SMU/DMUB handshakes, DCN
   * register bring-up in this emulation); running the suites while it is
   * still probing makes them fight over the device (P6 C4: the VFIO suite
   * lost its MSI vectors with -EBUSY).  Progress is logged every 30 s so a
   * long-but-working probe is distinguishable from a wedge. */
  while (waited_ms < 600000) {
    if (wait_for_completion_timeout(&initcalls_done, 2000) > 0) {
      completed = true;
      break;
    }
    waited_ms += 2000;
    if ((waited_ms % 30000) == 0)
      klogf("[INFO] LinuxKPI: initcalls still running (%u s)\n",
            waited_ms / 1000);
  }

  elapsed = linuxkpi_monotonic_ms() - start;
  if (completed) {
    klogf("[INFO] LinuxKPI: initcalls completed in %lu.%03lu s\n",
          (unsigned long)(elapsed / 1000),
          (unsigned long)(elapsed % 1000));
    /* Join the walker thread so its kthread control block is freed now; the
     * native thread itself is reaped by thread_exit().  A timed-out walker
     * may still be probing, so it is left alone. */
    kthread_stop(task);
  } else {
    klogf("[WARN] LinuxKPI: initcalls timed out after %lu s\n",
          (unsigned long)(elapsed / 1000));
  }
}
