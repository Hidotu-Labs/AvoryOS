#include "pid_namespace.h"
#include "namespace.h"
#include "sched.h"
#include "../mm/heap.h"
#include "../lib/string.h"

struct pid_namespace {
  uint32_t refs, id, depth, next_pid, init_tgid;
  bool dead;
  struct pid_namespace *parent;
};
extern spinlock_t tid_lock;
extern struct thread *global_thread_list;
static void get(struct pid_namespace *ns) {
  if (ns) __atomic_add_fetch(&ns->refs, 1, __ATOMIC_RELAXED);
}
void pid_namespace_put(struct pid_namespace *ns) {
  if (ns && __atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    struct pid_namespace *parent = ns->parent;
    kfree(ns); pid_namespace_put(parent);
  }
}
struct pid_namespace *pid_namespace_create(struct thread *parent) {
  struct pid_namespace *base = parent ? parent->pid_children_ns : NULL;
  if (base && base->depth + 1 >= PID_NS_MAX_DEPTH) return NULL;
  struct pid_namespace *ns = kmalloc(sizeof(*ns));
  if (!ns) return NULL;
  memset(ns, 0, sizeof(*ns));
  ns->refs = 1; ns->id = namespace_allocate_id();
  if (!ns->id) { kfree(ns); return NULL; }
  ns->depth = base ? base->depth + 1 : 1;
  ns->parent = base; get(base); ns->next_pid = 1;
  return ns;
}
void pid_namespace_unshare(struct thread *task, struct pid_namespace *ns) {
  spinlock_acquire(&task->namespace_lock);
  struct pid_namespace *old = task->pid_children_ns;
  task->pid_children_ns = ns;
  spinlock_release(&task->namespace_lock);
  pid_namespace_put(old);
}
int pid_namespace_inherit(struct thread *child, struct thread *parent,
                          struct pid_namespace *new_ns, bool thread) {
  spinlock_acquire(&parent->namespace_lock);
  struct pid_namespace *ns = new_ns ? new_ns : parent->pid_children_ns;
  if (thread && ns != parent->pid_ns) {
    spinlock_release(&parent->namespace_lock); return -22;
  }
  get(ns); get(ns);
  spinlock_release(&parent->namespace_lock);
  spinlock_acquire(&tid_lock);
  for (struct pid_namespace *n = ns; n; n = n->parent) {
    if (n->dead || n->next_pid >= 0x7fffffffU) {
      spinlock_release(&tid_lock); pid_namespace_put(ns); pid_namespace_put(ns);
      return -12;
    }
  }
  child->pid_ns = ns; child->pid_children_ns = ns;
  for (struct pid_namespace *n = ns; n; n = n->parent) {
    child->namespace_pids[n->depth] = n->next_pid++;
    if (!n->init_tgid) n->init_tgid = child->tid;
  }
  spinlock_release(&tid_lock);
  return 0;
}
void pid_namespace_release(struct thread *task) {
  struct pid_namespace *ns = task->pid_ns, *children = task->pid_children_ns;
  task->pid_ns = task->pid_children_ns = NULL;
  pid_namespace_put(ns); pid_namespace_put(children);
}
uint32_t pid_namespace_id(struct thread *task) {
  return task && task->pid_ns ? task->pid_ns->id : 0x4e530005;
}
uint32_t pid_namespace_tid(struct thread *viewer, struct thread *target) {
  if (!target) return 0;
  struct pid_namespace *want = viewer ? viewer->pid_ns : NULL;
  if (!want) return target->is_main_session ? 1 : target->tid;
  for (struct pid_namespace *ns = target->pid_ns; ns; ns = ns->parent)
    if (ns == want) return target->namespace_pids[ns->depth];
  return 0;
}
uint32_t pid_namespace_global_locked(struct thread *viewer, uint32_t local) {
  if (!local) return 0;
  if (!viewer || !viewer->pid_ns) return local;
  for (struct thread *t = global_thread_list; t; t = t->global_next)
    if (pid_namespace_tid(viewer, t) == local) return t->tid;
  return 0;
}
uint32_t pid_namespace_visible_locked(struct thread *viewer, uint32_t global) {
  if (!viewer || !viewer->pid_ns) return global;
  for (struct thread *t = global_thread_list; t; t = t->global_next)
    if (t->tid == global) return pid_namespace_tid(viewer, t);
  return 0;
}
uint32_t pid_namespace_global(struct thread *viewer, uint32_t local) {
  if (!local) return 0;
  if (!viewer || !viewer->pid_ns) return local;
  spinlock_acquire(&tid_lock);
  uint32_t id = pid_namespace_global_locked(viewer, local);
  spinlock_release(&tid_lock); return id;
}
uint32_t pid_namespace_visible(struct thread *viewer, uint32_t global) {
  if (!viewer || !viewer->pid_ns) return global;
  spinlock_acquire(&tid_lock);
  uint32_t id = pid_namespace_visible_locked(viewer, global);
  spinlock_release(&tid_lock); return id;
}
bool pid_namespace_nth(struct thread *viewer, uint32_t index, uint32_t *global) {
  bool found = false;
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (!pid_namespace_tid(viewer, t)) continue;
    if (index--) continue;
    *global = t->tid; found = true; break;
  }
  spinlock_release(&tid_lock); return found;
}
/* Called before reparenting. PID 1 dying permanently closes its namespace.
 * SIGKILL descendants after dropping tid_lock, using scheduler wake pins. */
void pid_namespace_exit(struct thread *task) {
  struct pid_namespace *ns = task->pid_ns;
  if (!ns || task->tgid != ns->init_tgid) return;
  spinlock_acquire(&tid_lock);
  /* Other live threads keep the namespace init process alive. */
  for (struct thread *t = global_thread_list; t; t = t->global_next)
    if (t != task && t->tgid == task->tgid && t->state != THREAD_DEAD &&
        t->state != THREAD_ZOMBIE) { spinlock_release(&tid_lock); return; }
  ns->dead = true;
  spinlock_release(&tid_lock);
  for (;;) {
    struct thread *victim = NULL;
    spinlock_acquire(&tid_lock);
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
      if (t == task || !pid_namespace_tid(task, t) ||
          t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE ||
          (t->pending_signals & (1ULL << (9 - 1)))) continue;
      t->pending_signals |= 1ULL << (9 - 1);
      __atomic_add_fetch(&t->futex_wake_claims, 1, __ATOMIC_ACQ_REL);
      victim = t; break;
    }
    spinlock_release(&tid_lock);
    if (!victim) break;
    sched_wakeup(victim);
    __atomic_sub_fetch(&victim->futex_wake_claims, 1, __ATOMIC_RELEASE);
  }
}

/* Namespace init only receives signals for which it has a handler. Kernel
 * teardown bypasses this check; ancestors may force SIGKILL/SIGSTOP. */
bool pid_namespace_signal_allowed(struct thread *sender, struct thread *target, int sig) {
  if (!sig || !target || !target->pid_ns ||
      target->tgid != target->pid_ns->init_tgid) return true;
  if ((sig == 9 || sig == 19) && sender &&
      sender->pid_ns != target->pid_ns && pid_namespace_tid(sender, target))
    return true;
  if (sig == 9 || sig == 19) return false;
  void *handler = target->signal_handlers[sig - 1].sa_handler;
  return handler && handler != (void *)1;
}
