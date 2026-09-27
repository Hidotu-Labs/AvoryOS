#include "dma.h"
#include "../io/io.h"

void dma_start(uint8_t channel, uint8_t mode, uint32_t buf_phys, uint32_t length) {
    if (channel < 4) {
        uint8_t dma_page[] = {0x87, 0x83, 0x81, 0x82};
        uint8_t addr_port = channel * 2;
        uint8_t count_port = channel * 2 + 1;
        
        outb(0x0A, 0x04 | channel);
        outb(0x0C, 0x00);
        outb(0x0B, mode | channel);
        
        uint16_t offset = buf_phys & 0xFFFF;
        outb(addr_port, offset & 0xFF);
        outb(addr_port, (offset >> 8) & 0xFF);
        
        outb(dma_page[channel], (buf_phys >> 16) & 0xFF);
        
        uint16_t count = length - 1;
        outb(count_port, count & 0xFF);
        outb(count_port, (count >> 8) & 0xFF);
        
        outb(0x0A, channel);
    } else if (channel >= 5 && channel <= 7) {
        uint8_t dma_page[] = {0, 0x8B, 0x89, 0x8A}; // indices: channel - 4
        uint8_t chan_idx = channel - 4;             // channel 5 -> chan_idx 1
        uint8_t addr_port = 0xC0 + chan_idx * 4;
        uint8_t count_port = 0xC2 + chan_idx * 4;
        
        // 1. Mask channel
        outb(0xD4, 0x04 | chan_idx);
        // 2. Clear byte pointer flip-flop
        outb(0xD8, 0x00);
        // 3. Set mode
        outb(0xD6, mode | chan_idx);
        
        // 4. Set offset (divided by 2 for 16-bit DMA words)
        uint16_t offset = (buf_phys >> 1) & 0xFFFF;
        outb(addr_port, offset & 0xFF);
        outb(addr_port, (offset >> 8) & 0xFF);
        
        // 5. Set page register (bit 0 must be masked out for 16-bit arrays to prevent address wrapping defects in some VMs)
        outb(dma_page[chan_idx], (buf_phys >> 16) & 0xFE);
        
        // 6. Set length (in words)
        uint16_t count = (length / 2) - 1;
        outb(count_port, count & 0xFF);
        outb(count_port, (count >> 8) & 0xFF);
        
        // 7. Unmask channel
        outb(0xD4, chan_idx);
    }
}
