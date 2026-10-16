#ifndef AVORY_NAMESPACE_H
#define AVORY_NAMESPACE_H

#include <stdint.h>
struct thread;
struct uts_namespace;
struct ipc_namespace;
uint32_t namespace_allocate_id(void);
struct ipc_namespace *ipc_namespace_create(void);
void ipc_namespace_put(struct ipc_namespace *ns);
void ipc_namespace_inherit(struct thread *child, struct thread *parent);
void ipc_namespace_install(struct thread *task, struct ipc_namespace *ns);
void ipc_namespace_release(struct thread *task);
uint32_t ipc_namespace_id(struct thread *task);
static inline void ipc_namespace_cleanup(struct ipc_namespace **ns) {
  ipc_namespace_put(*ns);
}

/* NULL denotes the immortal initial UTS namespace. */
struct uts_namespace *uts_namespace_copy(struct thread *source);
void uts_namespace_put(struct uts_namespace *ns);
void uts_namespace_inherit(struct thread *child, struct thread *parent);
void uts_namespace_install(struct thread *task, struct uts_namespace *ns);
void uts_namespace_release(struct thread *task);
uint32_t uts_namespace_id(struct thread *task);
void uts_namespace_read(struct thread *task, char hostname[65], char domain[65]);
int uts_namespace_set(struct thread *task, const char *name, uint32_t length,
                       int domain);
static inline void uts_namespace_cleanup(struct uts_namespace **ns) {
  uts_namespace_put(*ns);
}
#endif
