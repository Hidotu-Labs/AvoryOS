#ifndef __LINUX_LOCKDEP_TYPES_H
#define __LINUX_LOCKDEP_TYPES_H

/* LinuxKPI does not build lockdep. Keep the split Linux 6.18 type include
 * compatible with the same lightweight objects used by the spinlock/mutex
 * overlays, so imported headers see one definition. */
#include <linux/types.h>

#define MAX_LOCKDEP_SUBCLASSES 8UL

enum lockdep_wait_type {
  LD_WAIT_INV = 0,
  LD_WAIT_FREE,
  LD_WAIT_SPIN,
#ifdef CONFIG_PROVE_RAW_LOCK_NESTING
  LD_WAIT_CONFIG,
#else
  LD_WAIT_CONFIG = LD_WAIT_SPIN,
#endif
  LD_WAIT_SLEEP,
  LD_WAIT_MAX,
};

enum lockdep_lock_type {
  LD_LOCK_NORMAL = 0,
  LD_LOCK_PERCPU,
  LD_LOCK_WAIT_OVERRIDE,
  LD_LOCK_MAX,
};

struct lockdep_map {
  const char *name;
};

struct lock_class_key {
  int dummy;
};

struct pin_cookie {};

#endif /* __LINUX_LOCKDEP_TYPES_H */
