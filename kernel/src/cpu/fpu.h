#ifndef CPU_FPU_H
#define CPU_FPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../cpu/isr.h"
struct thread;

void fpu_init(void);
void fpu_init_cpu(void);
void fpu_init_thread(struct thread *t);
bool fpu_validate_legacy(const uint8_t fx[512], const uint8_t *cur_fpu_state);
void fpu_sanitize_xstate(uint8_t *state, size_t state_size,
                         const uint8_t *cur_fpu_state);
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
