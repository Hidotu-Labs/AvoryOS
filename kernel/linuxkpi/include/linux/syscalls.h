#ifndef __AVORY_LINUXKPI_SYSCALLS_H
#define __AVORY_LINUXKPI_SYSCALLS_H

/* AvoryOS overlay for <linux/syscalls.h>.
 *
 * The stock header pulls the full task/ptrace/security/task_stack surface
 * that the trimmed task model does not provide, and the native syscall layer
 * is not implemented through these prototypes.  The compiled consumer
 * (amdgpu_ras.c) includes the header without using any sys_* prototype, so
 * this is a shape header.  Extend with specific declarations if an imported
 * file ever needs one. */

#endif /* __AVORY_LINUXKPI_SYSCALLS_H */
