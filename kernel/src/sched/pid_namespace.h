#ifndef SCHED_PID_NAMESPACE_H
#define SCHED_PID_NAMESPACE_H
#include <stdint.h>
#include <stdbool.h>
struct thread;
struct pid_namespace;
#define PID_NS_MAX_DEPTH 32
struct pid_namespace *pid_namespace_create(struct thread *parent);
void pid_namespace_put(struct pid_namespace *ns);
/* Install consumes ns. NEWPID changes only future children on unshare. */
void pid_namespace_unshare(struct thread *task, struct pid_namespace *ns);
int pid_namespace_inherit(struct thread *child, struct thread *parent,
                          struct pid_namespace *new_ns, bool thread);
void pid_namespace_release(struct thread *task);
void pid_namespace_exit(struct thread *task);
bool pid_namespace_signal_allowed(struct thread *sender, struct thread *target, int signal);
uint32_t pid_namespace_id(struct thread *task);
uint32_t pid_namespace_tid(struct thread *viewer, struct thread *target);
/* Global ID conversion: locked variants require tid_lock, others acquire it. */
uint32_t pid_namespace_global_locked(struct thread *viewer, uint32_t local);
uint32_t pid_namespace_visible_locked(struct thread *viewer, uint32_t global);
uint32_t pid_namespace_global(struct thread *viewer, uint32_t local);
uint32_t pid_namespace_visible(struct thread *viewer, uint32_t global);
bool pid_namespace_nth(struct thread *viewer, uint32_t index, uint32_t *global);
static inline void pid_namespace_cleanup(struct pid_namespace **ns) { pid_namespace_put(*ns); }
#endif
