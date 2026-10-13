#include "syscall.h"
#include "../include/arch/uaccess.h"
#include "../sched/sched.h"

int syscall_user_dispatch_configure(struct thread *t, uint64_t mode,
                                    uint64_t offset, uint64_t length,
                                    uint64_t selector) {
  if (mode == 0) {
    if (offset || length || selector)
      return -22; /* EINVAL */
  } else if (mode == 1) {
    /* Linux validates arithmetic, not mapping residency of the native
     * interval. An empty interval is permitted only with offset zero. */
    if (offset && offset + length <= offset)
      return -22;
    if (selector && !is_user_range((void *)selector, 1))
      return -14; /* EFAULT; the byte is read at syscall entry */
  } else {
    return -22;
  }
  t->syscall_dispatch_offset = offset;
  t->syscall_dispatch_length = length;
  t->syscall_dispatch_selector = selector;
  t->syscall_dispatch_sigsys = false;
  t->syscall_dispatch_enabled = mode != 0;
  return 0;
}

static void dispatch_force_exit(struct thread *t, int sig) {
  t->exit_status = 128 + sig;
  sched_terminate_thread_group(t);
  process_do_exit(128 + sig);
}

bool syscall_user_dispatch_intercept(struct thread *t, struct syscall_regs *regs) {
  if (!t->syscall_dispatch_enabled ||
      regs->rip - t->syscall_dispatch_offset < t->syscall_dispatch_length)
    return false;

  if (t->syscall_dispatch_selector) {
    uint8_t selector;
    if (copy_from_user(&selector, (void *)t->syscall_dispatch_selector, 1)) {
      dispatch_force_exit(t, SIGSEGV);
      return true;
    }
    if (selector == 0)
      return false;
    if (selector != 1) {
      dispatch_force_exit(t, SIGSYS);
      return true;
    }
  }

  /* A blocked/ignored synchronous SIGSYS cannot emulate this syscall. */
  uint64_t bit = 1ULL << (SIGSYS - 1);
  if ((t->signal_mask & bit) ||
      t->signal_handlers[SIGSYS - 1].sa_handler == (void *)SIG_IGN) {
    dispatch_force_exit(t, SIGSYS);
    return true;
  }
  t->syscall_dispatch_sigsys = true;
  t->syscall_dispatch_call_addr = regs->rip;
  t->syscall_dispatch_number = (int32_t)regs->rax;
  t->signal_sender_pid[SIGSYS - 1] = 0;
  t->pending_signals |= bit;
  /* Keep the original RAX/arguments and post-SYSCALL RIP in the ucontext.
   * The handler supplies a return value; no Linux syscall is executed. */
  signal_deliver_syscall(regs);
  return true;
}
