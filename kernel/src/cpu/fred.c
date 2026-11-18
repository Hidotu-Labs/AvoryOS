#include "fred.h"
#include "../console/klog.h"
#include "../smp/cpu.h"
#include "features.h"
#include "gdt.h"
#include "msr.h"

extern void fred_entry_user(void);

void fred_init_cpu(void) {
  if (!cpu_has_fred())
    return;

  struct cpu_info *ci = cpu_get_current();
  uint32_t cpu_id = ci ? ci->cpu_id : 0;
  uint64_t rsp0 = ci ? (ci->stack_top & ~0x3FULL) : 0;
  uint64_t rsp1 = gdt_get_df_stack_top(cpu_id);

  /* Stack level 1 for NMI (2), #DF (8), and #MC (18); level 0 for all others. */
  uint64_t stklvls = (1ULL << (2 * 2)) | (1ULL << (8 * 2)) | (1ULL << (18 * 2));

  wrmsr(IA32_FRED_RSP0, rsp0);
  wrmsr(IA32_FRED_RSP1, rsp1);
  wrmsr(IA32_FRED_RSP2, rsp1);
  wrmsr(IA32_FRED_RSP3, rsp1);
  wrmsr(IA32_FRED_STKLVLS, stklvls);

  /* fred_entry_user is 4 KiB aligned; fred_entry_kernel sits at +256. */
  wrmsr(IA32_FRED_CONFIG, ((uint64_t)fred_entry_user) & ~0xFFFULL);

  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= CR4_FRED;
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

  if (cpu_id == 0) {
    __atomic_store_n(&cpu_has_fred_flag, true, __ATOMIC_RELEASE);
    klog_puts("[  OK  ] Intel FRED enabled on BSP.\n");
  }
}
