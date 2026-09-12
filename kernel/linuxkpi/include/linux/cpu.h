#ifndef __AVORY_LINUXKPI_CPU_H
#define __AVORY_LINUXKPI_CPU_H

/* Minimal Linux <linux/cpu.h> overlay.
 *
 * Upstream cpu.h drags in node.h -> device.h -> energy_model/topology, none of
 * which exist here yet; imported code (e.g. lib/radix-tree.c) only needs the
 * CPU-hotplug registration and per-CPU accessors. */

#include <linux/cpuhotplug.h>
#include <linux/percpu.h>

#endif /* __AVORY_LINUXKPI_CPU_H */
