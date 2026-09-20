#ifndef DRIVERS_USB_XHCI_H
#define DRIVERS_USB_XHCI_H

#include "../pci/pci.h"
#include "usb.h"
#include "../../lock/spinlock.h"
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
  uint32_t pipe_errors;
  /* Verbose per-pipe tracing counters, reported at /proc/usb. */
  uint32_t submitted;      // Normal TRBs placed on the ring
  uint32_t doorbell_rings; // Doorbell writes for this endpoint
  uint32_t resubmits;      // Resubmissions after a completion
  uint32_t completions_ok;    // Completion code 1 (Success)
  uint32_t completions_short; // Completion code 13 (Short Packet)
  uint32_t errors;            // Any other completion code
  uint32_t logged_events;     // Transfer events logged so far
  uint32_t orphan_events;     // Transfer events with no matching TRB
  uint32_t last_residual;     // Residual bytes of the last transfer event
  uint32_t last_length;       // buffer_len - residual of the last event
  uint32_t last_event_status; // Raw status dword of the last event
  uint32_t last_event_control;// Raw control dword of the last event
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
  /* Bitmap of ports whose Supported Protocol capability says USB3.  A port's
   * speed field reads 0 until a SuperSpeed link trains, so the reset type
   * (warm reset vs bus reset) must come from the protocol, not the speed. */
  uint8_t usb3_ports[32];
  struct usb_hcd hcd;
  struct xhci_slot slots[256];
  struct xhci_interrupt_state interrupt_pipes[XHCI_MAX_INTERRUPT_PIPES];
  uint32_t pending_ports;
  uint32_t connected_ports;
  /* Bounded late-enumeration attempts for ports that only report their
   * attached device some time after the host reset (real controllers can
   * take hundreds of milliseconds; QEMU reports the connection instantly). */
  uint16_t late_rescan_attempts;

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
  /* Event-ring access is shared by the ISR, the timer watchdog and control
   * transfer waiters.  Without this lock two drainers can consume the same
   * event and skip the next one, which loses control-transfer completions
   * (random enumeration failures) and interrupt reports. */
  spinlock_t event_lock;
  /* Command-ring submission is shared by thread context (probes, HID LED
   * reports) and IRQ context (silent-endpoint recovery and the GET_REPORT
   * fallback).  The ring cursor and the completion wait are not reentrant. */
  spinlock_t command_lock;
  /* EP0 transfers share one control buffer and one TRB ring per slot.  A
   * thread-context control transfer can overlap the IRQ-driven GET_REPORT
   * fallback, so the whole transfer is serialized per controller. */
  spinlock_t control_lock;
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
  /* Transfer-event tracing.  A configured-but-silent interrupt endpoint leaves
   * no other trace, so every transfer event (matched or orphaned) is counted
   * and the last one is kept for /proc/usb. */
  uint64_t transfer_events;
  uint64_t orphan_transfer_events;
  uint64_t irq_count;
  uint32_t last_transfer_event_status;
  uint32_t last_transfer_event_control;
  uint64_t last_transfer_event_parameter;
  /* EP0 transfer events are tracked separately: the shared last_transfer_*
   * fields are overwritten by interrupt transfers on other endpoints, which
   * made control transfers time out whenever a keyboard/mouse was active. */
  volatile uint64_t last_ep0_event_trb;
  volatile uint8_t last_ep0_event_code;
  uint32_t ep0_events_logged;
  uint32_t transfer_events_logged;
  uint32_t debug_ep0_logs;
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
void xhci_diag(struct usb_diag *d);
/* Per-pipe state for a generic (xHCI) interrupt pipe: endpoint state from the
 * output device context, submit/doorbell/completion counters and the last
 * transfer event.  Used by /proc/usb_mouse so one short read is decisive. */
void xhci_interrupt_pipe_diag(struct usb_diag *d,
                              struct usb_interrupt_pipe *pipe);
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
