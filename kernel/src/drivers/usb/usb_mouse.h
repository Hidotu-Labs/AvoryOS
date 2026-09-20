#ifndef DRIVERS_USB_USB_MOUSE_H
#define DRIVERS_USB_USB_MOUSE_H

#include "usb.h"
#include <stdbool.h>
#include <stdint.h>

// USB HID Mouse Driver
//
// Parses each interface's HID report descriptor and decodes report-protocol
// or boot-protocol mouse reports (report IDs, 12/16-bit axes, wheel/pan,
// buttons) into the kernel input subsystem (mouse state + evdev).

// Try to attach a USB mouse driver to this device.
// Returns true if the device is a mouse and was successfully initialized.
bool usb_mouse_probe(struct usb_device *dev);
void usb_mouse_disconnect(struct usb_device *dev);

// Called periodically to poll the mouse for new data.
// Invoked from the HCD IRQ handlers and the xHCI watchdog.
void usb_mouse_poll(void);

#endif

// Diagnostics (/proc/usb)
struct usb_diag;
void usb_mouse_diag(struct usb_diag *d);
