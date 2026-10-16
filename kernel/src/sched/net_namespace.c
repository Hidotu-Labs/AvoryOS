#include "net_namespace.h"
#include "namespace.h"
#include "sched.h"
#include "../mm/heap.h"
struct net_namespace *net_namespace_create(void) {
  struct net_namespace *n = kmalloc(sizeof(*n));
  if (!n) return 0;
  n->refs = 1; n->id = namespace_allocate_id();
  n->loopback_address = 0; n->loopback_flags = 8; /* IFF_LOOPBACK, down */
  if (!n->id) { kfree(n); return 0; }
  return n;
}
struct net_namespace *net_namespace_get(struct net_namespace *n) {
  if (n) __atomic_add_fetch(&n->refs, 1, __ATOMIC_RELAXED);
  return n;
}
void net_namespace_put(struct net_namespace *n) {
  if (n && !__atomic_sub_fetch(&n->refs, 1, __ATOMIC_ACQ_REL)) kfree(n);
}
void net_namespace_install(struct thread *t, struct net_namespace *n) {
  spinlock_acquire(&t->namespace_lock);
  struct net_namespace *old = t->net_ns;
  t->net_ns = n;
  spinlock_release(&t->namespace_lock);
  net_namespace_put(old);
}
void net_namespace_inherit(struct thread *child, struct thread *parent) {
  spinlock_acquire(&parent->namespace_lock);
  child->net_ns = net_namespace_get(parent->net_ns);
  spinlock_release(&parent->namespace_lock);
}
void net_namespace_release(struct thread *t) { net_namespace_install(t, 0); }
uint32_t net_namespace_id(struct thread *t) {
  if (!t) return 0x4e530004;
  spinlock_acquire(&t->namespace_lock);
  uint32_t id = t->net_ns ? t->net_ns->id : 0x4e530004;
  spinlock_release(&t->namespace_lock);
  return id;
}
