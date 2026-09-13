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
 *     free_irq()/disable_irq() on their own IRQ,
 *   - IRQF_ONESHOT does not mask in hardware; the dispatcher drops interrupts
 *     while disabled, so the device line may stay asserted until ack,
 *   - no irq_chip, no irq domains, no per-IRQ kthreads (the workqueue pool is
 *     the threaded context). */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
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
  void (*mask_fn)(void *data, int masked);
  void *mask_data;
};

static struct kpi_irq_desc kpi_irq_descs[KPI_IRQ_MAX];
static bool kpi_irq_ready;

static void kpi_irq_core_init(void) {
  if (kpi_irq_ready)
    return;
  for (int i = 0; i < KPI_IRQ_MAX; i++)
    spin_lock_init(&kpi_irq_descs[i].lock);
  kpi_irq_ready = true;
}

/* ── mask registry (used by enable_irq/disable_irq) ─────────────────────── */

void linuxkpi_irq_register_mask(unsigned int irq,
                                void (*fn)(void *data, int masked),
                                void *data) {
  if (irq >= KPI_IRQ_MAX)
    return;
  kpi_irq_descs[irq].mask_fn = fn;
  kpi_irq_descs[irq].mask_data = data;
}

void linuxkpi_irq_unregister_mask(unsigned int irq) {
  if (irq >= KPI_IRQ_MAX)
    return;
  kpi_irq_descs[irq].mask_fn = NULL;
  kpi_irq_descs[irq].mask_data = NULL;
}

/* ── threaded handler work ──────────────────────────────────────────────── */

static void kpi_irq_thread_work(struct work_struct *work) {
  struct kpi_irq_action *a =
      container_of(work, struct kpi_irq_action, work);

  if (a->thread_fn)
    a->thread_fn((int)a->irq, a->dev_id);
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
  if (d->disable_count || !d->actions) {
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
    if (ret == IRQ_WAKE_THREAD && a->thread_fn)
      schedule_work(&a->work);
    __atomic_sub_fetch(&a->in_flight, 1, __ATOMIC_ACQ_REL);
  }
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
  kpi_irq_core_init();

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
  if (d->actions && !(flags & IRQF_SHARED)) {
    spin_unlock_irqrestore(&d->lock, fl);
    kfree(a);
    return -EBUSY;
  }
  a->next = d->actions;
  d->actions = a;
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
    cancel_work_sync(&a->work);
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
  if (d->disable_count == 0 && d->mask_fn)
    d->mask_fn(d->mask_data, 1);
  d->disable_count++;
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
    if (d->disable_count == 0 && d->mask_fn)
      d->mask_fn(d->mask_data, 0);
  }
  spin_unlock_irqrestore(&d->lock, fl);
}

bool disable_hardirq(unsigned int irq) {
  disable_irq_nosync(irq);
  return true;
}

bool synchronize_hardirq(unsigned int irq) {
  synchronize_irq(irq);
  return true;
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
