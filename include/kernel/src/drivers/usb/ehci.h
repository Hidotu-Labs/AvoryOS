#ifndef DRIVERS_USB_EHCI_H
#define DRIVERS_USB_EHCI_H

/*
 * EHCI (Enhanced Host Controller Interface) — USB 2.0
 *
 * Provides High-Speed (480 Mbit/s) support. EHCI controllers often have
 * companion controllers (UHCI/OHCI) to handle Low/Full speed devices.
 */

#include "usb.h"
#include <stdbool.h>
#include <stdint.h>

// EHCI Capability Registers (MMIO BAR0)
#define EHCI_CAP_CAPLENGTH 0x00      // Capability Register Length (8-bit)
#define EHCI_CAP_HCIVERSION 0x02     // Interface Version Number (16-bit)
#define EHCI_CAP_HCSPARAMS 0x04      // Structural Parameters (32-bit)
#define EHCI_CAP_HCCPARAMS 0x08      // Capability Parameters (32-bit)
#define EHCI_CAP_HCSP_PORTROUTE 0x0C // Companion Port Route Description

// EHCI Operational Registers (Base + CAPLENGTH)
#define EHCI_REG_USBCMD 0x00           // USB Command
#define EHCI_REG_USBSTS 0x04           // USB Status
#define EHCI_REG_USBINTR 0x08          // USB Interrupt Enable
#define EHCI_REG_FRINDEX 0x0C          // Frame Index
#define EHCI_REG_CTRLDSSEGMENT 0x10    // 4G Segment Selector
#define EHCI_REG_PERIODICLISTBASE 0x14 // Periodic Frame List Base Address
#define EHCI_REG_ASYNCLISTADDR 0x18    // Current Asynchronous List Address
#define EHCI_REG_CONFIGFLAG 0x40       // Configured Flag Register
#define EHCI_REG_PORTSC 0x44           // Port Status/Control (starts here)

// PORTSC bits
#define EHCI_PORT_CONNECT (1 << 0)
#define EHCI_PORT_EN_CHANGE (1 << 1)
#define EHCI_PORT_ENABLE (1 << 2)
#define EHCI_PORT_RESET (1 << 8)
#define EHCI_PORT_OWNER (1 << 13)

// USBCMD bits
#define EHCI_CMD_RS (1 << 0)      // Run/Stop
#define EHCI_CMD_HCRESET (1 << 1) // Host Controller Reset
#define EHCI_CMD_FLSIZE (3 << 2)  // Frame List Size
#define EHCI_CMD_PSE (1 << 4)     // Periodic Schedule Enable
#define EHCI_CMD_ASE (1 << 5)     // Asynchronous Schedule Enable
#define EHCI_CMD_IAAD (1 << 6)    // Interrupt on Async Advance Doorbell

// USBSTS bits
#define EHCI_STS_USBINT (1 << 0)  // USB Interrupt (IOC)
#define EHCI_STS_ERROR (1 << 1)   // USB Error Interrupt
#define EHCI_STS_PCD (1 << 2)     // Port Change Detect
#define EHCI_STS_FLR (1 << 3)     // Frame List Rollover
#define EHCI_STS_HSE (1 << 4)     // Host System Error
#define EHCI_STS_IAA (1 << 5)     // Interrupt on Async Advance
#define EHCI_STS_HALTED (1 << 12) // HC Halted
#define EHCI_STS_RECL (1 << 13)   // Reclamation
#define EHCI_STS_PSS (1 << 14)    // Periodic Schedule Status
#define EHCI_STS_ASS (1 << 15)    // Asynchronous Schedule Status

// USBINTR bits
#define EHCI_INTR_USBINT (1 << 0) // USB Interrupt Enable
#define EHCI_INTR_ERROR (1 << 1)  // USB Error Interrupt Enable
#define EHCI_INTR_PCD (1 << 2)    // Port Change Detect Enable
#define EHCI_INTR_FLR (1 << 3)    // Frame List Rollover Enable
#define EHCI_INTR_HSE (1 << 4)    // Host System Error Enable
#define EHCI_INTR_IAA (1 << 5)    // Interrupt on Async Advance Enable

// HCCPARAMS bits
#define EHCI_HCC_64BIT (1 << 0)   // 64-bit addressing capability
#define EHCI_HCC_PFL (1 << 1)     // Programmable Frame List flag
#define EHCI_HCC_ASPC (1 << 2)    // Async Schedule Park Capability
#define EHCI_HCC_EECP (0xFF << 8) // EHCI Extended Capabilities Pointer

// EHCI Queue Element Transfer Descriptor (qTD)
// Must be 32-byte aligned.
struct ehci_qtd {
  volatile uint32_t next;      // Next qTD pointer
  volatile uint32_t alt_next;  // Alternate next qTD pointer
  volatile uint32_t token;     // Status, PID, Length, Toggle, etc.
  volatile uint32_t buffer[5]; // Buffer pointers (page-aligned)
} __attribute__((packed));

#define QTD_TOKEN_ACTIVE (1 << 7)
#define QTD_TOKEN_HALTED (1 << 6)
#define QTD_TOKEN_BUFF_ERR (1 << 5)
#define QTD_TOKEN_BABBLE (1 << 4)
#define QTD_TOKEN_XACT_ERR (1 << 3)
#define QTD_TOKEN_MISSED (1 << 2)
#define QTD_TOKEN_SPLIT (1 << 1)
#define QTD_TOKEN_PING (1 << 0)

#define QTD_PID_OUT 0
#define QTD_PID_IN 1
#define QTD_PID_SETUP 2

// EHCI Queue Head (QH)
// Must be 32-byte aligned.
struct ehci_qh {
  volatile uint32_t link;    // Horizontal link pointer
  volatile uint32_t ep_char; // Endpoint characteristics
  volatile uint32_t ep_caps; // Endpoint capabilities
  volatile uint32_t current; // Current qTD pointer
  struct ehci_qtd overlay;   // Overlay area (32 bytes)

  // Padding to make QH exactly 64 bytes if needed, though EHCI qTD is 32.
  // 16 (header) + 32 (overlay) = 48 bytes.
  // QH should be 64-byte aligned and typically 64 bytes in size.
  uint32_t reserved[4];
} __attribute__((packed, aligned(32)));

#define QH_TYPE_ITD (0 << 1)
#define QH_TYPE_QH (1 << 1)
#define QH_TYPE_SITD (2 << 1)
#define QH_TYPE_FSTN (3 << 1)

#define QH_EP_SPEED_FULL (0 << 12)
#define QH_EP_SPEED_LOW (1 << 12)
#define QH_EP_SPEED_HIGH (2 << 12)

// EHCI Extended Capabilities Pointer (EECP)
#define EHCI_EECP_ID_LEGACY 0x01
#define EHCI_LEGACY_BIOS_OWNED (1 << 16)
#define EHCI_LEGACY_OS_OWNED (1 << 24)

// Linkage Pointers
#define EHCI_PTR_TERMINATE (1 << 0)
#define EHCI_PTR_ITD (0 << 1)
#define EHCI_PTR_QH (1 << 1)
#define EHCI_PTR_SITD (2 << 1)
#define EHCI_PTR_FSTN (3 << 1)

// Periodic Schedule Constants
#define EHCI_PERIODIC_FRAME_COUNT 1024 // Standard frame list size

// EHCI Interrupt Pipe (for persistent interrupt IN endpoints)
// Parallels the OHCI interrupt pipe architecture for HID device support.
#define EHCI_MAX_INT_PIPES 8

struct ehci_int_pipe {
  bool active;
  struct ehci_controller *hc;
  struct ehci_qh *qh; // The QH in the periodic schedule
  uint32_t qh_phys;
  struct ehci_qtd *qtd[2]; // Ping-pong qTDs
  uint32_t qtd_phys[2];
  int cur_idx;    // Which qTD is currently active (0 or 1)
  void *data_buf; // DMA buffer for received data
  uint32_t data_buf_phys;
  uint16_t max_packet;
  uint8_t interval; // Polling interval (in microframes, 125us units)
};

// EHCI controller state
struct ehci_controller {
  uintptr_t cap_base; // Capability registers base (BAR0)
  uintptr_t op_base;  // Operational registers base (BAR0 + CAPLENGTH)
  uint8_t irq_line;
  uint8_t pci_bus;
  uint8_t pci_slot;
  uint8_t pci_func;
  uint16_t vendor_id;
  uint16_t device_id;
  uint8_t num_ports;
  bool present;
  bool irq_registered;
  uint64_t interrupts;

  // DMA Resources
  struct ehci_qh *async_qh; // Base QH for asynchronous schedule
  uint32_t async_qh_phys;

  // Periodic Schedule
  uint32_t *periodic_list; // 4KB-aligned array of 1024 frame pointers
  uint32_t periodic_list_phys;

  // Interrupt pipe QH/qTD pool (dedicated DMA page)
  struct ehci_qh *int_qh_pool; // Pool of QHs for interrupt pipes
  uint32_t int_qh_pool_phys;
  struct ehci_qtd *int_qtd_pool; // Pool of qTDs for interrupt pipes
  uint32_t int_qtd_pool_phys;

  // Pool for control transfers
  struct ehci_qh *qh_pool;
  uint32_t qh_pool_phys;
  struct ehci_qtd *qtd_pool;
  uint32_t qtd_pool_phys;
  void *transfer_buffer;
  uint32_t transfer_buffer_phys;

  struct usb_hcd hcd;
};

#define EHCI_MAX_CONTROLLERS 4

// Public API

void ehci_init(void);
void ehci_hand_to_companion(void);

// Controller accessors (for HID drivers to identify EHCI-backed devices)
int ehci_get_controller_count(void);
struct ehci_controller *ehci_get_controller(int index);

// Internal HCD interface
int ehci_control_transfer(struct ehci_controller *hc, uint8_t addr,
                          struct usb_control_request *req, void *data,
                          uint16_t len, bool low_speed);


struct ehci_int_pipe *ehci_setup_int_in(struct ehci_controller *hc,
                                        uint8_t dev_addr, uint8_t ep_num,
                                        uint16_t max_packet, uint8_t interval,
                                        bool low_speed, void *buffer,
                                        uint32_t buffer_phys);
bool ehci_int_pipe_completed(struct ehci_int_pipe *pipe);
void ehci_int_pipe_resubmit(struct ehci_int_pipe *pipe);

#endif
