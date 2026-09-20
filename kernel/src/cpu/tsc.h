#ifndef TSC_H
#define TSC_H

#include <stdint.h>

void tsc_init(void);
uint64_t tsc_get_freq_khz(void);

/* Re-measure the TSC rate against the HPET.  Call after hpet_init(); used to
 * repair a zero/implausible PIT-based calibration. */
void tsc_recalibrate_hpet(void);

#endif
