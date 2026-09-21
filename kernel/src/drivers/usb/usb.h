#ifndef DRIVERS_USB_USB_H
#define DRIVERS_USB_USB_H

#include "usb_debug.h"
#include <stdbool.h>
#include <stdint.h>

enum usb_speed {
  USB_SPEED_UNKNOWN = 0,
  USB_SPEED_LOW,
  USB_SPEED_FULL,
  USB_SPEED_HIGH,
  USB_SPEED_SUPER,
  USB_SPEED_SUPER_PLUS,
};

struct usb_device;
struct usb_interrupt_pipe;

struct usb_hcd_stats {
  uint64_t control_submitted;
  uint64_t control_completed;
  uint64_t control_failed;
  uint64_t devices_connected;
  uint64_t devices_removed;
  uint64_t interrupt_completed;
  uint64_t cancellations;
};

struct usb_interrupt_pipe {
  struct usb_device *dev;
  void *hcd_data;
  void *buffer;
  uint64_t buffer_phys;
  uint16_t buffer_len;
  uint16_t actual_length; // Bytes received by the most recent transfer.
  uint8_t endpoint;
  bool active;

  /* Runtime counters kept by the HCD and the generic wrappers.  A silent
   * mouse has no report to show, so the only evidence that anything happened
   * is how often a transfer completed, failed, or returned zero bytes.  These
   * are reported at /proc/usb. */
  uint32_t completions;      // Successful or short completions observed
  uint32_t errors;           // Error completions observed
  uint32_t zero_length;      // Completions that carried no payload
  uint8_t last_completion;   // HCD completion code of the most recent transfer
  uint16_t last_length;      // Actual length of the most recent transfer
};

// USB Request Types
#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09

struct usb_control_request {
  uint8_t request_type;
  uint8_t request;
  uint16_t value;
  uint16_t index;
  uint16_t length;
} __attribute__((packed));

// Descriptor Types
#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIGURATION 0x02
#define USB_DESC_STRING 0x03
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05

// Device Descriptor
struct usb_device_descriptor {
  uint8_t length;
  uint8_t type;
  uint16_t usb_version;
  uint8_t device_class;
  uint8_t device_subclass;
  uint8_t device_protocol;
  uint8_t max_packet_size;
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t device_version;
  uint8_t manufacturer_string_idx;
  uint8_t product_string_idx;
  uint8_t serial_number_idx;
  uint8_t num_configurations;
} __attribute__((packed));

// Host Controller Interface
struct usb_hcd {
  void *priv; // Pointer to controller-specific state (e.g. uhci_controller)
  const char *name;
  int (*control_transfer)(struct usb_hcd *hcd, uint8_t addr,
                          struct usb_control_request *req, void *data,
                          uint16_t len, enum usb_speed speed);
  int (*control_device)(struct usb_hcd *hcd, struct usb_device *dev,
                        struct usb_control_request *req, void *data,
                        uint16_t len);
  int (*device_prepare)(struct usb_hcd *hcd, struct usb_device *dev);
  // On success the HCD stores the controller-assigned address in dev->address.
  int (*address_device)(struct usb_hcd *hcd, struct usb_device *dev,
                        uint8_t requested_address);
  void (*device_removed)(struct usb_hcd *hcd, struct usb_device *dev);
  struct usb_interrupt_pipe *(*interrupt_open)(
      struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
      uint16_t max_packet, uint8_t interval, void *buffer,
      uint64_t buffer_phys);
  bool (*interrupt_completed)(struct usb_hcd *hcd,
                              struct usb_interrupt_pipe *pipe);
  int (*interrupt_resubmit)(struct usb_hcd *hcd,
                            struct usb_interrupt_pipe *pipe);
  void (*interrupt_cancel)(struct usb_hcd *hcd,
                           struct usb_interrupt_pipe *pipe);
  struct usb_hcd_stats stats;
};

// Device Structure
struct usb_device {
  uint8_t address;
  uint8_t port;
  bool connected;
  enum usb_speed speed;
  uint32_t generation;
  uint8_t configuration_value;
  bool configured;
  void *hcd_data;
  struct usb_device_descriptor desc;
  struct usb_hcd *hcd; // Reference to the host controller driver
  uint8_t claimed_by;  // 0=none, bit0=keyboard, bit1=mouse, bit2=hub
  /* Enumeration retry.  A device that fails its first descriptor read used to
   * stay dead until it was unplugged; real controllers can NAK/timeout once
   * during link training.  The watchdog retries a bounded number of times. */
  uint8_t enumerate_attempts;
  bool enumerate_failed;
};

// Diagnostics
//
// Drivers append human-readable state into a caller-provided buffer; the
// result is served at /proc/usb.  This is the only reliable way to inspect
// USB bring-up on machines without a serial capture, so each driver reports
// its probe decisions, not just its successes.
struct usb_diag {
  char *data;
  uint32_t cap;
  uint32_t len;
};

void usb_diag_printf(struct usb_diag *d, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
uint32_t usb_diag_read(uint32_t offset, uint32_t size, uint8_t *buffer);

/* Verbose bring-up tracing shared by the USB core and the HCDs.  Logs one
 * compact line ("tag len=N bytes=00112233 ...") so raw descriptors, report
 * buffers and xHCI transfer payloads can be compared on real hardware without
 * a debugger.  Only emits when usb_verbose is on. */
void usb_debug_hexdump(const char *tag, const void *data, uint32_t len,
                       uint32_t max_bytes);

/* Runtime verbose-trace switch (/proc/usb_debug, `usbdebug` cmdline). */
void usb_set_verbose(bool on);
bool usb_get_verbose(void);

// Per-driver diagnostics (implemented next to the state they report).
void xhci_diag(struct usb_diag *d);
void ehci_diag(struct usb_diag *d);
void usb_core_diag(struct usb_diag *d);
void usb_kbd_diag(struct usb_diag *d);
void usb_mouse_diag(struct usb_diag *d);

/* Short per-subsystem views of the same diagnostics, readable as
 * /proc/usb_mouse and /proc/usb_xhci.  A full /proc/usb is long; these make
 * one `cat` decisive on a console without scrollback. */
uint32_t usb_mouse_diag_read(uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t usb_xhci_diag_read(uint32_t offset, uint32_t size, uint8_t *buffer);

// Core API

void usb_init(void);
void usb_enumerate_device(struct usb_device *dev);
const char *usb_speed_name(enum usb_speed speed);
bool usb_speed_is_low(enum usb_speed speed);
int usb_control_transfer(struct usb_device *dev,
                         struct usb_control_request *req, void *data,
                         uint16_t len);

/* Fetch an interface's HID report descriptor into buf (zero-padded on a short
 * transfer).  Returns 0 on success, -1 when the request failed. */
int usb_hid_get_report_descriptor(struct usb_device *dev, uint8_t iface_num,
                                  uint8_t *buf, uint16_t len);

/* True when a HID interface's report descriptor declares a top-level usage on
 * the Generic Desktop page (0x06 keyboard, 0x02 mouse, ...).  Many real
 * keyboards/mice report subclass 0 / protocol 0 and would otherwise be
 * ignored by the boot-protocol-only matching in the HID drivers. */
bool usb_hid_usage_matches(struct usb_device *dev, uint8_t iface_num,
                           uint8_t usage);
struct usb_interrupt_pipe *usb_interrupt_open(
    struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
    uint8_t interval, void *buffer, uint64_t buffer_phys);
bool usb_interrupt_completed(struct usb_interrupt_pipe *pipe);
int usb_interrupt_resubmit(struct usb_interrupt_pipe *pipe);
void usb_interrupt_cancel(struct usb_interrupt_pipe *pipe);

// Called by HC driver when a new device is detected on a root hub port
void usb_device_discovered(struct usb_hcd *hcd, uint8_t port,
                           enum usb_speed speed);
void usb_device_removed(struct usb_hcd *hcd, uint8_t port);

/* Retry devices whose enumeration failed (bounded per device).  Called from
 * the xHCI watchdog so a one-off descriptor timeout does not leave a device
 * permanently invisible. */
void usb_retry_failed_enumerations(void);

#endif
