#ifndef SYS_PROCESS_INTERNAL_H
#define SYS_PROCESS_INTERNAL_H

#include "../fs/vfs.h"
#include "../sched/wait.h"
#include <stdbool.h>
#include <stdint.h>

struct thread;
struct mount_namespace;

/* Shared process syscall implementation details. These interfaces connect
 * lifecycle modules without exposing their handlers outside syscalls/. */
void syscall_register_process_clone(void);
void syscall_register_process_exec(void);
void syscall_register_process_exit(void);
void syscall_register_process_wait(void);
void syscall_register_pidfd(void);

uint8_t process_nice_to_sched_priority(int nice);
void process_mount_cleanup(struct mount_namespace **ns);

int64_t process_pidfd_reserve(struct thread *t, int *out_fd,
                              vfs_node_t **out_node);
void process_pidfd_publish(struct thread *t, int fd, vfs_node_t *node,
                           uint32_t pid, uint64_t flags);
void process_pidfd_abort(struct thread *t, int fd, vfs_node_t *node);
int process_pidfd_target(struct thread *t, uint32_t fd, uint32_t *pid_out);
wait_queue_t *process_pidfd_wait_queue(void);
void pidfd_wake_waiters(void);

#endif
