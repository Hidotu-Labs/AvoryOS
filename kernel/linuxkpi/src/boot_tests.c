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

  complete(&boot_tests_done);
  return 0;
}

void linuxkpi_run_boot_tests(void) {
  init_completion(&boot_tests_done);

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
  if (wait_for_completion_timeout(&boot_tests_done, 30000) == 0)
    klog_puts("[WARN] LinuxKPI: boot self-tests timed out\n");
}
