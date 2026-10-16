#include "namespace.h"
#include "sched.h"
#include "../mm/heap.h"
#include "../lib/string.h"

struct ipc_namespace { uint32_t refs, id; };
void shm_namespace_destroy(uint32_t id);
void sem_namespace_destroy(uint32_t id);

struct ipc_namespace *ipc_namespace_create(void) {
  struct ipc_namespace *ns = kmalloc(sizeof(*ns));
  if (!ns) return NULL;
  ns->refs = 1;
  ns->id = namespace_allocate_id();
  if (!ns->id) { kfree(ns); return NULL; }
  return ns;
}

void ipc_namespace_put(struct ipc_namespace *ns) {
  if (!ns || __atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL)) return;
  /* Existing mappings hold PMM refs independently of the removed ID/key.
   * They remain usable after unshare until munmap/shmdt or address-space exit. */
  shm_namespace_destroy(ns->id);
  sem_namespace_destroy(ns->id);
  kfree(ns);
}

void ipc_namespace_install(struct thread *task, struct ipc_namespace *ns) {
  spinlock_acquire(&task->namespace_lock);
  struct ipc_namespace *old = task->ipc_ns;
  task->ipc_ns = ns;
  spinlock_release(&task->namespace_lock);
  ipc_namespace_put(old);
}

void ipc_namespace_inherit(struct thread *child, struct thread *parent) {
  spinlock_acquire(&parent->namespace_lock);
  struct ipc_namespace *ns = parent->ipc_ns;
  if (ns) __atomic_add_fetch(&ns->refs, 1, __ATOMIC_RELAXED);
  spinlock_release(&parent->namespace_lock);
  ipc_namespace_install(child, ns);
}

void ipc_namespace_release(struct thread *task) { ipc_namespace_install(task, NULL); }

uint32_t ipc_namespace_id(struct thread *task) {
  if (!task) return 0x4E530002;
  spinlock_acquire(&task->namespace_lock);
  uint32_t id = task->ipc_ns ? task->ipc_ns->id : 0x4E530002;
  spinlock_release(&task->namespace_lock);
  return id;
}
