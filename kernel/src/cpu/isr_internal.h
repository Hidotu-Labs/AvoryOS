#ifndef CPU_ISR_INTERNAL_H
#define CPU_ISR_INTERNAL_H

#include "isr.h"

bool isr_interrupt_dispatch(struct registers *regs);
void isr_record_vector_delivery(uint32_t cpu_id, uint32_t vector);
void isr_panic(struct registers *regs, const char *msg);

#endif