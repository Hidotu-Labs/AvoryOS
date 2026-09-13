/* Per-CPU preemption counter and hardirq/softirq context accessors backing
 * the LinuxKPI <linux/preempt.h>.  See that header for the contract.
 *
 * The preemption counter is per-CPU: preempt_disable() prevents migration, so
 * the calling CPU is a stable owner for the duration of the section.  IRQ
 * nesting is per-thread and implemented in kernel/src/linuxkpi/native_sched.c
 * (it touches struct thread), because the scheduler can switch to another
 * thread from inside a handler; the switched-in thread must not inherit the
 * suspended handler's context. */

#include <linux/bug.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>

#include <linuxkpi/native.h>
#include <linuxkpi/native_sched.h>

/* Native MAX_CPUS is 64 (kernel/src/smp/cpu.h); keep in sync via
 * linuxkpi_max_cpus() at runtime for any future growth. */
#define KPI_MAX_CPUS 64

static unsigned int preempt_counts[KPI_MAX_CPUS];

static inline int kpi_cpu(void) {
  int cpu = linuxkpi_cpu_id();
  return (cpu >= 0 && cpu < KPI_MAX_CPUS) ? cpu : 0;
}

unsigned int __kpi_preempt_count(void) {
  return __atomic_load_n(&preempt_counts[kpi_cpu()], __ATOMIC_RELAXED);
}

void __kpi_preempt_add(int val) {
  __atomic_add_fetch(&preempt_counts[kpi_cpu()], (unsigned int)val,
                     __ATOMIC_ACQ_REL);
}

void __kpi_preempt_sub(int val) {
  unsigned int *count = &preempt_counts[kpi_cpu()];
  unsigned int cur = __atomic_load_n(count, __ATOMIC_RELAXED);

  if (cur >= (unsigned int)val)
    __atomic_sub_fetch(count, (unsigned int)val, __ATOMIC_ACQ_REL);
}

void __kpi_preempt_disable(void) { __kpi_preempt_add(1); }

void __kpi_preempt_enable(void) { __kpi_preempt_sub(1); }

void __kpi_preempt_enable_resched(void) {
  __kpi_preempt_sub(1);

  /* Reschedule only when it is safe and useful: the section is fully exited,
   * this is task context (an ISR exit handles its own resched), and IRQs are
   * on so the scheduler's unconditional hal_irq_enable() cannot clobber an
   * outer interrupt-disabled region. */
  if (__kpi_preempt_count() == 0 && !__kpi_in_interrupt() && !irqs_disabled() &&
      linuxkpi_need_resched())
    linuxkpi_yield();
}

bool __kpi_in_interrupt(void) { return linuxkpi_irq_depth() != 0; }

bool __kpi_in_softirq(void) { return linuxkpi_softirq_depth() != 0; }

/* might_sleep() backing (overlay <linux/kernel.h>): warn when called from a
 * non-sleepable context.  preempt_count covers LinuxKPI spinlocks and
 * explicit preempt_disable() sections, in_interrupt() covers hard/soft IRQ
 * handlers, and irqs_disabled() covers native IRQ-masked regions.
 *
 * The warning is once per caller site (not once globally): the ctx self-test
 * deliberately triggers one site every boot, which must not silence a real
 * violation reported from a different file/line later.  A WARN, not a BUG:
 * the caller continues and the soak decides whether it is real. */
#define KPI_MIGHT_SLEEP_SITES 8

static int might_sleep_warnings;
static const char *might_sleep_sites[KPI_MIGHT_SLEEP_SITES];

void __kpi_might_sleep(const char *file, int line) {
  unsigned int count = __kpi_preempt_count();
  int i;

  if (count == 0 && !__kpi_in_interrupt() && !irqs_disabled())
    return;

  __atomic_add_fetch(&might_sleep_warnings, 1, __ATOMIC_RELAXED);

  for (i = 0; i < KPI_MIGHT_SLEEP_SITES; i++) {
    if (might_sleep_sites[i] == file)
      return; /* this site already warned */
    if (might_sleep_sites[i] == NULL) {
      const char *expected = NULL;

      if (__atomic_compare_exchange_n(&might_sleep_sites[i], &expected, file,
                                      false, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED))
        break;
      i--; /* lost the race; retry this slot */
    }
  }
  if (i == KPI_MIGHT_SLEEP_SITES)
    return; /* table full: keep counting, stop printing */

  WARN(1, "might_sleep() from atomic context (%s:%d, preempt=%u irq=%d)\n",
       file, line, count, __kpi_in_interrupt() ? 1 : 0);
}

int kpi_might_sleep_warnings(void) {
  return __atomic_load_n(&might_sleep_warnings, __ATOMIC_RELAXED);
}

/* Called from sched_tick()/the reschedule IPI before an involuntary switch.
 * A KPI thread in an atomic section must not be preempted; blocking and
 * explicit schedule() calls remain the caller's responsibility. */
bool linuxkpi_preempt_allowed(void) { return __kpi_preempt_count() == 0; }
