#ifndef __AVORY_LINUXKPI_PM_H
#define __AVORY_LINUXKPI_PM_H

/* AvoryOS overlay for <linux/pm.h> (6.6 layout, Phase 5 C5).
 *
 * Stock pm.h pulls export/workqueue/timer/completion headers and redefines
 * pm_message_t, which <linux/device.h> already provides here.  The overlay
 * therefore carries the device-PM surface drivers actually initialize: the
 * pm_message_t type, the full 6.6 dev_pm_ops field set (amdgpu's pm_ops
 * initializes 12+ callbacks by name), the *_PM_OPS helper macros and the
 * pm_ptr family.  There is no PM core: SET_SYSTEM/LATE/NOIRQ/RUNTIME_PM_OPS
 * compile to nothing because CONFIG_PM/CONFIG_PM_SLEEP are unset, matching
 * upstream's #else arms, and real suspend/resume is Phase 8 work.
 *
 * Recorded in docs/linuxkpi-gaps.md (P5 C5). */

#include <linux/types.h>

struct device;

/* Keep the same spelling as upstream pm_message_t so stock headers that use
 * it (pci.h) and callback signatures agree. */
typedef struct pm_message {
  int event;
} pm_message_t;

struct dev_pm_ops {
  int (*prepare)(struct device *dev);
  void (*complete)(struct device *dev);
  int (*suspend)(struct device *dev);
  int (*resume)(struct device *dev);
  int (*freeze)(struct device *dev);
  int (*thaw)(struct device *dev);
  int (*poweroff)(struct device *dev);
  int (*restore)(struct device *dev);
  int (*suspend_late)(struct device *dev);
  int (*resume_early)(struct device *dev);
  int (*freeze_late)(struct device *dev);
  int (*thaw_early)(struct device *dev);
  int (*poweroff_late)(struct device *dev);
  int (*restore_early)(struct device *dev);
  int (*suspend_noirq)(struct device *dev);
  int (*resume_noirq)(struct device *dev);
  int (*freeze_noirq)(struct device *dev);
  int (*thaw_noirq)(struct device *dev);
  int (*poweroff_noirq)(struct device *dev);
  int (*restore_noirq)(struct device *dev);
  int (*runtime_suspend)(struct device *dev);
  int (*runtime_resume)(struct device *dev);
  int (*runtime_idle)(struct device *dev);
};

/* A device's power-management domain.  amdgpu embeds one in struct
 * amdgpu_device (vga_pm_domain); only the type is needed because runtime PM
 * is inert (docs/linuxkpi-gaps.md, P5 C5). */
struct dev_pm_domain {
  struct dev_pm_ops ops;
  int (*start)(struct device *dev);
  void (*dismiss)(struct device *dev);
};

/* Runtime-PM status enums: stock <linux/pm_runtime.h> uses these in its
 * !CONFIG_PM inline helpers even though no runtime PM exists. */
enum rpm_status {
  RPM_INVALID = -1,
  RPM_ACTIVE = 0,
  RPM_RESUMING,
  RPM_SUSPENDED,
  RPM_SUSPENDING,
};

enum rpm_request {
  RPM_REQ_NONE = 0,
  RPM_REQ_IDLE,
  RPM_REQ_SUSPEND,
  RPM_REQ_AUTOSUSPEND,
  RPM_REQ_RESUME,
};

/* The field-list macros are real even without a PM core, so drivers can
 * build dev_pm_ops from named callbacks. */
#define SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)                             \
  .suspend = pm_sleep_ptr(suspend_fn), .resume = pm_sleep_ptr(resume_fn),      \
  .freeze = pm_sleep_ptr(suspend_fn), .thaw = pm_sleep_ptr(resume_fn),         \
  .poweroff = pm_sleep_ptr(suspend_fn), .restore = pm_sleep_ptr(resume_fn),

#define LATE_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)                        \
  .suspend_late = pm_sleep_ptr(suspend_fn),                                    \
  .resume_early = pm_sleep_ptr(resume_fn),                                     \
  .freeze_late = pm_sleep_ptr(suspend_fn),                                     \
  .thaw_early = pm_sleep_ptr(resume_fn),                                       \
  .poweroff_late = pm_sleep_ptr(suspend_fn),                                   \
  .restore_early = pm_sleep_ptr(resume_fn),

#define NOIRQ_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)                       \
  .suspend_noirq = pm_sleep_ptr(suspend_fn),                                   \
  .resume_noirq = pm_sleep_ptr(resume_fn),                                     \
  .freeze_noirq = pm_sleep_ptr(suspend_fn),                                    \
  .thaw_noirq = pm_sleep_ptr(resume_fn),                                       \
  .poweroff_noirq = pm_sleep_ptr(suspend_fn),                                  \
  .restore_noirq = pm_sleep_ptr(resume_fn),

#define RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn)                         \
  .runtime_suspend = suspend_fn, .runtime_resume = resume_fn,                  \
  .runtime_idle = idle_fn,

#define SET_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)
#define SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)
#define SET_RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn)

#define pm_ptr(_ptr) NULL
#define pm_sleep_ptr(_ptr) NULL
#define pm_runtime_ptr(_ptr) NULL

/* PM driver flags (stock pm.h values) and the TRUE/FALSE constants upstream
 * picks up from <acpi/actypes.h> when CONFIG_ACPI is on.  amdgpu_drv.c's
 * runpm block needs them although runtime PM is inert here. */
#define DPM_FLAG_NO_DIRECT_COMPLETE (1U << 0)
#define DPM_FLAG_SMART_PREPARE (1U << 1)
#define DPM_FLAG_SMART_SUSPEND (1U << 2)
#define DPM_FLAG_MAY_SKIP_RESUME (1U << 3)

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif /* __AVORY_LINUXKPI_PM_H */
