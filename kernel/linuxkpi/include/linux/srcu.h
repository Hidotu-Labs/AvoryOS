#ifndef __AVORY_LINUXKPI_SRCU_H
#define __AVORY_LINUXKPI_SRCU_H

/* AvoryOS overlay for <linux/srcu.h>.
 *
 * CONFIG_TRACEPOINTS is off and no imported driver uses SRCU; upstream's
 * header is included unconditionally by tracepoint.h, so this provides the
 * type plus API stubs.  A real SRCU (or an import of upstream tiny SRCU)
 * is required before enabling tracepoints or anything that sleeps in a
 * srcu read-side section. */

#include <linux/types.h>

struct srcu_struct {
  int srcu_lock_nesting;
};

/* struct srcu_notifier_head lives in the notifier.h overlay (as upstream). */

/* Static definition helper.  The stub srcu_struct has no state that needs
 * initializing, so this is just a definition. */
#define DEFINE_STATIC_SRCU(name) struct srcu_struct name
#define __DEFINE_SRCU(name, is_static)

#define init_srcu_struct(ss) (0)
#define cleanup_srcu_struct(ss) do { (void)(ss); } while (0)

static inline int __srcu_read_lock(struct srcu_struct *ss) {
  (void)ss;
  return 0;
}

static inline void __srcu_read_unlock(struct srcu_struct *ss, int idx) {
  (void)ss;
  (void)idx;
}

#define srcu_read_lock(ss) __srcu_read_lock(ss)
#define srcu_read_unlock(ss, idx) __srcu_read_unlock(ss, idx)
#define srcu_read_lock_notrace(ss) __srcu_read_lock(ss)
#define srcu_read_unlock_notrace(ss, idx) __srcu_read_unlock(ss, idx)

static inline void synchronize_srcu(struct srcu_struct *ss) { (void)ss; }
static inline void synchronize_srcu_expedited(struct srcu_struct *ss) {
  (void)ss;
}
static inline void srcu_barrier(struct srcu_struct *ss) { (void)ss; }

#endif /* __AVORY_LINUXKPI_SRCU_H */
