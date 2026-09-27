#ifndef AUDIO_HDA_H
#define AUDIO_HDA_H

#include <stdbool.h>
#include <stdint.h>
#include "../../fs/vfs.h"

// Intel High Definition Audio (HDA) Controller Registers (BAR0 MMIO)
#define HDA_REG_GCAP        0x00 // Global Capabilities (16-bit)
#define HDA_REG_VMIN        0x02 // Minor Version (8-bit)
#define HDA_REG_VMAJ        0x03 // Major Version (8-bit)
#define HDA_REG_OUTPAY      0x04 // Output Payload Capability (16-bit)
#define HDA_REG_INPAY       0x06 // Input Payload Capability (16-bit)
#define HDA_REG_GCTL        0x08 // Global Control (32-bit)
#define HDA_REG_WAKEEN      0x0C // Wake Enable (16-bit)
#define HDA_REG_STATESTS    0x0E // State Change Status (16-bit)
#define HDA_REG_GSTS        0x10 // Global Status (16-bit)
#define HDA_REG_INTCTL      0x20 // Interrupt Control (32-bit)
#define HDA_REG_INTSTS      0x24 // Interrupt Status (32-bit)
#define HDA_REG_WALCLK      0x30 // Wall Clock Counter (32-bit)
#define HDA_REG_SSYNC       0x38 // Stream Synchronization (32-bit)

// Immediate Command Registers
#define HDA_REG_IC          0x60 // Immediate Command Output (32-bit)
#define HDA_REG_IR          0x64 // Immediate Response Input (32-bit)
#define HDA_REG_ICS         0x68 // Immediate Command Status (16-bit)

// Global Control Register Bits
#define HDA_GCTL_CRST       (1 << 0) // Controller Reset (1 = Operational, 0 = Reset)

// Immediate Command Status Bits
#define HDA_ICS_ICB         (1 << 0) // Immediate Command Busy
#define HDA_ICS_IRV         (1 << 1) // Immediate Result Valid

// Interrupt Control Bits
#define HDA_INTCTL_GIE      (1U << 31) // Global Interrupt Enable
#define HDA_INTCTL_CIE      (1U << 30) // Controller Interrupt Enable
#define HDA_INTCTL_SIE(s)   (1U << (s)) // Stream Interrupt Enable

// Stream Descriptor Registers (Offset: 0x80 + stream_index * 0x20)
#define HDA_SD_BASE         0x80
#define HDA_SD_SIZE         0x20

#define HDA_SD_CTL          0x00 // Stream Control (24-bit / 32-bit)
#define HDA_SD_STS          0x03 // Stream Status (8-bit)
#define HDA_SD_LPIB         0x04 // Link Position in Buffer (32-bit)
#define HDA_SD_CBL          0x08 // Cyclic Buffer Length (32-bit)
#define HDA_SD_LVI          0x0C // Last Valid Index (16-bit)
#define HDA_SD_FIFOS        0x0E // FIFO Size (16-bit)
#define HDA_SD_FMT          0x12 // Stream Format (16-bit)
#define HDA_SD_BDLPL        0x18 // Buffer Descriptor List Pointer Lower (32-bit)
#define HDA_SD_BDLPU        0x1C // Buffer Descriptor List Pointer Upper (32-bit)

// Stream Control Register Bits
#define HDA_SD_CTL_SRST     (1 << 0) // Stream Reset
#define HDA_SD_CTL_RUN      (1 << 1) // Stream Run (DMA Enable)
#define HDA_SD_CTL_IOCE     (1 << 2) // Interrupt On Completion Enable
#define HDA_SD_CTL_FEIE     (1 << 3) // FIFO Error Interrupt Enable
#define HDA_SD_CTL_DEIE     (1 << 4) // Descriptor Error Interrupt Enable
#define HDA_SD_CTL_STRIPE(s) (((s) & 0x3) << 16)
#define HDA_SD_CTL_STREAM(s) (((s) & 0xF) << 20) // Stream ID Tag (1-15)

// Stream Status Register Bits
#define HDA_SD_STS_BCIS     (1 << 2) // Buffer Completion Interrupt Status
#define HDA_SD_STS_FIFOE    (1 << 3) // FIFO Error
#define HDA_SD_STS_DESE     (1 << 4) // Descriptor Error

// Audio Formats (SD_FMT)
// 48 kHz, 16-bit, 2 channels (stereo) = (0 << 14) | (0 << 11) | (0 << 8) | (1 << 4) | (1 << 0) = 0x0011
#define HDA_FMT_48KHZ_16BIT_STEREO 0x0011
// 44.1 kHz, 16-bit, 2 channels (stereo) = (1 << 14) | (0 << 11) | (0 << 8) | (1 << 4) | (1 << 0) = 0x4011
#define HDA_FMT_44KHZ_16BIT_STEREO 0x4011

// Buffer Descriptor List (BDL) Entry
struct hda_bdl_entry {
    uint32_t addr_low;   // Lower 32 bits of physical address
    uint32_t addr_high;  // Upper 32 bits of physical address
    uint32_t length;     // Length of buffer in bytes
    uint32_t flags;      // Bit 0: IOC (Interrupt On Completion)
} __attribute__((packed));

#define HDA_BDL_FLAG_IOC    (1 << 0)

// Codec Verbs & Parameters
#define HDA_VERB_GET_PARAM             0xF00
#define HDA_VERB_SET_CONV_STREAM_CHAN  0x706
#define HDA_VERB_SET_PIN_WIDGET_CTRL   0x707
#define HDA_VERB_SET_UNSOLICITED_ENABLE 0x708
#define HDA_VERB_SET_EAPD_BTLENABLE    0x70C
#define HDA_VERB_SET_POWER_STATE       0x705
#define HDA_VERB_SET_AMP_GAIN_MUTE     0x300
#define HDA_VERB_SET_CONV_FMT          0x200
#define HDA_VERB_GET_CONFIG_DEFAULT    0xF1C
#define HDA_VERB_SET_CONNECT_SEL       0x701

// Parameter IDs
#define HDA_PARAM_VENDOR_ID            0x00
#define HDA_PARAM_REVISION_ID          0x02
#define HDA_PARAM_SUB_NODE_COUNT       0x04
#define HDA_PARAM_FUNC_GROUP_TYPE      0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP     0x09
#define HDA_PARAM_PCM_SIZE_RATE        0x0A
#define HDA_PARAM_STREAM_FORMATS       0x0B
#define HDA_PARAM_PIN_CAP              0x0C
#define HDA_PARAM_INPUT_AMP_CAP        0x0D
#define HDA_PARAM_OUTPUT_AMP_CAP       0x12

// Widget Types
#define HDA_WIDGET_AUDIO_OUTPUT        0x0
#define HDA_WIDGET_AUDIO_INPUT         0x1
#define HDA_WIDGET_AUDIO_MIXER         0x2
#define HDA_WIDGET_AUDIO_SELECTOR      0x3
#define HDA_WIDGET_PIN_COMPLEX         0x4
#define HDA_WIDGET_POWER_WIDGET        0x5
#define HDA_WIDGET_VOLUME_KNOB         0x6
#define HDA_WIDGET_BEEP_GENERATOR      0x7

// Public Driver API
void hda_init(void);
void hda_register_vfs(void);
bool hda_is_present(void);

// Audio playback routines
uint32_t hda_write_pcm(const void *buffer, uint32_t bytes, uint32_t rate, uint8_t channels, uint8_t bits);
int hda_ioctl_handler(uint32_t request, uint64_t arg);
int hda_poll_handler(int events);
uint32_t hda_get_ring_count(void);
uint64_t hda_get_played_bytes(void);
void *hda_get_wait_queue(void);
void hda_reset_stream(void);
void hda_set_format(uint32_t rate, uint8_t channels, uint8_t bits);

#endif // AUDIO_HDA_H