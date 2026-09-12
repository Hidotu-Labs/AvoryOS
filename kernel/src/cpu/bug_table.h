#ifndef CPU_BUG_TABLE_H
#define CPU_BUG_TABLE_H

#include <stdbool.h>
#include <stdint.h>

struct registers;

/* Kernel-mode handling for the stock Linux BUG/WARN trap table.
 *
 * Imported Linux code does not use the native <linux/bug.h> stub: stock
 * <asm/bug.h> emits `ud2` followed by an entry in the `__bug_table` section
 * (kept by the linker script, see kernel/linker-scripts/x86_64.lds).  The
 * entry layout below must match `struct bug_entry` from
 * include/asm-generic/bug.h with CONFIG_GENERIC_BUG_RELATIVE_POINTERS and
 * CONFIG_DEBUG_BUGVERBOSE, which is what linuxkpi/include/generated/
 * autoconf.h selects.
 *
 * asc_bug_handle_invalid_opcode() returns true when the fault was a WARN()
 * and execution can resume after the ud2; false means the invalid-opcode
 * handler should panic (BUG() or a genuine bad instruction). */
bool asc_bug_handle_invalid_opcode(struct registers *regs);

#endif /* CPU_BUG_TABLE_H */
