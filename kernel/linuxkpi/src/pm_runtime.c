/* LinuxKPI runtime power management core (CONFIG_PM).
 *
 * Implements the per-device runtime PM state machine, autosuspend timer,
 * bus/driver callback dispatch, and devm wrappers declared by stock
 * <linux/pm_runtime.h>.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeirq.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

struct workqueue_struct *pm_wq;
static DEFINE_SPINLOCK(kpi_pm_wq_lock);

static struct workqueue_struct *kpi_pm_wq(void) {
  struct workqueue_struct *wq = READ_ONCE(pm_wq);
  unsigned long flags;

  if (wq)
    return wq;

  spin_lock_irqsave(&kpi_pm_wq_lock, flags);
  wq = pm_wq;
  if (!wq) {
    wq = alloc_workqueue("pm", WQ_FREEZABLE, 0);
    if (!wq)
      wq = system_wq;
    WRITE_ONCE(pm_wq, wq);
  }
  spin_unlock_irqrestore(&kpi_pm_wq_lock, flags);
  return wq;
}

static const struct dev_pm_ops *kpi_pm_ops(const struct device *dev) {
  if (!dev)
    return NULL;
  if (dev->pm_domain)
    return &dev->pm_domain->ops;
  if (dev->type && dev->type->pm)
    return dev->type->pm;
  if (dev->bus && dev->bus->pm)
    return dev->bus->pm;
  if (dev->driver && dev->driver->pm)
    return dev->driver->pm;
  return NULL;
}

static void kpi_pm_update_status(struct device *dev, enum rpm_status status) {
  u64 now = ktime_get_mono_fast_ns();

  if (dev->power.runtime_status == RPM_SUSPENDED &&
      now > dev->power.accounting_timestamp) {
    dev->power.suspended_ns += now - dev->power.accounting_timestamp;
  }
  dev->power.accounting_timestamp = now;
  dev->power.runtime_status = status;
}

static void kpi_pm_async_work(struct work_struct *work) {
  struct device *dev = container_of(work, struct device, power.work);
  unsigned long flags;
  enum rpm_request req;

  spin_lock_irqsave(&dev->power.lock, flags);
  req = dev->power.request;
  dev->power.request = RPM_REQ_NONE;
  spin_unlock_irqrestore(&dev->power.lock, flags);

  switch (req) {
  case RPM_REQ_IDLE:
    __pm_runtime_idle(dev, 0);
    break;
  case RPM_REQ_SUSPEND:
    __pm_runtime_suspend(dev, 0);
    break;
  case RPM_REQ_AUTOSUSPEND:
    __pm_runtime_suspend(dev, RPM_AUTO);
    break;
  case RPM_REQ_RESUME:
    __pm_runtime_resume(dev, 0);
    break;
  default:
    break;
  }
}

static void kpi_pm_autosuspend_work(struct work_struct *work) {
  struct delayed_work *dwork = container_of(work, struct delayed_work, work);
  struct device *dev = container_of(dwork, struct device, power.autosuspend_work);

  __pm_runtime_suspend(dev, RPM_AUTO);
}

void pm_runtime_init(struct device *dev) {
  if (!dev)
    return;

  spin_lock_init(&dev->power.lock);
  init_waitqueue_head(&dev->power.wait_queue);
  INIT_WORK(&dev->power.work, kpi_pm_async_work);
  INIT_DELAYED_WORK(&dev->power.autosuspend_work, kpi_pm_autosuspend_work);
  atomic_set(&dev->power.usage_count, 0);
  dev->power.disable_depth = 1;
  dev->power.runtime_status = RPM_SUSPENDED;
  dev->power.last_status = RPM_SUSPENDED;
  dev->power.request = RPM_REQ_NONE;
  dev->power.runtime_error = 0;
  dev->power.autosuspend_delay = -1;
  dev->power.last_busy = ktime_get_mono_fast_ns();
  dev->power.accounting_timestamp = dev->power.last_busy;
  dev->power.suspended_ns = 0;
  dev->power.ignore_children = false;
  dev->power.no_callbacks = false;
  dev->power.irq_safe = false;
  dev->power.runtime_auto = true;
  dev->power.use_autosuspend = false;
  dev->power.pm_initialized = true;
}

static inline void kpi_pm_ensure_init(struct device *dev) {
  if (dev && !dev->power.pm_initialized)
    pm_runtime_init(dev);
}

static int kpi_pm_check_suspend_allowed(struct device *dev) {
  if (dev->power.runtime_error)
    return -EINVAL;
  if (dev->power.disable_depth > 0)
    return -EACCES;
  if (atomic_read(&dev->power.usage_count) > 0)
    return -EAGAIN;
  return 0;
}

static void kpi_pm_wait_transition_locked(struct device *dev,
                                          unsigned long *flags) {
  while (dev->power.runtime_status == RPM_SUSPENDING ||
         dev->power.runtime_status == RPM_RESUMING) {
    spin_unlock_irqrestore(&dev->power.lock, *flags);
    wait_event(dev->power.wait_queue,
               dev->power.runtime_status != RPM_SUSPENDING &&
                   dev->power.runtime_status != RPM_RESUMING);
    spin_lock_irqsave(&dev->power.lock, *flags);
  }
}

int pm_generic_runtime_suspend(struct device *dev) {
  const struct dev_pm_ops *pm = dev && dev->driver ? dev->driver->pm : NULL;

  if (pm && pm->runtime_suspend)
    return pm->runtime_suspend(dev);
  return 0;
}

int pm_generic_runtime_resume(struct device *dev) {
  const struct dev_pm_ops *pm = dev && dev->driver ? dev->driver->pm : NULL;

  if (pm && pm->runtime_resume)
    return pm->runtime_resume(dev);
  return 0;
}

int pm_runtime_force_suspend(struct device *dev) {
  const struct dev_pm_ops *pm;
  int ret;

  if (!dev)
    return -EINVAL;
  kpi_pm_ensure_init(dev);
  pm = kpi_pm_ops(dev);
  if (!pm || !pm->runtime_suspend)
    return 0;

  ret = pm->runtime_suspend(dev);
  if (!ret) {
    unsigned long flags;

    spin_lock_irqsave(&dev->power.lock, flags);
    kpi_pm_update_status(dev, RPM_SUSPENDED);
    spin_unlock_irqrestore(&dev->power.lock, flags);
  }
  return ret;
}

int __pm_runtime_resume(struct device *dev, int rpmflags) {
  unsigned long flags;
  const struct dev_pm_ops *pm;
  int retval = 0;

  if (!dev)
    return -EINVAL;
  kpi_pm_ensure_init(dev);

  if (rpmflags & RPM_GET_PUT)
    atomic_inc(&dev->power.usage_count);

  spin_lock_irqsave(&dev->power.lock, flags);

repeat:
  if (dev->power.runtime_error) {
    retval = -EINVAL;
    goto out;
  }
  if (dev->power.disable_depth > 0) {
    retval = (rpmflags & RPM_TRANSPARENT) ? 0 : -EACCES;
    goto out;
  }

  if (dev->power.runtime_status == RPM_ACTIVE) {
    retval = 1;
    goto out;
  }

  if (rpmflags & RPM_ASYNC) {
    struct workqueue_struct *wq = kpi_pm_wq();

    if (wq) {
      dev->power.request = RPM_REQ_RESUME;
      queue_work(wq, &dev->power.work);
      retval = 0;
      goto out;
    }
  }

  if (dev->power.runtime_status == RPM_RESUMING ||
      dev->power.runtime_status == RPM_SUSPENDING) {
    if (rpmflags & RPM_NOWAIT) {
      retval = -EAGAIN;
      goto out;
    }
    kpi_pm_wait_transition_locked(dev, &flags);
    goto repeat;
  }

  /* RPM_SUSPENDED -> RPM_RESUMING -> RPM_ACTIVE */
  kpi_pm_update_status(dev, RPM_RESUMING);
  pm = kpi_pm_ops(dev);
  spin_unlock_irqrestore(&dev->power.lock, flags);

  if (!dev->power.no_callbacks && pm && pm->runtime_resume)
    retval = pm->runtime_resume(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (retval) {
    dev->power.runtime_error = retval;
    kpi_pm_update_status(dev, RPM_SUSPENDED);
  } else {
    kpi_pm_update_status(dev, RPM_ACTIVE);
  }
  wake_up_all(&dev->power.wait_queue);

out:
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return retval;
}

static int kpi_pm_schedule_autosuspend_locked(struct device *dev,
                                            unsigned long delay_jiffies) {
  struct workqueue_struct *wq = kpi_pm_wq();

  if (!wq)
    return -EAGAIN;
  mod_delayed_work(wq, &dev->power.autosuspend_work, delay_jiffies);
  return 0;
}

int __pm_runtime_suspend(struct device *dev, int rpmflags) {
  unsigned long flags;
  const struct dev_pm_ops *pm;
  int retval = 0;

  if (!dev)
    return -EINVAL;
  kpi_pm_ensure_init(dev);

  if (rpmflags & RPM_GET_PUT) {
    if (!atomic_dec_and_test(&dev->power.usage_count))
      return 0;
  }

  spin_lock_irqsave(&dev->power.lock, flags);

repeat:
  retval = kpi_pm_check_suspend_allowed(dev);
  if (retval)
    goto out;

  if (dev->power.runtime_status == RPM_SUSPENDED) {
    retval = 1;
    goto out;
  }
  if (dev->power.runtime_status == RPM_SUSPENDING) {
    if (rpmflags & (RPM_ASYNC | RPM_NOWAIT)) {
      retval = -EINPROGRESS;
      goto out;
    }
    kpi_pm_wait_transition_locked(dev, &flags);
    goto repeat;
  }
  if (dev->power.runtime_status == RPM_RESUMING) {
    if (rpmflags & (RPM_ASYNC | RPM_NOWAIT)) {
      retval = -EAGAIN;
      goto out;
    }
    kpi_pm_wait_transition_locked(dev, &flags);
    goto repeat;
  }

  if ((rpmflags & RPM_AUTO) && dev->power.use_autosuspend &&
      dev->power.autosuspend_delay >= 0) {
    u64 expires_ns = READ_ONCE(dev->power.last_busy) +
                     (u64)dev->power.autosuspend_delay * 1000000ULL;
    u64 now_ns = ktime_get_mono_fast_ns();

    if (expires_ns > now_ns) {
      unsigned long delay = nsecs_to_jiffies(expires_ns - now_ns);
      if (delay == 0)
        delay = 1;
      retval = kpi_pm_schedule_autosuspend_locked(dev, delay);
      goto out;
    }
  }

  if (rpmflags & RPM_ASYNC) {
    struct workqueue_struct *wq = kpi_pm_wq();

    if (wq) {
      dev->power.request =
          (rpmflags & RPM_AUTO) ? RPM_REQ_AUTOSUSPEND : RPM_REQ_SUSPEND;
      queue_work(wq, &dev->power.work);
      retval = 0;
      goto out;
    }
  }

  /* RPM_ACTIVE -> RPM_SUSPENDING -> RPM_SUSPENDED */
  kpi_pm_update_status(dev, RPM_SUSPENDING);
  pm = kpi_pm_ops(dev);
  spin_unlock_irqrestore(&dev->power.lock, flags);

  if (!dev->power.no_callbacks && pm && pm->runtime_suspend)
    retval = pm->runtime_suspend(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (retval) {
    if (retval != -EAGAIN && retval != -EBUSY)
      dev->power.runtime_error = retval;
    kpi_pm_update_status(dev, RPM_ACTIVE);
  } else {
    kpi_pm_update_status(dev, RPM_SUSPENDED);
  }
  wake_up_all(&dev->power.wait_queue);

out:
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return retval;
}

int __pm_runtime_idle(struct device *dev, int rpmflags) {
  unsigned long flags;
  const struct dev_pm_ops *pm;
  int retval = 0;

  if (!dev)
    return -EINVAL;
  kpi_pm_ensure_init(dev);

  if (rpmflags & RPM_GET_PUT) {
    if (!atomic_dec_and_test(&dev->power.usage_count))
      return 0;
  }

  spin_lock_irqsave(&dev->power.lock, flags);
  retval = kpi_pm_check_suspend_allowed(dev);
  if (retval) {
    spin_unlock_irqrestore(&dev->power.lock, flags);
    return retval;
  }

  if (dev->power.runtime_status == RPM_SUSPENDED) {
    spin_unlock_irqrestore(&dev->power.lock, flags);
    return 1;
  }
  if (dev->power.runtime_status != RPM_ACTIVE) {
    spin_unlock_irqrestore(&dev->power.lock, flags);
    return -EAGAIN;
  }

  if (rpmflags & RPM_ASYNC) {
    struct workqueue_struct *wq = kpi_pm_wq();

    if (wq) {
      dev->power.request = RPM_REQ_IDLE;
      queue_work(wq, &dev->power.work);
      spin_unlock_irqrestore(&dev->power.lock, flags);
      return 0;
    }
  }

  pm = kpi_pm_ops(dev);
  spin_unlock_irqrestore(&dev->power.lock, flags);

  if (!dev->power.no_callbacks && pm && pm->runtime_idle) {
    retval = pm->runtime_idle(dev);
    if (retval)
      return retval;
  }

  return __pm_runtime_suspend(dev, dev->power.use_autosuspend ? RPM_AUTO : 0);
}

int pm_runtime_get_if_active(struct device *dev) {
  unsigned long flags;
  int retval;

  if (!dev)
    return -EINVAL;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (dev->power.disable_depth > 0) {
    retval = -EINVAL;
  } else if (dev->power.runtime_status == RPM_ACTIVE) {
    atomic_inc(&dev->power.usage_count);
    retval = 1;
  } else {
    retval = 0;
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return retval;
}

int pm_runtime_get_if_in_use(struct device *dev) {
  unsigned long flags;
  int retval;

  if (!dev)
    return -EINVAL;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (dev->power.disable_depth > 0) {
    retval = -EINVAL;
  } else if (dev->power.runtime_status == RPM_ACTIVE &&
             atomic_read(&dev->power.usage_count) > 0) {
    atomic_inc(&dev->power.usage_count);
    retval = 1;
  } else {
    retval = 0;
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return retval;
}

int pm_schedule_suspend(struct device *dev, unsigned int delay) {
  unsigned long flags;
  int retval;

  if (!dev)
    return -EINVAL;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  retval = kpi_pm_check_suspend_allowed(dev);
  if (!retval) {
    if (delay == 0) {
      struct workqueue_struct *wq = kpi_pm_wq();
      if (wq) {
        dev->power.request = RPM_REQ_SUSPEND;
        queue_work(wq, &dev->power.work);
      } else {
        retval = -EAGAIN;
      }
    } else {
      retval = kpi_pm_schedule_autosuspend_locked(dev, msecs_to_jiffies(delay));
    }
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return retval;
}

int __pm_runtime_set_status(struct device *dev, unsigned int status) {
  unsigned long flags;
  int error = 0;

  if (!dev || (status != RPM_ACTIVE && status != RPM_SUSPENDED))
    return -EINVAL;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (!dev->power.runtime_error && dev->power.disable_depth == 0) {
    error = -EAGAIN;
  } else {
    dev->power.runtime_error = 0;
    kpi_pm_update_status(dev, (enum rpm_status)status);
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return error;
}

int pm_runtime_barrier(struct device *dev) {
  unsigned long flags;
  int retval = 0;

  if (!dev)
    return -EINVAL;
  kpi_pm_ensure_init(dev);

  cancel_work_sync(&dev->power.work);
  cancel_delayed_work_sync(&dev->power.autosuspend_work);

  spin_lock_irqsave(&dev->power.lock, flags);
  kpi_pm_wait_transition_locked(dev, &flags);
  if (dev->power.runtime_error)
    retval = -EINVAL;
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return retval;
}

bool pm_runtime_block_if_disabled(struct device *dev) {
  unsigned long flags;
  bool disabled;

  if (!dev)
    return false;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  disabled = dev->power.disable_depth > 0;
  if (disabled) {
    dev->power.last_status = RPM_BLOCKED;
    dev->power.disable_depth++;
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return disabled;
}

void pm_runtime_unblock(struct device *dev) {
  unsigned long flags;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (dev->power.last_status == RPM_BLOCKED) {
    dev->power.last_status = dev->power.runtime_status;
    if (dev->power.disable_depth > 0)
      dev->power.disable_depth--;
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);
}

void pm_runtime_enable(struct device *dev) {
  unsigned long flags;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (dev->power.disable_depth > 0)
    dev->power.disable_depth--;
  spin_unlock_irqrestore(&dev->power.lock, flags);
}

void __pm_runtime_disable(struct device *dev, bool check_resume) {
  unsigned long flags;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (dev->power.disable_depth++ == 0 && check_resume &&
      dev->power.request == RPM_REQ_RESUME) {
    dev->power.request = RPM_REQ_NONE;
    dev->power.disable_depth--;
    spin_unlock_irqrestore(&dev->power.lock, flags);
    __pm_runtime_resume(dev, 0);
    spin_lock_irqsave(&dev->power.lock, flags);
    dev->power.disable_depth++;
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);

  if (dev->power.disable_depth == 1)
    pm_runtime_barrier(dev);
}

void pm_runtime_allow(struct device *dev) {
  unsigned long flags;
  bool idle = false;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (!dev->power.runtime_auto) {
    dev->power.runtime_auto = true;
    if (atomic_dec_and_test(&dev->power.usage_count))
      idle = true;
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);

  if (idle)
    __pm_runtime_idle(dev, RPM_ASYNC);
}

void pm_runtime_forbid(struct device *dev) {
  unsigned long flags;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  if (dev->power.runtime_auto) {
    dev->power.runtime_auto = false;
    atomic_inc(&dev->power.usage_count);
    spin_unlock_irqrestore(&dev->power.lock, flags);
    __pm_runtime_resume(dev, 0);
    return;
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);
}

void pm_runtime_no_callbacks(struct device *dev) {
  unsigned long flags;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  dev->power.no_callbacks = true;
  spin_unlock_irqrestore(&dev->power.lock, flags);
}

void pm_runtime_irq_safe(struct device *dev) {
  unsigned long flags;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  dev->power.irq_safe = true;
  spin_unlock_irqrestore(&dev->power.lock, flags);
}

void __pm_runtime_use_autosuspend(struct device *dev, bool use) {
  unsigned long flags;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  dev->power.use_autosuspend = use;
  spin_unlock_irqrestore(&dev->power.lock, flags);
}

void pm_runtime_set_autosuspend_delay(struct device *dev, int delay) {
  unsigned long flags;

  if (!dev)
    return;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  dev->power.autosuspend_delay = delay;
  spin_unlock_irqrestore(&dev->power.lock, flags);
}

u64 pm_runtime_autosuspend_expiration(struct device *dev) {
  int delay;

  if (!dev)
    return 0;
  kpi_pm_ensure_init(dev);
  delay = READ_ONCE(dev->power.autosuspend_delay);
  if (delay < 0)
    return 0;
  return READ_ONCE(dev->power.last_busy) + (u64)delay * 1000000ULL;
}

u64 pm_runtime_suspended_time(struct device *dev) {
  unsigned long flags;
  u64 total;

  if (!dev)
    return 0;
  kpi_pm_ensure_init(dev);

  spin_lock_irqsave(&dev->power.lock, flags);
  total = dev->power.suspended_ns;
  if (dev->power.runtime_status == RPM_SUSPENDED) {
    u64 now = ktime_get_mono_fast_ns();
    if (now > dev->power.accounting_timestamp)
      total += now - dev->power.accounting_timestamp;
  }
  spin_unlock_irqrestore(&dev->power.lock, flags);
  return total;
}

void pm_runtime_set_memalloc_noio(struct device *dev, bool enable) {
  (void)dev;
  (void)enable;
}

void pm_runtime_get_suppliers(struct device *dev) { (void)dev; }
void pm_runtime_put_suppliers(struct device *dev) { (void)dev; }
void pm_runtime_new_link(struct device *dev) { (void)dev; }
void pm_runtime_drop_link(struct device_link *link) { (void)link; }
void pm_runtime_release_supplier(struct device_link *link) { (void)link; }

static void kpi_pm_devm_disable(void *data) {
  struct device *dev = data;
  pm_runtime_dont_use_autosuspend(dev);
  pm_runtime_disable(dev);
}

int devm_pm_runtime_enable(struct device *dev) {
  if (!dev)
    return -EINVAL;
  pm_runtime_enable(dev);
  return devm_add_action_or_reset(dev, kpi_pm_devm_disable, dev);
}

int devm_pm_runtime_set_active_enabled(struct device *dev) {
  int ret;

  if (!dev)
    return -EINVAL;
  ret = pm_runtime_set_active(dev);
  if (ret)
    return ret;
  return devm_pm_runtime_enable(dev);
}

static void kpi_pm_devm_put_noidle(void *data) {
  pm_runtime_put_noidle((struct device *)data);
}

int devm_pm_runtime_get_noresume(struct device *dev) {
  if (!dev)
    return -EINVAL;
  pm_runtime_get_noresume(dev);
  return devm_add_action_or_reset(dev, kpi_pm_devm_put_noidle, dev);
}

int dev_pm_set_wake_irq(struct device *dev, int irq) {
  (void)dev;
  (void)irq;
  return 0;
}

int dev_pm_set_dedicated_wake_irq(struct device *dev, int irq) {
  (void)dev;
  (void)irq;
  return 0;
}

int dev_pm_set_dedicated_wake_irq_reverse(struct device *dev, int irq) {
  (void)dev;
  (void)irq;
  return 0;
}

void dev_pm_clear_wake_irq(struct device *dev) { (void)dev; }

int devm_pm_set_wake_irq(struct device *dev, int irq) {
  return dev_pm_set_wake_irq(dev, irq);
}
