/* Kernel-mode WARN/BUG reporting for imported Linux code.
 *
 * Stock <asm/bug.h> has no calls: BUG()/WARN() emit `ud2` plus an entry in
 * the `__bug_table` section, and the kernel is expected to decode the entry
 * from the invalid-opcode exception.  This file does that:
 *
 *   - match the faulting RIP against the table
 *   - WARN(): print the file:line and resume at RIP + 2 (length of ud2)
 *   - BUG(): print the location and let the ISR panic path stop the machine
 *
 * BUGFLAG_ONCE is honoured with a small address registry; the table itself
 * lives in .rodata and is not written to.  The message format mirrors the
 * native stub's "WARNING at file:line" so existing log greps keep working. */

#include "cpu/bug_table.h"

#include "console/klog.h"
#include "cpu/isr.h"

/* Layout must match <asm-generic/bug.h> (see cpu/bug_table.h). */
struct asc_bug_entry {
  int32_t bug_addr_disp;
  int32_t file_disp;
  uint16_t line;
  uint16_t flags;
};

#define ASC_BUGFLAG_WARNING (1u << 0)
#define ASC_BUGFLAG_ONCE (1u << 1)

extern const struct asc_bug_entry __start___bug_table[];
extern const struct asc_bug_entry __stop___bug_table[];

#define ASC_BUG_ONCE_MAX 64
static uint64_t once_reported[ASC_BUG_ONCE_MAX];
static int once_count;

static bool bug_once_seen(uint64_t addr) {
  for (int i = 0; i < once_count; i++)
    if (once_reported[i] == addr)
      return true;
  return false;
}

static void bug_once_add(uint64_t addr) {
  if (once_count < ASC_BUG_ONCE_MAX)
    once_reported[once_count++] = addr;
}

bool asc_bug_handle_invalid_opcode(struct registers *regs) {
  const uint8_t *insn = (const uint8_t *)regs->rip;

  /* Only ud2 has a table entry; everything else is a real invalid opcode. */
  if (insn[0] != 0x0f || insn[1] != 0x0b)
    return false;

  for (const struct asc_bug_entry *entry = __start___bug_table;
       entry < __stop___bug_table; entry++) {
    uint64_t bug_addr =
        (uint64_t)(const void *)&entry->bug_addr_disp + (int64_t)entry->bug_addr_disp;

    if (bug_addr != regs->rip)
      continue;

    const char *file =
        (const char *)&entry->file_disp + (int64_t)entry->file_disp;
    if (!file || entry->line == 0)
      file = "<unknown>";

    if (entry->flags & ASC_BUGFLAG_WARNING) {
      if ((entry->flags & ASC_BUGFLAG_ONCE) && bug_once_seen(bug_addr)) {
        regs->rip += 2;
        return true;
      }
      if (entry->flags & ASC_BUGFLAG_ONCE)
        bug_once_add(bug_addr);
      klogf("\x1b[33mWARNING: at %s:%u (imported WARN)\x1b[0m\n", file,
            entry->line);
      regs->rip += 2;
      return true;
    }

    /* BUG(): make the location visible before the panic dump. */
    klogf("\x1b[31m[BUG] at %s:%u (imported BUG)\x1b[0m\n", file,
          entry->line);
    return false;
  }
  return false;
}
