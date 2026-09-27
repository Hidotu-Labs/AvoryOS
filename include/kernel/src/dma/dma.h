#ifndef DMA_H
#define DMA_H

#include <stdint.h>

#define DMA_MODE_READ 0x44     // Read from memory to device
#define DMA_MODE_WRITE 0x48    // Write from device to memory
#define DMA_MODE_SINGLE 0x40   // Single transfer mode
#define DMA_MODE_AUTOINIT 0x10 // Auto-initialize

// Initialize a DMA transfer for a specific channel
void dma_start(uint8_t channel, uint8_t mode, uint32_t buf_phys,
               uint32_t length);

#endif
