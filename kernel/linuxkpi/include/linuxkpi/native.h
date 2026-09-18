#ifndef LINUXKPI_NATIVE_H
#define LINUXKPI_NATIVE_H

/* Bridge declarations for native AvoryOS primitives that LinuxKPI
 * implementation files need.  Native helpers are implemented in
 * kernel/src/linuxkpi/native.c (compiled with native headers, where the
 * freestanding <stdint.h> is available); the declarations here deliberately
 * avoid libc-style typedefs so they can be included alongside Linux headers.
 *
 * Functions whose native symbol name matches the declaration use
 * __asm__("...") aliases so the implementation can use a distinct C name and
 * avoid clashing with Linux API names (for example, native kmalloc() and
 * Linux's kmalloc() macro).
 */

/* Logical CPU index of the calling CPU (0 for the BSP, `ncpus` max). */
int linuxkpi_cpu_id(void);

/* Maximum number of logical CPUs the native kernel supports. */
int linuxkpi_max_cpus(void);

/* x86 PAUSE hint, the native equivalent of cpu_relax(). */
void linuxkpi_cpu_relax(void);

/* TSC-based timing for KPI-side benchmarks.  The native <stdint.h> and the
 * Linux headers typedef uint64_t differently, so the bridge carries plain
 * unsigned long long values. */
unsigned long long linuxkpi_rdtsc_fence(void);
unsigned long long linuxkpi_cycles_to_ns(unsigned long long cycles);

/* Kernel command line from the bootloader (empty string when none). */
const char *linuxkpi_boot_cmdline(void);

#endif /* LINUXKPI_NATIVE_H */
