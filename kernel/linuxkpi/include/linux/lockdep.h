#ifndef __AVORY_LINUXKPI_LOCKDEP_H
#define __AVORY_LINUXKPI_LOCKDEP_H

/* No-op lock dependency tracking.  Many upstream headers include
 * <linux/lockdep.h> and expect the assertion macros to exist; without
 * CONFIG_LOCKDEP they are all no-ops, which is what this overlay provides
 * without dragging in sched.h/thread_info.h. */

#include <linux/types.h>

struct lockdep_map {
  const char *name;
};

struct lock_class_key {
  int dummy;
};

static inline void lockdep_init(void) {}
static inline void lockdep_off(void) {}
static inline void lockdep_on(void) {}
static inline void lockdep_init_map(struct lockdep_map *map, const char *name,
                                    struct lock_class_key *key, int subclass) {
  (void)key;
  (void)subclass;
  if (map)
    map->name = name;
}
#define lockdep_init_map_type(map, name, key, subclass, inner, outer, u, r)   \
  lockdep_init_map(map, name, key, subclass)

#define might_lock(l) ((void)(l))
#define might_lock_read(l) ((void)(l))
#define might_lock_write(l) ((void)(l))

#define lockdep_assert_held(l) ((void)(l))
#define lockdep_assert_held_once(l) ((void)(l))
#define lockdep_assert_not_held(l) ((void)(l))
#define lockdep_assert_irqs_enabled()                                          \
  do {                                                                        \
  } while (0)
#define lockdep_assert_irqs_disabled()                                         \
  do {                                                                        \
  } while (0)
#define lockdep_assert_preemption_enabled()                                    \
  do {                                                                        \
  } while (0)
#define lockdep_assert_preemption_disabled()                                   \
  do {                                                                        \
  } while (0)

#define lockdep_is_held(l) (1)
#define lockdep_is_held_type(l, t)                                             \
  ((void)(t), 1)
#define lockdep_assert_held_write(l) ((void)(l))
#define lockdep_assert_held_read(l) ((void)(l))

#define lockdep_set_class(l, c)                                                \
  do {                                                                        \
    (void)(l);                                                                 \
    (void)(c);                                                                 \
  } while (0)
#define lockdep_set_subclass(l, s)                                             \
  do {                                                                        \
    (void)(l);                                                                 \
    (void)(s);                                                                 \
  } while (0)

/* Lockdep annotation hooks.  These must be argument-less expansions (like
 * upstream's !CONFIG_LOCKDEP definitions): upstream headers call e.g.
 * rwsem_acquire_read(&sem->dep_map, ...) even though dep_map only exists
 * under CONFIG_DEBUG_LOCK_ALLOC, and a macro that referenced its arguments
 * would fail to compile. */
#define lock_acquire(l, s, t, r, n, c, ip) do { } while (0)
#define lock_acquire_shared(l, s, t, n, ip) do { } while (0)
#define lock_acquire_shared_recursive(l, s, t, n, ip) do { } while (0)
#define lock_acquire_exclusive(l, s, t, n, ip) do { } while (0)
#define lock_release(l, ip) do { } while (0)
#define lock_acquired(l, ip) do { } while (0)
#define lock_contended(l, ip) do { } while (0)
#define lock_map_acquire(l) do { } while (0)
#define lock_map_acquire_read(l) do { } while (0)
#define lock_map_acquire_try(l) do { } while (0)
#define lock_map_release(l) do { } while (0)

/* Lockdep wait-context maps: only the shape matters without lockdep. */
struct lock_map {
  int kpi_unused;
};
#define DEFINE_WAIT_OVERRIDE_MAP(name, subclass)                              \
  struct lock_map name = {0}
#define rwsem_acquire(l, s, t, ip) do { } while (0)
#define rwsem_acquire_read(l, s, t, ip) do { } while (0)
#define rwsem_acquire_nest(l, s, t, n, ip) do { } while (0)
#define rwsem_release(l, ip) do { } while (0)
#define lockdep_rcu_suspicious(f, l, s) do { } while (0)
#define lockdep_assert_held_exclusive(l) ((void)(l))
#define lockdep_assert_held_first(l) ((void)(l))
#define lockdep_assert_held_read(l) ((void)(l))
#define lockdep_assert_once(cond) ((void)(cond))
#define lockdep_assert(l) ((void)(l))
#define lockdep_assert_not_held_once(l) ((void)(l))
#define lockdep_assert_none_held_once() do { } while (0)
#define assert_spin_locked(lock) ((void)(lock))
#define assert_raw_spin_locked(lock) ((void)(lock))

#endif /* __AVORY_LINUXKPI_LOCKDEP_H */
