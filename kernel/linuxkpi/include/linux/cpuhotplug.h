#ifndef __AVORY_LINUXKPI_CPUHOTPLUG_H
#define __AVORY_LINUXKPI_CPUHOTPLUG_H

/* Minimal Linux <linux/cpuhotplug.h> overlay.
 *
 * AvoryOS has no CPU hotplug; registrations succeed and their callbacks are
 * never invoked.  Only the pieces imported code references are provided. */

enum cpuhp_state {
  CPUHP_RADIX_DEAD,
};

static inline int cpuhp_setup_state_nocalls(enum cpuhp_state state,
                                            const char *name,
                                            int (*startup)(unsigned int cpu),
                                            int (*teardown)(unsigned int cpu)) {
  (void)state;
  (void)name;
  (void)startup;
  (void)teardown;
  return 0;
}

static inline void cpuhp_remove_state_nocalls(enum cpuhp_state state) {
  (void)state;
}

#endif /* __AVORY_LINUXKPI_CPUHOTPLUG_H */
