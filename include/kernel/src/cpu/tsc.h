#ifndef TSC_H
#define TSC_H

#include <stdint.h>

void tsc_init(void);
uint64_t tsc_get_freq_khz(void);

#endif
