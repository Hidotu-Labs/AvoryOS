#ifndef __AVORY_LINUXKPI_RCUPDATE_H
#define __AVORY_LINUXKPI_RCUPDATE_H

/* Linux <linux/rcupdate.h> overlay.
 *
 * Read side: per-CPU nesting counter (linuxkpi/src/rcu.c).  Grace periods are
 * detected by polling those counters, so a reader that blocks or is preempted
 * keeps the grace period open; callbacks are invoked by a dedicated kthread.
 * This is correct but not yet optimized (no tree RCU, no expedited IPI path).
 */

#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/types.h>

/* struct rcu_head / rcu_callback_t come from <linux/types.h> upstream
 * (struct callback_head plus `#define rcu_head callback_head`). */

void __kpi_rcu_read_lock(void);
void __kpi_rcu_read_unlock(void);

#define rcu_read_lock() __kpi_rcu_read_lock()
#define rcu_read_unlock() __kpi_rcu_read_unlock()

/* Lockdep/RCU assertion helpers.  AvoryOS has no lockdep and its readers are
 * tracked by a nesting counter, so the "am I in a read-side section?" probes
 * report 1 like upstream does when PROVE_RCU is off.  Implementations in
 * linuxkpi/src/rcu.c. */
int rcu_read_lock_held(void);
int rcu_read_lock_bh_held(void);
int rcu_read_lock_sched_held(void);
int rcu_read_lock_any_held(void);

#define rcu_dereference(p) READ_ONCE(p)
#define rcu_dereference_raw(p) READ_ONCE(p)
#define rcu_dereference_check(p, c) ((void)(c), READ_ONCE(p))
#define rcu_dereference_protected(p, c) ((void)(c), (p))
/* KCSAN handoff annotation; identity here. */
#define rcu_pointer_handoff(p) (p)

#define RCU_INITIALIZER(v) (v)
#define unrcu_pointer(p) ((p))
#define rcu_replace_pointer(rcu_ptr, ptr, c)                                  \
  ({                                                                          \
    typeof(ptr) __oldp = rcu_dereference_protected(rcu_ptr, c);               \
    rcu_assign_pointer(rcu_ptr, ptr);                                         \
    __oldp;                                                                   \
  })

/* RCU-delayed free.  The object's embedded rcu_head is converted back to the
 * object with the field offset, then a small wrapper queues the kfree. */
void __kvfree_call_rcu(struct rcu_head *head, __SIZE_TYPE__ offset);
#define kfree_rcu(ptr, rhf)                                                   \
  do {                                                                        \
    typeof(ptr) ___p = (ptr);                                                 \
    if (___p)                                                                 \
      __kvfree_call_rcu(&(___p)->rhf, __builtin_offsetof(typeof(*___p), rhf));\
  } while (0)
#define rcu_access_pointer(p) READ_ONCE(p)
#define rcu_assign_pointer(p, v)                                              \
  do {                                                                        \
    WRITE_ONCE((p), (v));                                                     \
  } while (0)
#define RCU_INIT_POINTER(p, v)                                                \
  do {                                                                        \
    (p) = (v);                                                                \
  } while (0)

void call_rcu(struct rcu_head *head, rcu_callback_t func);
void rcu_barrier(void);
void synchronize_rcu(void);
void synchronize_rcu_expedited(void);

/* Bring-up: start the callback kthread. */
void linuxkpi_rcu_init(void);

#define list_for_each_entry_rcu(pos, head, member)                            \
  list_for_each_entry(pos, head, member)
#define list_for_each_entry_safe_rcu(pos, n, head, member)                    \
  list_for_each_entry_safe(pos, n, head, member)
#define hlist_for_each_entry_rcu(pos, head, member)                           \
  hlist_for_each_entry(pos, head, member)

/* RCU lockdep assertion (upstream rcupdate.h): no lockdep here, so it is a
 * no-op that still consumes its arguments. */
#define RCU_LOCKDEP_WARN(c, s)                                                 \
  do {                                                                         \
    (void)(c);                                                                 \
    (void)(s);                                                                 \
  } while (0)

#endif /* __AVORY_LINUXKPI_RCUPDATE_H */
