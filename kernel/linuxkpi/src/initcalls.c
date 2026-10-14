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
#include <linux/delay.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_sched.h>

extern void linuxkpi_run_initcalls_inline(void);
extern void linuxkpi_pci_scan(void);

static struct completion initcalls_done;
static bool initcalls_finished;

static int linuxkpi_initcalls_thread(void *arg) {
  (void)arg;

  /* Wrap the native PCI devices before any driver initcall can register with
   * the LinuxKPI PCI bus (Phase 4 C4), so a driver's probe sees them. */
  linuxkpi_pci_scan();
  linuxkpi_run_initcalls_inline();
  __atomic_store_n(&initcalls_finished, true, __ATOMIC_RELEASE);
  complete(&initcalls_done);
  return 0;
}

void linuxkpi_run_initcalls(void) {
  struct task_struct *task;
  unsigned long long start, elapsed;
  unsigned int waited_ms = 0;
  bool reported_exit = false;

  init_completion(&initcalls_done);
  __atomic_store_n(&initcalls_finished, false, __ATOMIC_RELEASE);
  start = linuxkpi_monotonic_ms();

  task = kthread_run(linuxkpi_initcalls_thread, NULL, "kpi/initcalls");
  if (IS_ERR(task)) {
    klog_puts("[WARN] LinuxKPI: could not create the initcall thread; "
              "running initcalls inline (blocking may misbehave)\n");
    linuxkpi_initcalls_thread(NULL);
    return;
  }

  /* Keep the boot dependency barrier even after a slow/wedged probe. No
   * safe cancellation exists while the driver may own hardware resources. */
  for (;;) {
    unsigned long completed = wait_for_completion_timeout(&initcalls_done, 2000);
    /* A notification is not proof that the walker returned. Never open the
     * boot gate after an aborted worker or a stray completion token. */
    if (__atomic_load_n(&initcalls_finished, __ATOMIC_ACQUIRE))
      break;
    if (!READ_ONCE(task->kpi_thread) && !reported_exit) {
      klog_puts("[ERROR] LinuxKPI: initcall thread exited before finishing; "
                "boot remains blocked\n");
      reported_exit = true;
    }
    if (completed)
      msleep(2000); /* avoid spinning on unexpected completion credits */
    waited_ms += 2000;
    if ((waited_ms % 30000) == 0)
      klogf("[INFO] LinuxKPI: initcalls still running (%u s); boot waiting\n",
            waited_ms / 1000);
  }

  elapsed = linuxkpi_monotonic_ms() - start;
  klogf("[INFO] LinuxKPI: initcalls completed in %lu.%03lu s\n",
        (unsigned long)(elapsed / 1000),
        (unsigned long)(elapsed % 1000));
  kthread_stop(task);
}
