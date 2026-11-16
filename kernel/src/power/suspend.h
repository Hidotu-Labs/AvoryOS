#ifndef AVORY_POWER_SUSPEND_H
#define AVORY_POWER_SUSPEND_H

#include <stdint.h>

/* Prepare and unwind the S3 work from steps 1 and 2. The resume address must
 * be a kernel landing pad that restores the BSP before calling resume(). */
int power_s3_prepare(uint64_t resume_rsp, uint64_t resume_rip);
void power_s3_before_sleep(void);
void power_s3_restore_bsp_early(void);
void power_s3_cancel(void);
int power_s3_resume(void);
int power_s3_enter(void);

#endif
