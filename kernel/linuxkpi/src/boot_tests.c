/* Phase 1 self-test orchestration.
 *
 * The time/sync suites must run in a real kernel thread, not in the BSP idle
 * context that kmain_high_half occupies: sched_schedule() treats
 * cpu->idle_thread as the always-available fallback task and resumes it
 * regardless of its state or wakeup deadline, so blocking calls issued there
 * behave differently from every driver thread.  Kernel threads are also the
 * context that drivers use later, so blocking/timer behaviour is exercised
 * exactly as it will be in production. */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kthread.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

/* Defined by the Linux-API test translation units; see
 * kernel/src/tests/linuxkpi/linux/. */
extern void linuxkpi_test_phase1_libs(void);
extern void linuxkpi_test_phase1_mem(void);
extern void linuxkpi_test_phase1_time(void);
extern void linuxkpi_test_phase1_preempt(void);
extern void linuxkpi_test_phase1_imports(void);
extern void linuxkpi_test_phase1_params(void);
extern void linuxkpi_test_phase1_core(void);
extern void linuxkpi_test_phase2_page(void);
extern void linuxkpi_test_phase2_vmalloc(void);
extern void linuxkpi_test_phase2_dma(void);
extern void linuxkpi_test_phase2_dmabuf(void);
extern void linuxkpi_test_phase2_ttm_prep(void);
extern void linuxkpi_test_phase3_drm(void);
extern void linuxkpi_test_phase3_drm_modeset(void);
extern void linuxkpi_test_phase4_ttm(void);
extern void linuxkpi_test_phase4_sched(void);
extern void linuxkpi_test_phase4_pci(void);
extern void linuxkpi_test_phase4_bochs(void);
extern void linuxkpi_test_phase5_pci(void);
extern void linuxkpi_test_phase5_irq(void);
extern void linuxkpi_test_phase5_acpi(void);
extern void linuxkpi_test_phase5_firmware(void);
extern void linuxkpi_test_phase5_i2c(void);
extern void linuxkpi_test_phase5_sysfs(void);
extern void linuxkpi_test_phase5_devmodel(void);
extern void linuxkpi_test_phase5_ctx(void);
extern void linuxkpi_test_phase5_vfio(void);
extern void linuxkpi_test_phase6_link(void);
extern void linuxkpi_test_phase6_dcn(void);

static struct completion boot_tests_done;

static int linuxkpi_boot_tests_thread(void *arg) {
  (void)arg;

  linuxkpi_test_phase1_libs();
  linuxkpi_test_phase1_mem();
  linuxkpi_test_phase1_time();
  linuxkpi_test_phase1_preempt();
  linuxkpi_test_phase1_imports();
  linuxkpi_test_phase1_params();
  linuxkpi_test_phase1_core();
  linuxkpi_test_phase2_page();
  linuxkpi_test_phase2_vmalloc();
  linuxkpi_test_phase2_dma();
  linuxkpi_test_phase2_dmabuf();
  linuxkpi_test_phase2_ttm_prep();
  linuxkpi_test_phase3_drm();
  linuxkpi_test_phase3_drm_modeset();
  linuxkpi_test_phase4_ttm();
  linuxkpi_test_phase4_sched();
  linuxkpi_test_phase4_pci();
  linuxkpi_test_phase4_bochs();
  linuxkpi_test_phase5_pci();
  linuxkpi_test_phase5_irq();
  linuxkpi_test_phase5_acpi();
  linuxkpi_test_phase5_firmware();
  linuxkpi_test_phase5_i2c();
  linuxkpi_test_phase5_sysfs();
  linuxkpi_test_phase5_devmodel();
  linuxkpi_test_phase5_ctx();
  linuxkpi_test_phase5_vfio();
  linuxkpi_test_phase6_link();
  linuxkpi_test_phase6_dcn();

  complete(&boot_tests_done);
  return 0;
}

void linuxkpi_run_boot_tests(void) {
  init_completion(&boot_tests_done);

  unsigned long free_before = asc_pmm_get_free_pages_total();

  struct task_struct *task =
      kthread_run(linuxkpi_boot_tests_thread, NULL, "kpi/tests");
  if (IS_ERR(task)) {
    klog_puts("[WARN] LinuxKPI: could not create the boot-test thread; "
              "running the suites inline (blocking may misbehave)\n");
    linuxkpi_test_phase1_libs();
    linuxkpi_test_phase1_mem();
    linuxkpi_test_phase1_time();
    linuxkpi_test_phase1_preempt();
    linuxkpi_test_phase1_imports();
    linuxkpi_test_phase1_params();
    linuxkpi_test_phase1_core();
    return;
  }

  /* Bounded wait: a wedged suite must not hold up the boot. */
  if (wait_for_completion_timeout(&boot_tests_done, 60000) == 0) {
    klog_puts("[WARN] LinuxKPI: boot self-tests timed out\n");
  } else {
    /* The thread signals the completion on its last line, then returns and
     * runs thread_exit().  Join it so its kthread control block is released
     * now instead of being retained for the rest of the boot; the native
     * thread and its stack are reaped by thread_exit()/sched_reap_thread().
     * On timeout the thread may still be mid-suite, so leave it alone. */
    kthread_stop(task);
  }

  /* The suites run asynchronously; this is the last common point where a
   * whole-run PMM delta can be observed.  A large positive delta after the
   * final suite points at a leak in it (each suite also checks its own
   * delta; this catches allocations that only show once the test thread
   * exits or the device teardown runs). */
  unsigned long free_after = asc_pmm_get_free_pages_total();
  klogf("[INFO] LinuxKPI: boot self-tests PMM delta=%ld pages "
        "(free=%lu)\n",
        (long)free_after - (long)free_before, free_after);
}
