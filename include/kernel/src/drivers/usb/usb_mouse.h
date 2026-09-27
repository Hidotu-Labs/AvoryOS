#ifndef DRIVERS_USB_USB_MOUSE_H
#define DRIVERS_USB_USB_MOUSE_H

#include "usb.h"
#include <stdbool.h>
#include <stdint.h>

// USB HID Mouse Driver
// Handles USB Boot Protocol mice via UHCI Interrupt Transfers.
// Translates the 3-byte (or 4-byte) HID Boot Mouse report into the kernel's
// existing mouse input subsystem (mouse_state_t + evdev events).

// USB HID Boot Protocol mouse report (3+ bytes)
struct usb_mouse_report {
  uint8_t buttons;   // Bit 0: Left, Bit 1: Right, Bit 2: Middle
  int8_t x;          // X displacement (signed)
  int8_t y;          // Y displacement (signed)
  int8_t wheel;      // Scroll wheel (optional, 4th byte)
} __attribute__((packed));

// Button bits
#define USB_MOUSE_BTN_LEFT   (1 << 0)
#define USB_MOUSE_BTN_RIGHT  (1 << 1)
#define USB_MOUSE_BTN_MIDDLE (1 << 2)

// Public API

// Try to attach a USB mouse driver to this device.
// Returns true if the device is a mouse and was successfully initialized.
bool usb_mouse_probe(struct usb_device *dev);
void usb_mouse_disconnect(struct usb_device *dev);

// Called periodically to poll the mouse for new data.
// Invoked from the UHCI IRQ handler on IOC completion.
void usb_mouse_poll(void);

#endif
