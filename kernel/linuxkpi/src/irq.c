/* Linux IRQ core for LinuxKPI (Phase 5 C2).
 *
 * The native controller owns vectors and the IDT; this file owns the Linux
 * driver-facing API: a 256-entry descriptor table indexed by the vector (MSI
 * vectors use the native vector directly, legacy INTx uses 32 + line), action
 * chains for IRQF_SHARED, threaded handlers via the system workqueue, and
 * enable/disable/synchronize with per-vector mask callbacks registered by the
 * PCI layer.
 *
 * Deliberate simplifications (recorded in docs/linuxkpi-gaps.md):
 *   - the descriptor lock is held while hard handlers run, because action
 *     nodes are freed on free_irq(); handlers must not call request_irq()/
 *     free_irq()/disable_irq*()/synchronize_*irq() on their own IRQ,
 *   - ONESHOT masking uses the native per-vector mask callback;
 *   - no irq_chip, no irq domains, no per-IRQ kthreads (the workqueue pool is
 *     the threaded context). */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_irq.h>

#define KPI_IRQ_MAX 256

struct kpi_irq_action {
  unsigned int irq;
  irq_handler_t handler;
  irq_handler_t thread_fn;
  void *dev_id;
  unsigned long flags;
  const char *name;
  struct work_struct work;
  int in_flight;
  struct kpi_irq_action *next;
};

struct kpi_irq_desc {
  spinlock_t lock;
  struct kpi_irq_action *actions;
  unsigned int disable_count;
  unsigned int threads_active; /* queued + executing, including removed actions */
  unsigned int oneshot_active;
  bool masked;
  void (*mask_fn)(void *data, int masked);
  void *mask_data;
};

static struct kpi_irq_desc kpi_irq_descs[KPI_IRQ_MAX];
unsigned long linuxkpi_irq_live_actions(void) {
  unsigned long count = 0, flags;
  for (unsigned int irq = 0; irq < KPI_IRQ_MAX; irq++) {
    struct kpi_irq_action *action;
    spin_lock_irqsave(&kpi_irq_descs[irq].lock, flags);
    for (action = kpi_irq_descs[irq].actions; action; action = action->next)
      count++;
    count += kpi_irq_descs[irq].threads_active;
    spin_unlock_irqrestore(&kpi_irq_descs[irq].lock, flags);
  }
  return count;
}
/* Zero-initialized spinlocks are valid. Never reinitialize a live descriptor
 * during concurrent first registrations. All descriptor state uses this lock. */
static void kpi_irq_update_mask(struct kpi_irq_desc *d) {
  bool masked = !d->actions || d->disable_count || d->oneshot_active;
  if (d->mask_fn && masked != d->masked) {
    d->mask_fn(d->mask_data, masked);
    d->masked = masked;
  }
}

/* ── mask registry (used by enable_irq/disable_irq) ─────────────────────── */

void linuxkpi_irq_register_mask(unsigned int irq,
                                void (*fn)(void *data, int masked),
                                void *data) {
  if (irq >= KPI_IRQ_MAX)
    return;
  unsigned long flags;
  struct kpi_irq_desc *d = &kpi_irq_descs[irq];
  spin_lock_irqsave(&d->lock, flags);
  d->mask_fn = fn;
  d->mask_data = data;
  d->masked = !d->actions || d->disable_count || d->oneshot_active;
  if (fn)
    fn(data, d->masked);
  spin_unlock_irqrestore(&d->lock, flags);
}

void linuxkpi_irq_unregister_mask(unsigned int irq) {
  if (irq >= KPI_IRQ_MAX)
    return;
  unsigned long flags;
  struct kpi_irq_desc *d = &kpi_irq_descs[irq];
  spin_lock_irqsave(&d->lock, flags);
  d->mask_fn = NULL;
  d->mask_data = NULL;
  spin_unlock_irqrestore(&d->lock, flags);
}

/* ── threaded handler work ──────────────────────────────────────────────── */

static void kpi_irq_thread_work(struct work_struct *work) {
  struct kpi_irq_action *a =
      container_of(work, struct kpi_irq_action, work);

  struct kpi_irq_desc *d = &kpi_irq_descs[a->irq];
  unsigned long flags;

  a->thread_fn((int)a->irq, a->dev_id);
  spin_lock_irqsave(&d->lock, flags);
  d->threads_active--;
  if (a->flags & IRQF_ONESHOT)
    d->oneshot_active--;
  kpi_irq_update_mask(d);
  spin_unlock_irqrestore(&d->lock, flags);
}

/* ── dispatch (called from the native ISR trampoline) ───────────────────── */

void linuxkpi_irq_handle_vector(unsigned int irq) {
  struct kpi_irq_desc *d;
  struct kpi_irq_action *a;
  unsigned long flags;

  if (irq >= KPI_IRQ_MAX)
    return;
  d = &kpi_irq_descs[irq];

  spin_lock_irqsave(&d->lock, flags);
  if (d->disable_count || d->oneshot_active || !d->actions) {
    spin_unlock_irqrestore(&d->lock, flags);
    return;
  }
  for (a = d->actions; a; a = a->next) {
    irqreturn_t ret = IRQ_NONE;

    __atomic_add_fetch(&a->in_flight, 1, __ATOMIC_ACQ_REL);
    if (a->handler)
      ret = a->handler((int)irq, a->dev_id);
    else
      ret = IRQ_WAKE_THREAD;
    if (ret == IRQ_WAKE_THREAD && a->thread_fn && schedule_work(&a->work)) {
      d->threads_active++;
      if (a->flags & IRQF_ONESHOT)
        d->oneshot_active++;
    }
    __atomic_sub_fetch(&a->in_flight, 1, __ATOMIC_ACQ_REL);
  }
  kpi_irq_update_mask(d);
  spin_unlock_irqrestore(&d->lock, flags);
}

/* ── request/free ───────────────────────────────────────────────────────── */

int request_threaded_irq(unsigned int irq, irq_handler_t handler,
                         irq_handler_t thread_fn, unsigned long flags,
                         const char *name, void *dev) {
  struct kpi_irq_desc *d;
  struct kpi_irq_action *a;
  unsigned long fl;

  if (irq >= KPI_IRQ_MAX || (!handler && !thread_fn))
    return -EINVAL;
  if ((flags & IRQF_SHARED) && !dev)
    return -EINVAL;

  a = kzalloc(sizeof(*a), GFP_KERNEL);
  if (!a)
    return -ENOMEM;
  a->irq = irq;
  a->handler = handler;
  a->thread_fn = thread_fn;
  a->dev_id = dev;
  a->flags = flags;
  a->name = name;
  if (thread_fn)
    INIT_WORK(&a->work, kpi_irq_thread_work);

  d = &kpi_irq_descs[irq];
  spin_lock_irqsave(&d->lock, fl);
  if ((flags & IRQF_ONESHOT) && !d->mask_fn) {
    spin_unlock_irqrestore(&d->lock, fl);
    kfree(a);
    return -ENOTSUPP;
  }
  if (d->actions && (!(flags & IRQF_SHARED) ||
                     !(d->actions->flags & IRQF_SHARED) ||
                     ((d->actions->flags ^ flags) & IRQF_ONESHOT))) {
    spin_unlock_irqrestore(&d->lock, fl);
    kfree(a);
    return -EBUSY;
  }
  if (!d->actions)
    d->disable_count = 0;
  a->next = d->actions;
  d->actions = a;
  kpi_irq_update_mask(d);
  spin_unlock_irqrestore(&d->lock, fl);
  return 0;
}

int request_any_context_irq(unsigned int irq, irq_handler_t handler,
                            unsigned long flags, const char *name,
                            void *dev_id) {
  return request_threaded_irq(irq, handler, NULL, flags, name, dev_id);
}

void free_irq(unsigned int irq, void *dev_id) {
  struct kpi_irq_desc *d;
  struct kpi_irq_action *a = NULL, **pp;
  unsigned long fl;

  if (irq >= KPI_IRQ_MAX)
    return;
  d = &kpi_irq_descs[irq];

  spin_lock_irqsave(&d->lock, fl);
  for (pp = &d->actions; *pp; pp = &(*pp)->next) {
    if ((*pp)->dev_id == dev_id) {
      a = *pp;
      *pp = a->next;
      kpi_irq_update_mask(d);
      break;
    }
  }
  spin_unlock_irqrestore(&d->lock, fl);
  if (!a)
    return;

  /* The dispatcher cannot run this action anymore (it is unlinked), but one
   * invocation may still be in flight on another CPU. */
  while (__atomic_load_n(&a->in_flight, __ATOMIC_ACQUIRE) != 0)
    msleep(1);
  if (a->thread_fn)
    flush_work(&a->work);
  kfree(a);
}

/* ── enable/disable/synchronize ─────────────────────────────────────────── */

void disable_irq_nosync(unsigned int irq) {
  struct kpi_irq_desc *d;
  unsigned long fl;

  if (irq >= KPI_IRQ_MAX)
    return;
  d = &kpi_irq_descs[irq];
  spin_lock_irqsave(&d->lock, fl);
  d->disable_count++;
  kpi_irq_update_mask(d);
  spin_unlock_irqrestore(&d->lock, fl);
}

void synchronize_irq(unsigned int irq) {
  struct kpi_irq_desc *d;
  struct kpi_irq_action *a;
  unsigned long fl;
  bool busy;

  if (irq >= KPI_IRQ_MAX)
    return;
  d = &kpi_irq_descs[irq];
  do {
    busy = false;
    spin_lock_irqsave(&d->lock, fl);
    busy = d->threads_active != 0;
    for (a = d->actions; a; a = a->next) {
      if (__atomic_load_n(&a->in_flight, __ATOMIC_ACQUIRE)) {
        busy = true;
        break;
      }
    }
    spin_unlock_irqrestore(&d->lock, fl);
    if (busy)
      msleep(1);
  } while (busy);
}

void disable_irq(unsigned int irq) {
  disable_irq_nosync(irq);
  synchronize_irq(irq);
}

void enable_irq(unsigned int irq) {
  struct kpi_irq_desc *d;
  unsigned long fl;

  if (irq >= KPI_IRQ_MAX)
    return;
  d = &kpi_irq_descs[irq];
  spin_lock_irqsave(&d->lock, fl);
  if (d->disable_count) {
    d->disable_count--;
    kpi_irq_update_mask(d);
  }
  spin_unlock_irqrestore(&d->lock, fl);
}

bool disable_hardirq(unsigned int irq) {
  disable_irq_nosync(irq);
  return synchronize_hardirq(irq);
}

bool synchronize_hardirq(unsigned int irq) {
  unsigned long flags;
  bool idle;
  if (irq >= KPI_IRQ_MAX)
    return true;
  struct kpi_irq_desc *d = &kpi_irq_descs[irq];
  /* Dispatch holds the lock across hard handlers: acquisition drains them,
   * but must not sleep waiting for threaded handlers. */
  spin_lock_irqsave(&d->lock, flags);
  idle = d->threads_active == 0;
  spin_unlock_irqrestore(&d->lock, flags);
  return idle;
}

/* ── affinity stubs ─────────────────────────────────────────────────────── */

int irq_set_affinity_hint(unsigned int irq, const struct cpumask *m) {
  (void)irq;
  (void)m;
  return 0;
}

int irq_set_affinity_notifier(unsigned int irq, void *notify) {
  (void)irq;
  (void)notify;
  return 0;
}

int irq_set_vcpu_affinity(unsigned int irq, void *vcpu_info) {
  (void)irq;
  (void)vcpu_info;
  return -EINVAL;
}

void irq_update_affinity_hint(unsigned int irq, const struct cpumask *m) {
  (void)irq;
  (void)m;
}

/* ── IRQ domains (amdgpu_irq.c) ───────────────────────────────────────────
 *
 * AvoryOS has no irq_domain core; amdgpu creates one for its ACP interrupt
 * sources (disabled here) and maps hwirq -> virq.  Stock <linux/irqdomain.h>
 * provides irq_domain_add_linear()/irq_create_mapping() as inlines over the
 * externs implemented here.  No real domain is created: mapping falls back to
 * the hwirq number.  Real support is only needed if the ACP/domain path is
 * ever enabled (docs/linuxkpi-gaps.md P6 C2). */
struct irq_domain *__irq_domain_add(struct fwnode_handle *fwnode,
                                    unsigned int size,
                                    irq_hw_number_t hwirq_max, int direct_max,
                                    const struct irq_domain_ops *ops,
                                    void *host_data) {
  (void)fwnode;
  (void)size;
  (void)hwirq_max;
  (void)direct_max;
  (void)ops;
  (void)host_data;
  return NULL;
}

void irq_domain_remove(struct irq_domain *domain) { (void)domain; }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
/* Same unsupported-domain result as the old __irq_domain_add API. */
struct irq_domain *irq_domain_instantiate(const struct irq_domain_info *info) {
  (void)info;
  return NULL;
}
#endif

unsigned int irq_create_mapping_affinity(struct irq_domain *domain,
                                         irq_hw_number_t hwirq,
                                         const struct irq_affinity_desc *affinity) {
  (void)domain;
  (void)affinity;
  return (unsigned int)hwirq;
}

int generic_handle_domain_irq(struct irq_domain *domain, unsigned int hwirq) {
  (void)domain;
  (void)hwirq;
  return -EINVAL;
}

/* IRQ-chip registration is inert (the native IDT/vector layer owns dispatch);
 * amdgpu_irq.c calls these for its optional ACP domain path.  Recorded in
 * docs/linuxkpi-gaps.md (P6 C2). */
void irq_set_chip_and_handler_name(unsigned int irq, struct irq_chip *chip,
                                   irq_flow_handler_t handle,
                                   const char *name) {
  (void)irq;
  (void)chip;
  (void)handle;
  (void)name;
}

void handle_simple_irq(struct irq_desc *desc) { (void)desc; }
