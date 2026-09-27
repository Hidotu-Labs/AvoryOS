#ifndef DRIVERS_USB_UHCI_H
#define DRIVERS_USB_UHCI_H

/*
 * UHCI (Universal Host Controller Interface) — USB 1.x
 *
 * Intel's USB 1.0/1.1 host controller specification.
 * Uses I/O-space registers and a Frame List in physical memory to schedule
 * isochronous, interrupt, control, and bulk transfers at up to 12 Mbit/s.
 *
 * Reference: Intel UHCI Design Guide, Revision 1.1 (March 1996)
 */

#include "usb.h"
#include <stdbool.h>
#include <stdint.h>

// UHCI I/O-space Register Offsets
#define UHCI_REG_USBCMD 0x00    // USB Command              (16-bit R/W)
#define UHCI_REG_USBSTS 0x02    // USB Status               (16-bit R/WC)
#define UHCI_REG_USBINTR 0x04   // USB Interrupt Enable     (16-bit R/W)
#define UHCI_REG_FRNUM 0x06     // Frame Number             (16-bit R/W)
#define UHCI_REG_FLBASEADD 0x08 // Frame List Base Address  (32-bit R/W)
#define UHCI_REG_SOFMOD 0x0C    // Start of Frame Modify    (8-bit  R/W)
#define UHCI_REG_PORTSC1 0x10   // Port 1 Status/Control    (16-bit R/WC)
#define UHCI_REG_PORTSC2 0x12   // Port 2 Status/Control    (16-bit R/WC)

// USBCMD bits
#define UHCI_CMD_RS (1 << 0)      // Run/Stop
#define UHCI_CMD_HCRESET (1 << 1) // Host Controller Reset
#define UHCI_CMD_GRESET (1 << 2)  // Global Reset
#define UHCI_CMD_EGSM (1 << 3)    // Enter Global Suspend Mode
#define UHCI_CMD_FGR (1 << 4)     // Force Global Resume
#define UHCI_CMD_SWDBG (1 << 5)   // Software Debug
#define UHCI_CMD_CF (1 << 6)      // Configure Flag
#define UHCI_CMD_MAXP (1 << 7)    // Max Packet (1=64 bytes, 0=32 bytes)

// USBSTS bits
#define UHCI_STS_USBINT (1 << 0) // USB Interrupt (IOC)
#define UHCI_STS_ERROR (1 << 1)  // USB Error Interrupt
#define UHCI_STS_RD (1 << 2)     // Resume Detect
#define UHCI_STS_HSE (1 << 3)    // Host System Error
#define UHCI_STS_HCPE (1 << 4)   // Host Controller Process Error
#define UHCI_STS_HCH (1 << 5)    // HC Halted

// USBINTR bits
#define UHCI_INTR_TIMEOUT (1 << 0) // Timeout/CRC Interrupt Enable
#define UHCI_INTR_RESUME (1 << 1)  // Resume Interrupt Enable
#define UHCI_INTR_IOC (1 << 2)     // Interrupt on Complete Enable
#define UHCI_INTR_SP (1 << 3)      // Short Packet Interrupt Enable

// PORTSC bits
#define UHCI_PORT_CCS (1 << 0)   // Current Connect Status
#define UHCI_PORT_CSC (1 << 1)   // Connect Status Change (W1C)
#define UHCI_PORT_PE (1 << 2)    // Port Enabled
#define UHCI_PORT_PEC (1 << 3)   // Port Enable Change (W1C)
#define UHCI_PORT_LSA (1 << 4)   // Line Status D+ (bit 4)
#define UHCI_PORT_LSB (1 << 5)   // Line Status D- (bit 5)
#define UHCI_PORT_RD (1 << 6)    // Resume Detect
#define UHCI_PORT_LSDA (1 << 8)  // Low-Speed Device Attached
#define UHCI_PORT_PR (1 << 9)    // Port Reset
#define UHCI_PORT_SUSP (1 << 12) // Suspend

// UHCI Transfer Descriptor (TD)
// Must be 16-byte aligned.
struct uhci_td {
  volatile uint32_t link;
  volatile uint32_t status;
  volatile uint32_t token;
  volatile uint32_t buffer;
} __attribute__((packed, aligned(16)));

#define TD_LINK_TERMINATE (1 << 0)
#define TD_LINK_QH (1 << 1)
#define TD_LINK_VF (1 << 2)

#define TD_STATUS_BITSTUFF (1 << 17)
#define TD_STATUS_TIMEOUT (1 << 18)
#define TD_STATUS_NAK (1 << 19)
#define TD_STATUS_BABBLE (1 << 20)
#define TD_STATUS_DBUFFER (1 << 21)
#define TD_STATUS_HALTED (1 << 22)
#define TD_STATUS_ACTIVE (1 << 23)
#define TD_STATUS_IOC (1 << 24)
#define TD_STATUS_IOS (1 << 25)
#define TD_STATUS_LS (1 << 26)
#define TD_STATUS_C_ERR (3 << 27)
#define TD_STATUS_SPD (1 << 29)

#define TD_PID_SETUP 0x2D
#define TD_PID_IN 0x69
#define TD_PID_OUT 0xE1

// UHCI Queue Head (QH)
// Must be 16-byte aligned.
struct uhci_qh {
  volatile uint32_t head;
  volatile uint32_t element;
} __attribute__((packed, aligned(16)));

#define QH_LINK_TERMINATE (1 << 0)
#define QH_LINK_QH (1 << 1)

// PCI class/subclass/progif for UHCI
#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03
#define PCI_PROGIF_UHCI 0x00
#define PCI_PROGIF_OHCI 0x10

// UHCI controller state
struct uhci_controller {
  uint16_t io_base;   // I/O-space base address
  uint8_t irq_line;   // PCI interrupt line
  uint8_t pci_bus;    // PCI bus number
  uint8_t pci_slot;   // PCI slot number
  uint8_t pci_func;   // PCI function number
  uint16_t vendor_id; // PCI vendor ID
  uint16_t device_id; // PCI device ID
  uint8_t num_ports;  // Number of root hub ports (usually 2)
  bool present;       // Controller discovered and initialized


  uint32_t *frame_list;     // Virtual address (1024 * 4 bytes)
  uint32_t frame_list_phys; // Physical address (4KB aligned)
  bool irq_registered;
  uint64_t interrupts;


  struct uhci_td *td_pool;
  uint32_t td_pool_phys;
  struct uhci_qh *qh_pool;
  uint32_t qh_pool_phys;

  // DMA buffer for control requests and small data transfers
  void *transfer_buffer;
  uint32_t transfer_buffer_phys;

  struct usb_hcd hcd; // Generic host controller interface
};

struct usb_control_request;
int uhci_control_transfer(struct uhci_controller *hc, uint8_t addr,
                          struct usb_control_request *req, void *data,
                          uint16_t len, bool low_speed);

#define UHCI_MAX_CONTROLLERS 8

// Public API

// Probe PCI for UHCI controllers and map their I/O resources.
// Must be called after pci_init().
void uhci_init(void);

// Returns the number of discovered UHCI controllers.
int uhci_get_controller_count(void);

// Returns a pointer to a controller by index, or NULL.
struct uhci_controller *uhci_get_controller(int index);

// Self-test: verifies PCI discovery, register access, and prints diagnostics.
void uhci_self_test(void);

#endif
