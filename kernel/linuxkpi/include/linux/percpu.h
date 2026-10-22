#ifndef __AVORY_LINUXKPI_PERCPU_H
#define __AVORY_LINUXKPI_PERCPU_H

/* Static per-CPU variables have initialized templates in .kpi_percpu.
 * Boot clones that section for each native CPU before imported code starts.
 * Pointer accessors require a pinned caller; get_cpu_* supplies that pin.
 * Dynamic alloc_percpu/module allocations are not implemented here. */

unsigned long linuxkpi_percpu_offset(unsigned int cpu);
unsigned int linuxkpi_percpu_cpu(void);
#define __KPI_PERCPU __attribute__((section(".kpi_percpu"), aligned(64)))
#define DECLARE_PER_CPU(type, name) extern type name __KPI_PERCPU
#define DEFINE_PER_CPU(type, name) type name __KPI_PERCPU
#define DECLARE_PER_CPU_ALIGNED(type, name) DECLARE_PER_CPU(type, name)
#define DEFINE_PER_CPU_ALIGNED(type, name) DEFINE_PER_CPU(type, name)
#define DECLARE_PER_CPU_SHARED_ALIGNED(type, name) DECLARE_PER_CPU(type, name)
#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name) DEFINE_PER_CPU(type, name)
#define DECLARE_PER_CPU_READ_MOSTLY(type, name) DECLARE_PER_CPU(type, name)
#define DEFINE_PER_CPU_READ_MOSTLY(type, name) DEFINE_PER_CPU(type, name)
#define DECLARE_PER_CPU_CACHE_HOT(type, name) DECLARE_PER_CPU(type, name)
#define DEFINE_PER_CPU_CACHE_HOT(type, name) DEFINE_PER_CPU(type, name)
#define DECLARE_PER_CPU_FIRST(type, name) DECLARE_PER_CPU(type, name)
#define DEFINE_PER_CPU_FIRST(type, name) DEFINE_PER_CPU(type, name)
#define DECLARE_PER_CPU_PAGE_ALIGNED(type, name) extern type name __attribute__((section(".kpi_percpu"), aligned(4096)))
#define DEFINE_PER_CPU_PAGE_ALIGNED(type, name) type name __attribute__((section(".kpi_percpu"), aligned(4096)))
#define DECLARE_PER_CPU_SECTION(type, name, sec) DECLARE_PER_CPU(type, name)
#define DEFINE_PER_CPU_SECTION(type, name, sec) DEFINE_PER_CPU(type, name)

#define per_cpu_ptr(ptr, cpu) ((typeof(ptr))((char *)(ptr) + linuxkpi_percpu_offset(cpu)))
#define per_cpu(var, cpu) (*per_cpu_ptr(&(var), (cpu)))
#define this_cpu_ptr(ptr) per_cpu_ptr((ptr), linuxkpi_percpu_cpu())
#define raw_cpu_ptr(ptr) this_cpu_ptr(ptr)
#define per_cpu_offset(cpu) linuxkpi_percpu_offset(cpu)
#define __my_cpu_offset linuxkpi_percpu_offset(linuxkpi_percpu_cpu())
#define my_cpu_offset __my_cpu_offset
#define get_cpu_ptr(ptr) ({ preempt_disable(); this_cpu_ptr(ptr); })
#define put_cpu_ptr(ptr) do { (void)(ptr); preempt_enable(); } while (0)
#define get_cpu_var(var) (*get_cpu_ptr(&(var)))
#define put_cpu_var(var) put_cpu_ptr(&(var))

/* Pin the lookup and operation together, including against interrupt code
 * using the same CPU's value. Remote operations retain Linux's caller-lock
 * requirements; these are deliberately not global atomic operations. */
#define this_cpu_read(var) ({ unsigned long __kpi_flags; \
    local_irq_save(__kpi_flags); __auto_type __kpi_value = *this_cpu_ptr(&(var)); \
    local_irq_restore(__kpi_flags); __kpi_value; })
#define __KPI_CPU_UPDATE(var, op, val) do { unsigned long __kpi_flags; \
    local_irq_save(__kpi_flags); *this_cpu_ptr(&(var)) op (val); \
    local_irq_restore(__kpi_flags); } while (0)
#define this_cpu_write(var, val) __KPI_CPU_UPDATE(var, =, val)
#define this_cpu_inc(var) __KPI_CPU_UPDATE(var, +=, 1)
#define this_cpu_dec(var) __KPI_CPU_UPDATE(var, -=, 1)
#define this_cpu_add(var, val) __KPI_CPU_UPDATE(var, +=, val)
#define this_cpu_sub(var, val) __KPI_CPU_UPDATE(var, -=, val)
#define raw_cpu_read(var) (*raw_cpu_ptr(&(var)))
#define raw_cpu_write(var, val) do { *raw_cpu_ptr(&(var)) = (val); } while (0)
#define raw_cpu_add(var, val) do { *raw_cpu_ptr(&(var)) += (val); } while (0)
#define raw_cpu_sub(var, val) do { *raw_cpu_ptr(&(var)) -= (val); } while (0)
#define raw_cpu_inc(var) raw_cpu_add(var, 1)
#define raw_cpu_dec(var) raw_cpu_sub(var, 1)
#define raw_cpu_and(var, val) do { *raw_cpu_ptr(&(var)) &= (val); } while (0)
#define raw_cpu_or(var, val) do { *raw_cpu_ptr(&(var)) |= (val); } while (0)
#define this_cpu_and(var, val) __KPI_CPU_UPDATE(var, &=, val)
#define this_cpu_or(var, val) __KPI_CPU_UPDATE(var, |=, val)
#define __this_cpu_read(var) raw_cpu_read(var)
#define __this_cpu_write(var, val) raw_cpu_write(var, val)
#define __this_cpu_add(var, val) raw_cpu_add(var, val)
#define __this_cpu_sub(var, val) raw_cpu_sub(var, val)
#define __this_cpu_inc(var) raw_cpu_inc(var)
#define __this_cpu_dec(var) raw_cpu_dec(var)
#define __this_cpu_inc_return(var) (++*raw_cpu_ptr(&(var)))
#define __this_cpu_dec_return(var) (--*raw_cpu_ptr(&(var)))
#define __this_cpu_and(var, val) raw_cpu_and(var, val)
#define __this_cpu_or(var, val) raw_cpu_or(var, val)
#define this_cpu_read_stable(var) this_cpu_read(var)
#define this_cpu_read_const(var) this_cpu_read(var)

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>

#ifdef get_cpu
#undef get_cpu
#endif
#ifdef put_cpu
#undef put_cpu
#endif
static inline unsigned int get_cpu(void) { preempt_disable(); return linuxkpi_percpu_cpu(); }
static inline void put_cpu(void) { preempt_enable(); }
#ifndef EXPORT_PER_CPU_SYMBOL
#define EXPORT_PER_CPU_SYMBOL(var)
#define EXPORT_PER_CPU_SYMBOL_GPL(var)
#endif
#endif
