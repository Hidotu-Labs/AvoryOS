// Eager FPU / SSE Context Switching
//
// FPU/SSE state is eagerly saved (fxsave64) and restored (fxrstor64) directly
// inside switch_context for each thread. This ensures complete multi-core (SMP)
// coherency across all cores and threads without stale register state.

#include "fpu.h"
#include "../cpu/isr.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "../console/klog.h"
#include <stdint.h>

// Clear CR0.TS — allow FPU/SSE instructions without fault (clts instruction)
static inline void cr0_clear_ts(void) {
    __asm__ volatile("clts" : : : "memory");
}

void fpu_nm_handler(struct registers *regs) {
    (void)regs;
    // With eager FPU switching, #NM is simply cleared
    cr0_clear_ts();
}

void fpu_on_context_switch(void) {
    // Eager FPU switches state in switch_context directly.
}

void fpu_forget_thread(struct thread *t) {
    (void)t;
}

void fpu_ensure_loaded(const struct thread *t) {
    (void)t;
    cr0_clear_ts();
}

void fpu_init(void) {
    cr0_clear_ts();
    register_interrupt_handler(7, fpu_nm_handler);
    klog_debug_puts("[FPU] Eager FPU context switching initialized\n");
}
