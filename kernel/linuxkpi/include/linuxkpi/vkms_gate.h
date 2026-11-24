#ifndef AVORYOS_VKMS_GATE_H
#define AVORYOS_VKMS_GATE_H

#include <linux/module.h>

bool linuxkpi_vkms_enabled(void);

#undef module_init
#define module_init(fn)                                                        \
  static int __init _avory_gated_##fn(void) {                                  \
    return linuxkpi_vkms_enabled() ? fn() : 0;                                 \
  }                                                                            \
  __initcall(_avory_gated_##fn)

#endif /* AVORYOS_VKMS_GATE_H */
