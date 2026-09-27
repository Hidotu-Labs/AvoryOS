#ifndef DRIVERS_USB_OHCI_H
#define DRIVERS_USB_OHCI_H

#include "usb.h"
#include <stdbool.h>
#include <stdint.h>

// OHCI MMIO Register Offsets
#define OHCI_REG_REVISION 0x00
#define OHCI_REG_CONTROL 0x04
#define OHCI_REG_COMMAND_STATUS 0x08
#define OHCI_REG_INTERRUPT_STATUS 0x0C
#define OHCI_REG_INTERRUPT_ENABLE 0x10
#define OHCI_REG_INTERRUPT_DISABLE 0x14
#define OHCI_REG_HCCA 0x18
#define OHCI_REG_PERIOD_CURRENT_ED 0x1C
#define OHCI_REG_CONTROL_HEAD_ED 0x20
#define OHCI_REG_CONTROL_CURRENT_ED 0x24
#define OHCI_REG_BULK_HEAD_ED 0x28
#define OHCI_REG_BULK_CURRENT_ED 0x2C
#define OHCI_REG_DONE_HEAD 0x30
#define OHCI_REG_FM_INTERVAL 0x34
#define OHCI_REG_FM_REMAINING 0x38
#define OHCI_REG_FM_NUMBER 0x3C
#define OHCI_REG_PERIODIC_START 0x40
#define OHCI_REG_LS_THRESHOLD 0x44
#define OHCI_REG_RH_DESCRIPTOR_A 0x48
#define OHCI_REG_RH_DESCRIPTOR_B 0x4C
#define OHCI_REG_RH_STATUS 0x50
#define OHCI_REG_RH_PORT_STATUS 0x54 // Array of ports

// HcControl bits
#define OHCI_CTRL_CLE (1 << 4) // Control List Enable
#define OHCI_CTRL_BLE (1 << 5) // Bulk List Enable
#define OHCI_CTRL_PLE (1 << 2) // Periodic List Enable
#define OHCI_CTRL_IE (1 << 3)  // Isochronous Enable

#define OHCI_CTRL_HCFS_MASK 0xC0 // Host Controller Functional State
#define OHCI_CTRL_HCFS_RESET 0x00
#define OHCI_CTRL_HCFS_RESUME 0x40
#define OHCI_CTRL_HCFS_OPERATIONAL 0x80
#define OHCI_CTRL_HCFS_SUSPEND 0xC0

// HcCommandStatus bits
#define OHCI_CMD_HCR (1 << 0) // Host Controller Reset
#define OHCI_CMD_CLF (1 << 1) // Control List Filled

// HcInterrupt bits
#define OHCI_INTR_MIE (1 << 31) // Master Interrupt Enable
#define OHCI_INTR_WDH (1 << 1)  // Writeback Done Head
#define OHCI_INTR_RHSC (1 << 6) // Root Hub Status Change
#define OHCI_INTR_UE (1 << 4)   // Unrecoverable Error

// HcRhDescriptorA bits
#define OHCI_RHA_NPS (1 << 9)  // No Power Switching
#define OHCI_RHA_PSM (1 << 8)  // Power Switching Mode
#define OHCI_RHA_NDP_MASK 0xFF // Number Downstream Ports

// HcRhPortStatus bits (read/write-1-to-set/write-1-to-clear)
#define OHCI_PORT_CCS (1 << 0)   // Current Connect Status (R)
#define OHCI_PORT_PES (1 << 1)   // Port Enable Status (R/W1S)
#define OHCI_PORT_PSS (1 << 2)   // Port Suspend Status
#define OHCI_PORT_POCI (1 << 3)  // Port Over Current Indicator
#define OHCI_PORT_PRS (1 << 4)   // Port Reset Status (W1S)
#define OHCI_PORT_PPS (1 << 8)   // Port Power Status (W1S)
#define OHCI_PORT_LSDA (1 << 9)  // Low Speed Device Attached (R)
#define OHCI_PORT_CSC (1 << 16)  // Connect Status Change (W1C)
#define OHCI_PORT_PESC (1 << 17) // Port Enable Status Change (W1C)
#define OHCI_PORT_PSSC (1 << 18) // Port Suspend Status Change
#define OHCI_PORT_OCIC (1 << 19) // Over Current Indicator Change
#define OHCI_PORT_PRSC (1 << 20) // Port Reset Status Change (W1C)

// TD Control Field bits
// Bits [20:19] = Direction PID: 00=SETUP, 01=OUT, 10=IN
#define OHCI_TD_DP_SETUP (0 << 19)
#define OHCI_TD_DP_OUT (1 << 19)
#define OHCI_TD_DP_IN (2 << 19)
// Bits [25:24] = Delay Interrupt (DI): 7 = no interrupt
#define OHCI_TD_DI_NONE (7 << 21)
#define OHCI_TD_DI_IMM (0 << 21) // Immediate interrupt
// Bits [25] = Data Toggle: 0 from ED, 1 from TD
#define OHCI_TD_TOGGLE_ED (0 << 24) // Get toggle from ED toggleCarry
#define OHCI_TD_TOGGLE_0 ((1 << 24) | (0 << 25)) // Force DATA0
#define OHCI_TD_TOGGLE_1 ((1 << 24) | (1 << 25)) // Force DATA1
// Bit [28] = Buffer Rounding (allow short packets)
#define OHCI_TD_ROUNDING (1 << 18)
// Condition code [31:28] (set by HC, 0xF = not accessed yet)
#define OHCI_TD_CC_MASK 0xF0000000
#define OHCI_TD_CC_SHIFT 28
#define OHCI_TD_CC_NOERR 0x00000000
#define OHCI_TD_CC_NOT_ACCESSED 0xF0000000

// ED Control Field bits
#define OHCI_ED_FA_SHIFT 0       // Function Address [6:0]
#define OHCI_ED_EN_SHIFT 7       // Endpoint Number [10:7]
#define OHCI_ED_DIR_TD (0 << 11) // Direction from TD
#define OHCI_ED_DIR_OUT (1 << 11)
#define OHCI_ED_DIR_IN (2 << 11)
#define OHCI_ED_SPEED_FULL (0 << 13)
#define OHCI_ED_SPEED_LOW (1 << 13)
#define OHCI_ED_SKIP (1 << 14)    // Skip this ED
#define OHCI_ED_FMT_GEN (0 << 15) // General TD format
#define OHCI_ED_MPS_SHIFT 16      // Max Packet Size [26:16]
// Head pointer bits
#define OHCI_ED_HEAD_HALT (1 << 0)  // Halted
#define OHCI_ED_HEAD_CARRY (1 << 1) // Toggle carry

// OHCI Structures

// Host Controller Communications Area (Must be 256-byte aligned)
struct ohci_hcca {
  uint32_t interrupt_table[32];
  uint16_t frame_number;
  uint16_t pad1;
  uint32_t done_head;
  uint8_t reserved[116];
} __attribute__((packed, aligned(256)));

// Endpoint Descriptor (Must be 16-byte aligned)
struct ohci_ed {
  volatile uint32_t control;
  volatile uint32_t tail_p;
  volatile uint32_t head_p;
  volatile uint32_t next_ed;
} __attribute__((packed, aligned(16)));

// Transfer Descriptor (Must be 16-byte aligned)
struct ohci_td {
  volatile uint32_t control;
  volatile uint32_t buffer_p;
  volatile uint32_t next_td;
  volatile uint32_t buffer_end;
} __attribute__((packed, aligned(16)));

#define OHCI_MAX_EDS 64
#define OHCI_MAX_TDS 128

// OHCI Controller State
#define OHCI_MAX_CONTROLLERS 4

struct ohci_controller {
  uintptr_t mmio_base;
  uint8_t irq_line;
  uint8_t pci_bus, pci_slot, pci_func;
  uint16_t vendor_id, device_id;
  uint8_t num_ports;
  bool present;
  bool irq_registered;
  uint64_t interrupts;

  struct ohci_hcca *hcca;
  uint32_t hcca_phys;

  struct ohci_ed *ed_pool;
  uint32_t ed_pool_phys;
  int ed_used; // Control transfer ED allocation (starts at OHCI_CTRL_ED_START)
  int int_ed_used; // Interrupt pipe ED allocation (0 .. OHCI_INT_ED_MAX-1)

  struct ohci_td *td_pool;
  uint32_t td_pool_phys;
  int td_used; // Control transfer TD allocation (starts at OHCI_CTRL_TD_START)
  int int_td_used; // Interrupt pipe TD allocation (0 .. OHCI_INT_TD_MAX-1)

  // DMA buffer for control request data and small transfers
  void *transfer_buffer;
  uint32_t transfer_buffer_phys;

  struct usb_hcd hcd;
};

// Pool partitioning: interrupt pipes use low indices, control xfers use high
#define OHCI_INT_ED_MAX 8
#define OHCI_INT_TD_MAX 16
#define OHCI_CTRL_ED_START 8
#define OHCI_CTRL_TD_START 16

// Interrupt Pipe (for persistent interrupt IN endpoints)
#define OHCI_MAX_INT_PIPES 8

struct ohci_int_pipe {
  bool active;
  struct ohci_controller *hc;
  struct ohci_ed *ed;
  uint32_t ed_phys;
  struct ohci_td *td[2]; // Ping-pong TDs
  uint32_t td_phys[2];
  int cur_idx; // Which TD is currently active (0 or 1)
  void *data_buf;
  uint32_t data_buf_phys;
  uint16_t max_packet;
};

// Set up an interrupt IN endpoint. Returns pipe handle or NULL.
struct ohci_int_pipe *ohci_setup_int_in(struct ohci_controller *hc,
                                        uint8_t dev_addr, uint8_t ep_num,
                                        uint16_t max_packet, uint8_t interval,
                                        bool low_speed, void *buffer,
                                        uint32_t buffer_phys);

// Returns true if the pipe's current TD has completed.
bool ohci_int_pipe_completed(struct ohci_int_pipe *pipe);

// Resubmit the pipe for the next transfer.
void ohci_int_pipe_resubmit(struct ohci_int_pipe *pipe);

// Public API
void ohci_init(void);
int ohci_get_controller_count(void);
struct ohci_controller *ohci_get_controller(int index);

#endif
