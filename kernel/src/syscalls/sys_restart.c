/* Translate Linux's internal restart results at native syscall return.
 * Drivers can abort an interruptible operation with -ERESTARTSYS. That
 * number is a kernel protocol, not an errno that may escape to libc. */
#include "syscall.h"
#include "cpu/isr.h"
#include "sched/sched.h"

void syscall_restart_return(struct registers *regs, uint64_t number,
                             uint64_t return_ip, bool handler,
                             uint64_t action_flags) {
  if (number == UINT64_MAX || !syscall_restart_error(regs->rax))
    return;

  int64_t result = (int64_t)regs->rax;
  bool restart = result == -513 ||
                 (result == -512 && (!handler || (action_flags & SA_RESTART))) ||
                 (result == -514 && !handler);
  /* Restart blocks need saved operation state (e.g. a remaining timeout).
   * None is implemented by the native syscall layer yet. Return EINTR
   * rather than expose the internal code or replay an unsafe partial call.
   * A redirected rseq/fault return must also keep its redirected RIP. */
  if (!restart || return_ip < 2 || regs->rip != return_ip) {
    regs->rax = (uint64_t)-4;
    return;
  }

  regs->rax = number;
  regs->rip = return_ip - 2; /* native x86-64 SYSCALL instruction */
}
