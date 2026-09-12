/* Kernel FPU entry points, Phase 0.  See linuxkpi/fpu.h for the contract. */

#include <stdbool.h>
#include <stdint.h>

#include "cpu/features.h"
#include "hal/hal.h"
#include "linuxkpi/fpu.h"
#include "smp/cpu.h"

#define FPU_AREA_SIZE  4096
#define FPU_AREA_ALIGN 64

/* Per-CPU XSAVE scratch.  Interrupts are masked for the duration of a
 * begin/end section, so a thread cannot be preempted out from under its
 * buffer and another context cannot overwrite it. */
static uint8_t fpu_save_area[MAX_CPUS][FPU_AREA_SIZE]
    __attribute__((aligned(FPU_AREA_ALIGN)));
static uint32_t fpu_nesting[MAX_CPUS];
static hal_irq_state_t fpu_irq_flags[MAX_CPUS];
static bool fpu_section_active[MAX_CPUS];

static inline void fpu_save_state(void *area) {
  if (cpu_has_xsave_flag) {
    __asm__ volatile("xsave64 (%0)"
                     :
                     : "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu)
                     : "memory");
  } else {
    __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
  }
}

static inline void fpu_restore_state(const void *area) {
  if (cpu_has_xsave_flag) {
    __asm__ volatile("xrstor64 (%0)"
                     :
                     : "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu)
                     : "memory");
  } else {
    __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
  }
}

static inline uint32_t fpu_cpu_index(void) {
  struct cpu_info *ci = cpu_get_current();
  if (!ci || ci->cpu_id >= MAX_CPUS)
    return 0;
  return ci->cpu_id;
}

bool irq_fpu_usable(void) { return true; }

/* x86_64 upstream entry point: the mask selects which state components to
 * initialize; AvoryOS always saves the full XSAVE area and lets the caller
 * run, which is a superset. */
void kernel_fpu_begin_mask(unsigned int kfpu_mask) {
  (void)kfpu_mask;
  kernel_fpu_begin();
}

void kernel_fpu_begin(void) {
  uint32_t cpu = fpu_cpu_index();

  if (fpu_section_active[cpu]) {
    fpu_nesting[cpu]++;
    return;
  }

  fpu_irq_flags[cpu] = hal_irq_save();
  fpu_save_state(fpu_save_area[cpu]);
  fpu_nesting[cpu] = 1;
  fpu_section_active[cpu] = true;
}

void kernel_fpu_end(void) {
  uint32_t cpu = fpu_cpu_index();

  if (!fpu_section_active[cpu])
    return; /* unbalanced end: better to leak state than corrupt it */

  if (--fpu_nesting[cpu] > 0)
    return;

  fpu_restore_state(fpu_save_area[cpu]);
  fpu_section_active[cpu] = false;
  hal_irq_restore(fpu_irq_flags[cpu]);
}

bool linuxkpi_fpu_selftest(void) {
  static const uint8_t magic[16] __attribute__((aligned(16))) = {
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
      0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};
  uint8_t out[16] __attribute__((aligned(16)));

  /* No "xmm0" clobbers: the compiler rejects them under -mno-sse, and it
   * cannot allocate XMM registers in this translation unit anyway.  The asm
   * statements are volatile, so they stay ordered around the calls. */
  __asm__ volatile("movdqu (%0), %%xmm0" : : "r"(magic) : "memory");
  kernel_fpu_begin();
  __asm__ volatile("pxor %%xmm0, %%xmm0" ::: "memory");
  kernel_fpu_end();
  __asm__ volatile("movdqu %%xmm0, (%0)" : : "r"(out) : "memory");

  for (int i = 0; i < 16; i++) {
    if (out[i] != magic[i])
      return false;
  }
  return true;
}
