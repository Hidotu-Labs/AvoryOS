#ifndef __AVORY_LINUXKPI_INTERRUPT_H
#define __AVORY_LINUXKPI_INTERRUPT_H

/* AvoryOS overlay for <linux/interrupt.h>.
 *
 * Upstream's header builds the whole generic IRQ layer (irq descriptors,
 * kstat, affinity, debugfs).  AvoryOS keeps its native IRQ controller and
 * bridges only the driver-facing API, so this declares the pieces imported
 * code compiles against.  The handlers themselves are implemented in
 * linuxkpi/src/irq.c; until Phase 5 wires them to the native vector
 * allocator, the declarations are enough because no compiled driver
 * registers an IRQ yet. */

#include <linux/types.h>
#include <linux/irqreturn.h>
#include <linux/irqflags.h>
#include <linux/hardirq.h>
#include <linux/compiler.h>

struct cpumask;

typedef irqreturn_t (*irq_handler_t)(int, void *);
typedef irqreturn_t (*irq_thread_fn_t)(int, void *);

/* Values match upstream include/linux/interrupt.h. */
#define IRQF_SHARED 0x00000080
#define IRQF_PROBE_SHARED 0x00000100
#define IRQF_NOBALANCING 0x00000800
#define IRQF_IRQPOLL 0x00001000
#define IRQF_ONESHOT 0x00002000
#define IRQF_NO_SUSPEND 0x00004000
#define IRQF_FORCE_RESUME 0x00008000
#define IRQF_NO_THREAD 0x00010000
#define IRQF_EARLY_RESUME 0x00020000
#define IRQF_COND_SUSPEND 0x00040000
#define IRQF_NO_AUTOEN 0x00080000
#define IRQF_NO_DEBUG 0x00100000

struct irq_affinity {
  unsigned int pre_vectors;
  unsigned int post_vectors;
  unsigned int nr_sets;
  unsigned int set_size[4];
  void (*calc_sets)(struct irq_affinity *, unsigned int nvecs);
  void *priv;
};

struct irq_affinity_desc;

int request_threaded_irq(unsigned int irq, irq_handler_t handler,
                         irq_handler_t thread_fn, unsigned long flags,
                         const char *name, void *dev);
int request_any_context_irq(unsigned int irq, irq_handler_t handler,
                            unsigned long flags, const char *name, void *dev_id);

static inline int __must_check request_irq(unsigned int irq,
                                           irq_handler_t handler,
                                           unsigned long flags,
                                           const char *name, void *dev) {
  return request_threaded_irq(irq, handler, NULL, flags, name, dev);
}

void free_irq(unsigned int irq, void *dev_id);

void enable_irq(unsigned int irq);
void disable_irq(unsigned int irq);
void disable_irq_nosync(unsigned int irq);
bool disable_hardirq(unsigned int irq);
void synchronize_irq(unsigned int irq);
bool synchronize_hardirq(unsigned int irq);

int irq_set_affinity_hint(unsigned int irq, const struct cpumask *m);
int irq_set_affinity_notifier(unsigned int irq, void *notify);
int irq_set_vcpu_affinity(unsigned int irq, void *vcpu_info);
void irq_update_affinity_hint(unsigned int irq, const struct cpumask *m);

#endif /* __AVORY_LINUXKPI_INTERRUPT_H */
