#ifndef DRIVERS_USB_USB_H
#define DRIVERS_USB_USB_H

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
};

// Core API

void usb_init(void);
void usb_enumerate_device(struct usb_device *dev);
const char *usb_speed_name(enum usb_speed speed);
bool usb_speed_is_low(enum usb_speed speed);
int usb_control_transfer(struct usb_device *dev,
                         struct usb_control_request *req, void *data,
                         uint16_t len);
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

#endif
