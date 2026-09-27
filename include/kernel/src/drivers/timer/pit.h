#ifndef PIT_H
#define PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency);
uint64_t pit_get_ticks(void);
void pit_sleep(uint32_t ms);

#endif
