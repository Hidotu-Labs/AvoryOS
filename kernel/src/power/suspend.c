#include "suspend.h"

#include "../acpi/acpi.h"
#include "../apic/ioapic.h"
#include "../apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "../cpu/features.h"
#include "../cpu/fpu.h"
#include "../cpu/gdt.h"
#include "../cpu/idt.h"
#include "../cpu/irq.h"
#include "../cpu/isr.h"
#include "../cpu/pic.h"
#include "../drivers/manager/device.h"
#include "../drivers/pci/pci.h"
#include "../drivers/serial.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"
#include "../syscalls/syscall.h"
#include "../console/klog.h"
#include "../sched/sched.h"
#include "../hal/hal.h"

/* Implemented by the fd syscall layer; syncs this shared file table before
 * storage devices are asked to suspend. */
extern void sys_sync_all_open_files(void);

#define POWER_S3_AP_TIMEOUT_MS 1000

static bool s3_prepared;
extern int power_s3_enter_arch(void);

int power_s3_prepare(uint64_t resume_rsp, uint64_t resume_rip) {
  if (s3_prepared || !resume_rsp || !resume_rip)
    return -16;

  if (!acpi_s3_supported()) {
    klog_puts("[S3] Refusing suspend: ACPI S3 wake or PM1 control is unavailable.\n");
    return -95;
  }

  // Flush while every CPU and storage controller is still online.
  sys_sync_all_open_files();

  if (!cpu_quiesce_aps(POWER_S3_AP_TIMEOUT_MS)) {
    klog_puts("[S3] Refusing suspend: an application processor is busy.\n");
    return -16;
  }

  /* All APs are idle and parked. Freeze BSP scheduling before any driver is
   * quiesced so no other task can touch hardware midway through the callbacks. */
  if (!sched_power_freeze_current()) {
    cpu_release_aps();
    return -16;
  }

  int result = dm_suspend_devices();
  if (result) {
    sched_power_thaw_current();
    cpu_release_aps();
    return result;
  }

  pci_save_platform_s3_state();

  if (!cpu_prepare_s3_wake(resume_rsp, resume_rip)) {
    klog_puts("[S3] Refusing suspend: low-memory wake vector setup failed.\n");
    (void)dm_resume_devices();
    sched_power_thaw_current();
    cpu_release_aps();
    return -95;
  }

  s3_prepared = true;
  return 0;
}

void power_s3_before_sleep(void) {
  if (s3_prepared)
    lapic_timer_suspend_for_s3();
}

/* The real-mode trampoline restores the BSP's kernel CR3 and jumps here with
 * interrupts disabled. Rebuild the architectural state that firmware may
 * have reset before any device callback or AP is allowed to run. */
void power_s3_restore_bsp_early(void) {
  __asm__ volatile("clts" ::: "memory");
  cpu_features_init();
  serial_resume();
  gdt_reload_bsp();
  idt_load();
  cpu_restore_s3_context();
  fpu_init_cpu();
  syscall_init_cpu();

  pci_restore_platform_s3_state();
  pic_remap(32, 40);
  pic_disable();

  lapic_init((uint64_t)acpi_get_lapic_base());
  uint32_t ioapic_base = acpi_get_ioapic_base();
  if (ioapic_base)
    ioapic_init((uint64_t)ioapic_base, acpi_get_ioapic_gsi_base());
  isr_set_apic_mode(true);
  irq_manager_sync();
  lapic_timer_resume_from_s3(true);
}

void power_s3_cancel(void) {
  if (!s3_prepared)
    return;
  cpu_cancel_s3_wake();
  lapic_timer_resume_from_s3(false);
  hal_irq_enable();
  (void)dm_resume_devices();
  cpu_release_aps();
  sched_power_thaw_current();
  s3_prepared = false;
}

int power_s3_resume(void) {
  if (!s3_prepared)
    return -22;
  cpu_cancel_s3_wake();
  hal_irq_enable();
  cpu_resume_aps();
  sched_resume_from_s3();
  int result = dm_resume_devices();
  sched_power_thaw_current();
  s3_prepared = false;
  return result;
}

int power_s3_enter(void) { return power_s3_enter_arch(); }
