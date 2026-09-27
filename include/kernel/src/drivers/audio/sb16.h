#ifndef SB16_H
#define SB16_H

#include <stdbool.h>
#include <stdint.h>

extern volatile uint32_t sb16_irq_fired;

void sb16_reserve_dma(void); // Reserve ISA DMA buffer early (before PCI/storage)
void sb16_init(void);
void sb16_play_chunk(uint32_t phys_addr, uint32_t length, uint16_t sample_rate,
                     uint8_t channels, uint8_t bits);

// OSS /dev/dsp interface
void sb16_register_vfs(void);
void sb16_set_format(uint32_t rate, uint8_t channels, uint8_t bits);
uint32_t sb16_get_sample_rate(void);
uint8_t sb16_get_channels(void);
uint8_t sb16_get_bits(void);

#endif
