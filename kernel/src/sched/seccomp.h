#ifndef AVORY_SECCOMP_H
#define AVORY_SECCOMP_H
#include <stdbool.h>
#include <stdint.h>
struct thread;
struct syscall_regs;
struct seccomp_filter;
int64_t seccomp_control(uint32_t op, uint32_t flags, uint64_t args);
bool seccomp_syscall(struct thread *t, struct syscall_regs *regs,
                      uint32_t nr, uint32_t arch, const uint64_t args[6]);
void seccomp_inherit(struct thread *child, struct thread *parent);
void seccomp_release_task(struct thread *t);
int seccomp_mode(struct thread *t);
#endif
