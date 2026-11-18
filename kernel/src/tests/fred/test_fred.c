#include "../../apic/lapic.h"
#include "../../console/klog.h"
#include "../../cpu/features.h"
#include "../../cpu/fred.h"
#include "../../cpu/isr.h"
#include "../../cpu/msr.h"
#include <stdbool.h>
#include <stdint.h>

extern void fred_entry_user(void);

static volatile int g_fred_bp_hits = 0;
static volatile int g_fred_nmi_hits = 0;
static volatile uint64_t g_fred_nmi_rsp = 0;

static void fred_test_bp_handler(struct registers *regs) {
  (void)regs;
  g_fred_bp_hits++;
}

static void fred_test_nmi_handler(struct registers *regs) {
  g_fred_nmi_rsp = (uint64_t)regs;
  g_fred_nmi_hits++;
}

void test_fred(void) {
  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  if (!cpu_has_fred()) {
    if (cpu_has_fred_flag || (cr4 & CR4_FRED)) {
      klog_puts(KLOG_CLR_RED
                "[FRED TEST] FAIL: FRED active without CPUID bit\n"
                KLOG_CLR_RESET);
      return;
    }
    klog_puts("[FRED TEST] SKIP: CPU does not advertise FRED.\n");
    return;
  }

  if (!cpu_has_fred_flag || !(cr4 & CR4_FRED)) {
    klog_puts(KLOG_CLR_RED "[FRED TEST] FAIL: CR4.FRED not enabled\n"
              KLOG_CLR_RESET);
    return;
  }

  uint64_t cfg = rdmsr(IA32_FRED_CONFIG);
  uint64_t rsp0 = rdmsr(IA32_FRED_RSP0);
  uint64_t rsp1 = rdmsr(IA32_FRED_RSP1);
  uint64_t stklvls = rdmsr(IA32_FRED_STKLVLS);

  if ((cfg & ~0xFFFULL) != ((uint64_t)fred_entry_user & ~0xFFFULL)) {
    klog_puts(KLOG_CLR_RED "[FRED TEST] FAIL: IA32_FRED_CONFIG mismatch\n"
              KLOG_CLR_RESET);
    return;
  }

  if (!rsp0 || (rsp0 & 0x3FULL) || !rsp1 || (rsp1 & 0x3FULL)) {
    klog_puts(KLOG_CLR_RED "[FRED TEST] FAIL: unaligned FRED RSP0/RSP1\n"
              KLOG_CLR_RESET);
    return;
  }

  uint64_t expected_stklvls =
      (1ULL << (2 * 2)) | (1ULL << (8 * 2)) | (1ULL << (18 * 2));
  if (stklvls != expected_stklvls) {
    klog_puts(KLOG_CLR_RED "[FRED TEST] FAIL: IA32_FRED_STKLVLS mismatch\n"
              KLOG_CLR_RESET);
    return;
  }

  /* Exercise Ring 0 -> Ring 0 FRED delivery (fred_entry_kernel + erets). */
  g_fred_bp_hits = 0;
  register_interrupt_handler(3, fred_test_bp_handler);
  __asm__ volatile("int3");
  register_interrupt_handler(3, 0);

  if (g_fred_bp_hits != 1) {
    klog_puts(KLOG_CLR_RED
              "[FRED TEST] FAIL: Ring-0 int3 did not round-trip via FRED\n"
              KLOG_CLR_RESET);
    return;
  }

  /* Exercise stack level 1 (NMI via LAPIC self-IPI on IA32_FRED_RSP1). */
  g_fred_nmi_hits = 0;
  g_fred_nmi_rsp = 0;
  register_interrupt_handler(2, fred_test_nmi_handler);
  lapic_send_nmi(lapic_get_id());
  for (volatile int i = 0; i < 1000000 && !g_fred_nmi_hits; i++) {
    __asm__ volatile("pause");
  }
  isr_init_exceptions();

  if (g_fred_nmi_hits != 1) {
    klog_puts(KLOG_CLR_RED
              "[FRED TEST] FAIL: Self-NMI did not deliver via FRED\n"
              KLOG_CLR_RESET);
    return;
  }
  if (g_fred_nmi_rsp > rsp1 || g_fred_nmi_rsp < (rsp1 - 8192)) {
    klog_puts(KLOG_CLR_RED
              "[FRED TEST] FAIL: NMI did not switch to IA32_FRED_RSP1\n"
              KLOG_CLR_RESET);
    return;
  }

  klog_puts(KLOG_CLR_GREEN "[FRED TEST] PASS\n" KLOG_CLR_RESET);
}
