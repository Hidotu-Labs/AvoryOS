#ifndef __AVORY_LINUXKPI_ASM_PERCPU_H
#define __AVORY_LINUXKPI_ASM_PERCPU_H

/* Native GS points at cpu_info, not Linux's percpu area. Do not import x86
 * GS-relative accessors: every C access must use the native offset bridge. */
#include <linux/percpu.h>
#define init_per_cpu_var(var) var
#define DECLARE_INIT_PER_CPU(var) extern typeof(var) init_per_cpu_var(var)
#define DECLARE_EARLY_PER_CPU(type, name) DECLARE_PER_CPU(type, name)
#define DECLARE_EARLY_PER_CPU_READ_MOSTLY(type, name) DECLARE_PER_CPU_READ_MOSTLY(type, name)
#define DEFINE_EARLY_PER_CPU(type, name, value) DEFINE_PER_CPU(type, name) = (value)
#define DEFINE_EARLY_PER_CPU_READ_MOSTLY(type, name, value) DEFINE_PER_CPU_READ_MOSTLY(type, name) = (value)
#define early_per_cpu(name, cpu) per_cpu(name, cpu)

#endif
