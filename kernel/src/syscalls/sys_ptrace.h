#ifndef SYS_PTRACE_H
#define SYS_PTRACE_H

#include <stdbool.h>
#include <stdint.h>

struct registers;
struct syscall_regs;
struct thread;

void syscall_register_ptrace(void);
void ptrace_syscall_enter(struct thread *task, struct syscall_regs *regs);
void ptrace_syscall_exit(struct thread *task, struct syscall_regs *regs);
void ptrace_user_return_stop(struct registers *regs);
void ptrace_tracer_exit(struct thread *tracer);
void ptrace_tracee_exit(struct thread *tracee);
void ptrace_fork_event(struct syscall_regs *regs, struct thread *child,
                       uint32_t child_tid, uint64_t clone_flags);
void ptrace_exec_event(struct syscall_regs *regs);
void ptrace_exit_event(struct thread *tracee, int status);
void ptrace_vfork_done(struct thread *parent, uint32_t child_tid);
/* Return -1 when no tracer is attached, 0 when the signal was suppressed, or
 * the signal number the tracer selected for delivery. */
int ptrace_signal_delivery_stop(struct registers *regs, int signal,
                                const void *siginfo);

#endif
