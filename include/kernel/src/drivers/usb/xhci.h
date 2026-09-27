#ifndef DRIVERS_USB_XHCI_H
#define DRIVERS_USB_XHCI_H

#include "../pci/pci.h"
#include "usb.h"
#include <stdbool.h>
#include <stdint.h>

#define XHCI_MAX_CONTROLLERS 4
#define XHCI_RING_TRBS 256

struct xhci_trb {
  uint64_t parameter;
  uint32_t status;
  uint32_t control;
} __attribute__((packed, aligned(16)));

struct xhci_erst_entry {
  uint64_t base;
  uint32_t size;
  uint32_t reserved;
} __attribute__((packed));

struct xhci_slot {
  bool enabled;
  uint8_t id;
  uint8_t port;
  enum usb_speed speed;
  uint16_t ep0_max_packet;
  void *output_context;
  uint64_t output_context_phys;
  void *input_context;
  uint64_t input_context_phys;
  struct xhci_trb *ep0_ring;
  uint64_t ep0_ring_phys;
  uint16_t ep0_enqueue;
  uint8_t ep0_cycle;
  void *control_buffer;
  uint64_t control_buffer_phys;
};

#define XHCI_MAX_INTERRUPT_PIPES 16
struct xhci_interrupt_state {
  struct usb_interrupt_pipe pipe;
  struct xhci_trb *ring;
  uint64_t ring_phys;
  uint16_t enqueue;
  uint8_t cycle;
  uint8_t endpoint_id;
  uint64_t expected_trb;
  uint8_t completion_code;
  bool completed;
  uint64_t completions;
};

struct xhci_controller {
  struct pci_device *pci;
  volatile uint8_t *cap;
  volatile uint8_t *op;
  volatile uint8_t *runtime;
  volatile uint32_t *doorbells;
  uint64_t mmio_phys;
  uint16_t version;
  uint8_t cap_length;
  uint8_t max_slots;
  uint8_t max_ports;
  uint16_t max_interrupters;
  uint16_t scratchpad_count;
  bool context_64;
  bool ac64;
  bool running;
  struct usb_hcd hcd;
  struct xhci_slot slots[256];
  struct xhci_interrupt_state interrupt_pipes[XHCI_MAX_INTERRUPT_PIPES];
  uint32_t pending_ports;
  uint32_t connected_ports;

  void *dcbaa;
  uint64_t dcbaa_phys;
  void *scratchpad_array;
  uint64_t scratchpad_array_phys;
  struct xhci_trb *command_ring;
  uint64_t command_ring_phys;
  uint16_t command_enqueue;
  uint8_t command_cycle;

  struct xhci_trb *event_ring;
  uint64_t event_ring_phys;
  uint16_t event_dequeue;
  uint8_t event_cycle;
  struct xhci_erst_entry *erst;
  uint64_t erst_phys;

  struct pci_msix msix;
  struct pci_msi msi;
  uint8_t irq_vector;
  bool msix_enabled;
  bool msi_enabled;
  bool irq_reported;
  uint64_t commands_submitted;
  uint64_t commands_completed;
  uint64_t events_seen;
  uint64_t ring_wraps;
  uint64_t interrupts;
  uint64_t timeouts;
  volatile uint64_t last_command_trb;
  volatile uint8_t last_completion_code;
  volatile uint8_t last_command_slot;
  volatile uint64_t last_transfer_trb;
  volatile uint8_t last_transfer_code;
  const char *debug_stage;
  uint32_t debug_usbcmd;
  uint32_t debug_usbsts;
  uint32_t debug_portsc[256];
  uint8_t debug_port_result[256];
  uint8_t debug_last_port;
  uint8_t debug_last_slot;
  uint8_t debug_last_command_type;
  uint8_t debug_last_ep0_request;
  uint16_t debug_last_ep0_value;
  uint16_t debug_last_ep0_length;
  uint8_t debug_ep0_data[8];
};

void xhci_init(void);
void xhci_msix_watchdog(void);
int xhci_get_controller_count(void);
int xhci_get_matched_count(void);
const char *xhci_get_last_probe_failure(void);
const char *xhci_get_last_dma_object(void);
bool xhci_get_last_ac64(void);
const char *xhci_get_last_dma_layer(void);
uint32_t xhci_get_last_dma_flags(void);
uint64_t xhci_get_last_dma_phys(void);
struct xhci_controller *xhci_get_controller(int index);
bool xhci_phase2_stress(uint32_t reset_cycles, uint32_t command_count);

#endif
