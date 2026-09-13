#ifndef __AVORY_LINUXKPI_MMU_NOTIFIER_H
#define __AVORY_LINUXKPI_MMU_NOTIFIER_H

/* AvoryOS overlay for <linux/mmu_notifier.h>.
 *
 * CONFIG_MMU_NOTIFIER is off and userptr/HMM are disabled, so the type only
 * has to exist for dma-resv.c's include.  Everything that would use it is
 * compiled out (or must be gated by callers). */

#include <linux/types.h>
#include <linux/mm_types.h>

struct mmu_notifier_range {
  struct mm_struct *mm;
  unsigned long start;
  unsigned long end;
  unsigned int flags;
};

#define MMU_NOTIFY_RELEASE 0
#define MMU_NOTIFY_CLEAR 1

/* amdgpu_drv.c calls this on module exit; with CONFIG_MMU_NOTIFIER off there
 * is no notifier list, so it is a no-op (stock mmu_notifier.h's
 * !CONFIG_MMU_NOTIFIER arm provides the same). */
static inline void mmu_notifier_synchronize(void) {}

#endif /* __AVORY_LINUXKPI_MMU_NOTIFIER_H */
