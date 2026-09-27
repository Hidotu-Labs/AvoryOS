#ifndef CPU_FPU_H
#define CPU_FPU_H

#include "../cpu/isr.h"

struct thread;

// Initialize FPU management subsystem and register Exception 7 (#NM) handler
void fpu_init(void);

// Exception 7 (#NM - Device Not Available) handler for Lazy FPU switching
void fpu_nm_handler(struct registers *regs);

// Ensure the FPU state for thread `t` is fully loaded in hardware.
// Accepts const thread* since we only read the fpu_state buffer here,
// but we do update cpu->fpu_owner so cast to non-const internally.
void fpu_ensure_loaded(const struct thread *t);

// Forget any cached FPU ownership for thread `t` (called on thread teardown)
void fpu_forget_thread(struct thread *t);

#endif

