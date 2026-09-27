#ifndef DRIVERS_USB_USB_KBD_H
#define DRIVERS_USB_USB_KBD_H

#include "usb.h"
#include <stdbool.h>
#include <stdint.h>

// USB HID Keyboard Driver
// Handles USB Boot Protocol keyboards via UHCI Interrupt Transfers.
// Translates the 8-byte HID Boot Keyboard report into the kernel's
// existing keyboard input subsystem (ring buffer + evdev events).

// USB HID Boot Protocol keyboard report (8 bytes)
struct usb_kbd_report {
  uint8_t modifiers;   // Bit field: L_CTRL, L_SHIFT, L_ALT, L_GUI,
                       //            R_CTRL, R_SHIFT, R_ALT, R_GUI
  uint8_t reserved;    // Always 0
  uint8_t keys[6];     // Up to 6 simultaneous key presses (HID usage codes)
} __attribute__((packed));

// Modifier bits
#define USB_KBD_MOD_LCTRL  (1 << 0)
#define USB_KBD_MOD_LSHIFT (1 << 1)
#define USB_KBD_MOD_LALT   (1 << 2)
#define USB_KBD_MOD_LGUI   (1 << 3)
#define USB_KBD_MOD_RCTRL  (1 << 4)
#define USB_KBD_MOD_RSHIFT (1 << 5)
#define USB_KBD_MOD_RALT   (1 << 6)
#define USB_KBD_MOD_RGUI   (1 << 7)

// Public API

// Try to attach a USB keyboard driver to this device.
// Returns true if the device is a keyboard and was successfully initialized.
bool usb_kbd_probe(struct usb_device *dev);
void usb_kbd_disconnect(struct usb_device *dev);

// Called periodically to poll the keyboard for new data.

void usb_kbd_poll(void);
uint32_t usb_kbd_active_count(void);
const char *usb_kbd_report_format(void);

#endif
