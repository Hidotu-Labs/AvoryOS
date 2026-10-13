#ifndef CPU_FPU_H
#define CPU_FPU_H

#include "../cpu/isr.h"
struct thread;

void fpu_init(void);
void fpu_init_cpu(void);
void fpu_init_thread(struct thread *t);
void fpu_nm_handler(struct registers *regs);
/* Local CPU, IRQs masked; call before publishing the arriving thread. */
void fpu_on_context_switch(void);
/* Only the current thread may be loaded; serializes ownership with IRQs. */
void fpu_ensure_loaded(const struct thread *t);
void fpu_copy_thread(struct thread *child, const struct thread *parent);
void fpu_reset_current(void);
/* Teardown is allowed only once a thread is off-CPU. */
void fpu_forget_thread(struct thread *t);

#endif
