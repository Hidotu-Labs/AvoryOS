#ifndef __AVORY_LINUXKPI_PERCPU_H
#define __AVORY_LINUXKPI_PERCPU_H

/* Minimal Linux <linux/percpu.h> overlay.
 *
 * IMPORTANT: there is no per-CPU allocator yet.  Variables declared with
 * DECLARE_PER_CPU/DEFINE_PER_CPU become ordinary single-instance globals and
 * this_cpu_* accessors alias the plain variable.  That is wrong for data
 * shared across CPUs (it is effectively a uniprocessor emulation), and only
 * acceptable for the currently imported code paths (e.g. the unused radix
 * tree preload cache).  A real per-CPU implementation is required before
 * drivers use per-CPU data; tracked in docs/linuxkpi-progress.md.
 */

#include <linux/compiler.h>
#include <linux/types.h>

#define DECLARE_PER_CPU(type, name) extern type name
#define DEFINE_PER_CPU(type, name) type name
#define DECLARE_PER_CPU_ALIGNED(type, name) extern type name
#define DEFINE_PER_CPU_ALIGNED(type, name) type name
#define DECLARE_PER_CPU_SHARED_ALIGNED(type, name) extern type name
#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name) type name

#define per_cpu_ptr(ptr, cpu) ((void)(cpu), (typeof(*(ptr)) *)(ptr))
#define per_cpu(var, cpu) (*(typeof(var) *)((void)(cpu), &(var)))
#define this_cpu_ptr(ptr) ((typeof(*(ptr)) *)(ptr))
#define raw_cpu_ptr(ptr) ((typeof(*(ptr)) *)(ptr))
#define get_cpu_ptr(ptr) ((typeof(*(ptr)) *)(ptr))
#define put_cpu_ptr(ptr)                                                      \
  do {                                                                        \
    (void)(ptr);                                                              \
  } while (0)
#define get_cpu_var(var) (var)
#define put_cpu_var(var)                                                      \
  do {                                                                        \
    (void)(var);                                                              \
  } while (0)

#define this_cpu_read(var) (var)
#define this_cpu_write(var, val)                                              \
  do {                                                                        \
    (var) = (val);                                                            \
  } while (0)
#define this_cpu_inc(var)                                                     \
  do {                                                                        \
    (var)++;                                                                  \
  } while (0)
#define this_cpu_dec(var)                                                     \
  do {                                                                        \
    (var)--;                                                                  \
  } while (0)
#define this_cpu_add(var, val)                                                \
  do {                                                                        \
    (var) += (val);                                                           \
  } while (0)
#define this_cpu_sub(var, val)                                                \
  do {                                                                        \
    (var) -= (val);                                                           \
  } while (0)

#define raw_cpu_read(var) this_cpu_read(var)
#define raw_cpu_write(var, val) this_cpu_write(var, val)
#define __this_cpu_read(var) this_cpu_read(var)
#define __this_cpu_write(var, val) this_cpu_write(var, val)

/* <linux/smp.h> defines get_cpu()/put_cpu() as macros; if it was included
 * first, make sure they do not expand these declarations. */
#ifdef get_cpu
#undef get_cpu
#endif
#ifdef put_cpu
#undef put_cpu
#endif

static inline unsigned int get_cpu(void) { return 0; }
static inline void put_cpu(void) {}

/* No modules/ksymtab here: exports are no-ops.  Upstream defines these in
 * <linux/percpu-defs.h>, which this overlay replaces. */
#ifndef EXPORT_PER_CPU_SYMBOL
#define EXPORT_PER_CPU_SYMBOL(var)
#define EXPORT_PER_CPU_SYMBOL_GPL(var)
#endif

#endif /* __AVORY_LINUXKPI_PERCPU_H */
