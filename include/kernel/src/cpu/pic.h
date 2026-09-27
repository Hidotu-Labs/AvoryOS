#ifndef PIC_H
#define PIC_H

#include <stdint.h>

// Remap the PIC to offset vectors (typically 32 to 47 instead of 0 to 15)
void pic_remap(int offset1, int offset2);

// Send the End Of Interrupt signal to the given IRQ line
void pic_send_eoi(uint8_t irq);

// Only allow interrupts from a specific IRQ line
void pic_clear_mask(uint8_t irq);

// Completely mask/disable the legacy PICs
void pic_disable(void);

#endif
