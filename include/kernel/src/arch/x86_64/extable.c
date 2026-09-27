#include "arch/x86_64/extable.h"
#include "../../cpu/ktrack.h"
#include <stddef.h>

extern const struct exception_table_entry __start___ex_table[];
extern const struct exception_table_entry __stop___ex_table[];

bool extable_has_entry(uint64_t rip) {
  if (rip == 0)
    return false;

  const struct exception_table_entry *entry = __start___ex_table;
  const struct exception_table_entry *end = __stop___ex_table;

  while (entry < end) {
    if (entry->insn == rip)
      return true;
    entry++;
  }

  return false;
}

bool extable_fixup(struct registers *regs) {
  if (!regs)
    return false;

  const struct exception_table_entry *entry = __start___ex_table;
  const struct exception_table_entry *end = __stop___ex_table;

  while (entry < end) {
    if (entry->insn == regs->rip) {
      KTRACK_ERR(KSUBSYS_UACCESS, -14); // EFAULT handled by fixup
      regs->rip = entry->fixup;
      return true;
    }
    entry++;
  }

  return false;
}
