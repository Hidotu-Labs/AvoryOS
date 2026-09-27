#ifndef ARCH_X86_64_EXTABLE_H
#define ARCH_X86_64_EXTABLE_H

#include "cpu/isr.h"
#include <stdbool.h>
#include <stdint.h>

struct exception_table_entry {
  uint64_t insn;
  uint64_t fixup;
};

bool extable_has_entry(uint64_t rip);
bool extable_fixup(struct registers *regs);

#endif
