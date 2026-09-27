#ifndef CPU_KPF_DUMP_H
#define CPU_KPF_DUMP_H

#include <stdint.h>
#include "isr.h"

// Report a kernel-mode page fault that the paging engine refused, going
// straight to the serial port without taking a single lock. Called right
// before the panic, because the panic itself only writes to the console.
void kpf_dump_page_fault(struct registers *regs, uint64_t cr2);

// One lock-free line announcing that a panic started, for the case where the
// console dump that follows never gets finished.
void kpf_dump_panic_entry(const char *reason, struct registers *regs);

// Panic on top of a panic that was still printing. The console path is proven
// unusable at this point, so this is all that goes out before halting.
void kpf_dump_panic_recursion(const char *reason, struct registers *regs);

#endif
