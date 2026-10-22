#ifndef __AVORY_LINUXKPI_SUSPEND_H
#define __AVORY_LINUXKPI_SUSPEND_H

/* State identifiers remain part of driver data even when CONFIG_SUSPEND is
 * disabled. Native system sleep is not implemented by this interface. */
#include <linux/types.h>
typedef int __bitwise suspend_state_t;
#define PM_SUSPEND_ON ((__force suspend_state_t)0)
#define PM_SUSPEND_TO_IDLE ((__force suspend_state_t)1)
#define PM_SUSPEND_STANDBY ((__force suspend_state_t)2)
#define PM_SUSPEND_MEM ((__force suspend_state_t)3)
#define PM_SUSPEND_MIN PM_SUSPEND_TO_IDLE
#define PM_SUSPEND_MAX ((__force suspend_state_t)4)
/* Match upstream's !CONFIG_PM_SLEEP / !CONFIG_HIBERNATION paths. */
struct notifier_block;
#define PM_HIBERNATION_PREPARE 0x0001
#define PM_POST_HIBERNATION 0x0002
static inline bool pm_resume_via_firmware(void) { return false; }
static inline bool pm_hibernate_is_recovering(void) { return false; }
static inline bool pm_hibernation_mode_is_suspend(void) { return false; }
static inline int register_pm_notifier(struct notifier_block *nb) {
  (void)nb; return 0;
}
static inline int unregister_pm_notifier(struct notifier_block *nb) {
  (void)nb; return 0;
}
static inline void ksys_sync_helper(void) { }
#endif
