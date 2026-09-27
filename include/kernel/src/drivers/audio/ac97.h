#ifndef AUDIO_AC97_H
#define AUDIO_AC97_H

#include <stdbool.h>
#include <stdint.h>

// NAM (Native Audio Mixer) Registers
#define AC97_NAM_RESET 0x00
#define AC97_NAM_MASTER_VOLUME 0x02
#define AC97_NAM_MIC_VOLUME 0x0E
#define AC97_NAM_PCM_OUT_VOLUME 0x18
#define AC97_NAM_EXT_AUDIO_ID 0x28
#define AC97_NAM_EXT_AUDIO_CTRL 0x2A
#define AC97_NAM_PCM_FRONT_DAC_RATE 0x2C

#define AC97_EXT_AUDIO_VRA (1 << 0)

// NABM (Native Audio Bus Master) Registers
#define AC97_PO_BBA 0x10 // PCM Out Buffer Descriptor List Base Address
#define AC97_PO_LVI 0x15 // PCM Out Last Valid Index
#define AC97_PO_SR 0x16  // PCM Out Status Register
#define AC97_PO_CIV 0x14 // PCM Out Current Index Value
#define AC97_PO_CR 0x1B  // PCM Out Control Register

#define AC97_GLOB_CNT 0x2C // Global Control
#define AC97_GLOB_STA 0x30 // Global Status

// NABM Control Register Bits
#define AC97_CR_RPBM (1 << 0)  // Run/Pause Bus Master
#define AC97_CR_RR (1 << 1)    // Reset Registers
#define AC97_CR_LVBIE (1 << 2) // Last Valid Buffer Interrupt Enable
#define AC97_CR_FEIE (1 << 3)  // FIFO Error Interrupt Enable
#define AC97_CR_IOCE (1 << 4)  // Interrupt On Completion Enable

// NABM Status Register Bits
#define AC97_SR_DCH (1 << 0)      // DMA Controller Halted
#define AC97_SR_CELV (1 << 1)     // Current Entry is Last Valid
#define AC97_SR_LVBCI (1 << 2)    // Last Valid Buffer Completion Interrupt
#define AC97_SR_BCIS (1 << 3)     // Buffer Completion Interrupt Status
#define AC97_SR_FIFO_ERR (1 << 4) // FIFO Error

// Buffer Descriptor Structure
struct ac97_bdl_entry {
  uint32_t pointer; // Physical address of buffer
  uint16_t length;  // Length in samples
  uint16_t flags;   // Bit 15=IOC, Bit 14=BUP
} __attribute__((packed));

#define AC97_BDL_FLAG_IOC (1 << 15) // Interrupt On Completion
#define AC97_BDL_FLAG_BUP (1 << 14) // Buffer Underrun Policy

// Public API
void ac97_init(void);
void ac97_register_vfs(void);

#endif
