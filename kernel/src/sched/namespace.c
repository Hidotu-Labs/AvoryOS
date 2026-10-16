#include "namespace.h"
#include "capability.h"
#include "sched.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../hal/hal.h"

struct uts_namespace {
  uint32_t refs;
  uint32_t id;
  spinlock_t lock;
  char hostname[65];
  char domain[65];
};

static struct uts_namespace initial_uts = {
    .refs = 1, .id = 0x4E530008, .hostname = "AvoryOS", .domain = "localhost"};
static uint64_t next_namespace_id = 0x4E540000;

uint32_t namespace_allocate_id(void) {
  uint64_t id = __atomic_add_fetch(&next_namespace_id, 1, __ATOMIC_RELAXED);
  return id > 0xFFFFFFFFULL ? 0 : (uint32_t)id;
}

static struct uts_namespace *uts_namespace_get(struct thread *task) {
  if (!task) return &initial_uts;
  hal_irq_state_t irq = hal_irq_save();
  spinlock_acquire(&task->namespace_lock);
  struct uts_namespace *ns = task->uts_ns ? task->uts_ns : &initial_uts;
  if (ns != &initial_uts)
    __atomic_add_fetch(&ns->refs, 1, __ATOMIC_RELAXED);
  spinlock_release(&task->namespace_lock);
  hal_irq_restore(irq);
  return ns;
}

void uts_namespace_put(struct uts_namespace *ns) {
  if (ns && ns != &initial_uts &&
      __atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL) == 0)
    kfree(ns);
}

struct uts_namespace *uts_namespace_copy(struct thread *source) {
  struct uts_namespace *ns = kmalloc(sizeof(*ns));
  if (!ns) return NULL;
  memset(ns, 0, sizeof(*ns));
  spinlock_init(&ns->lock);
  ns->refs = 1;
  ns->id = namespace_allocate_id();
  if (!ns->id) { kfree(ns); return NULL; }
  struct uts_namespace *old = uts_namespace_get(source);
  hal_irq_state_t irq = hal_irq_save();
  spinlock_acquire(&old->lock);
  memcpy(ns->hostname, old->hostname, sizeof(ns->hostname));
  memcpy(ns->domain, old->domain, sizeof(ns->domain));
  spinlock_release(&old->lock);
  hal_irq_restore(irq);
  uts_namespace_put(old);
  return ns;
}

/* Consumes the caller's namespace reference. */
void uts_namespace_install(struct thread *task, struct uts_namespace *ns) {
  hal_irq_state_t irq = hal_irq_save();
  spinlock_acquire(&task->namespace_lock);
  struct uts_namespace *old = task->uts_ns;
  task->uts_ns = ns == &initial_uts ? NULL : ns;
  spinlock_release(&task->namespace_lock);
  hal_irq_restore(irq);
  uts_namespace_put(old);
}

void uts_namespace_inherit(struct thread *child, struct thread *parent) {
  uts_namespace_install(child, uts_namespace_get(parent));
}

void uts_namespace_release(struct thread *task) {
  uts_namespace_install(task, NULL);
}

uint32_t uts_namespace_id(struct thread *task) {
  struct uts_namespace *ns = uts_namespace_get(task);
  uint32_t id = ns->id;
  uts_namespace_put(ns);
  return id;
}

void uts_namespace_read(struct thread *task, char hostname[65], char domain[65]) {
  struct uts_namespace *ns = uts_namespace_get(task);
  hal_irq_state_t irq = hal_irq_save();
  spinlock_acquire(&ns->lock);
  memcpy(hostname, ns->hostname, 65);
  memcpy(domain, ns->domain, 65);
  spinlock_release(&ns->lock);
  hal_irq_restore(irq);
  uts_namespace_put(ns);
}

int uts_namespace_set(struct thread *task, const char *name, uint32_t length,
                       int domain) {
  if (!capability_has(task, CAP_SYS_ADMIN)) return -1;
  if (length > 64) return -22;
  struct uts_namespace *ns = uts_namespace_get(task);
  hal_irq_state_t irq = hal_irq_save();
  spinlock_acquire(&ns->lock);
  char *out = domain ? ns->domain : ns->hostname;
  memset(out, 0, 65);
  memcpy(out, name, length);
  spinlock_release(&ns->lock);
  hal_irq_restore(irq);
  uts_namespace_put(ns);
  return 0;
}
