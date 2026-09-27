#include "usb.h"
#include "../../console/klog.h"
#include "../../io/io.h"
#include "../../mm/heap.h"
#include "uhci.h"
#include "usb_kbd.h"
#include "usb_mouse.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_USB_DEVICES 128
static struct usb_device *devices[MAX_USB_DEVICES];
static int device_count = 0;
static uint32_t next_generation = 1;

void usb_enumerate_device(struct usb_device *dev);

static bool usb_initialized = false;

void usb_init(void) {
  if (usb_initialized)
    return;
  usb_initialized = true;
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Subsystem initialized.\n");
  for (int i = 0; i < MAX_USB_DEVICES; i++)
    devices[i] = NULL;
}

const char *usb_speed_name(enum usb_speed speed) {
  switch (speed) {
  case USB_SPEED_LOW: return "Low-Speed";
  case USB_SPEED_FULL: return "Full-Speed";
  case USB_SPEED_HIGH: return "High-Speed";
  case USB_SPEED_SUPER: return "SuperSpeed";
  case USB_SPEED_SUPER_PLUS: return "SuperSpeedPlus";
  default: return "Unknown-Speed";
  }
}

bool usb_speed_is_low(enum usb_speed speed) { return speed == USB_SPEED_LOW; }

int usb_control_transfer(struct usb_device *dev,
                         struct usb_control_request *req, void *data,
                         uint16_t len) {
  if (!dev || !dev->hcd || !dev->connected ||
      (!dev->hcd->control_device && !dev->hcd->control_transfer))
    return -1;
  dev->hcd->stats.control_submitted++;
  int result;
  if (dev->hcd->control_device)
    result = dev->hcd->control_device(dev->hcd, dev, req, data, len);
  else
    result = dev->hcd->control_transfer(dev->hcd, dev->address, req, data,
                                        len, dev->speed);
  if (result < 0)
    dev->hcd->stats.control_failed++;
  else
    dev->hcd->stats.control_completed++;
  return result;
}

struct usb_interrupt_pipe *usb_interrupt_open(
    struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
    uint8_t interval, void *buffer, uint64_t buffer_phys) {
  if (!dev || !dev->connected || !dev->hcd || !dev->hcd->interrupt_open)
    return NULL;
  return dev->hcd->interrupt_open(dev->hcd, dev, endpoint, max_packet,
                                  interval, buffer, buffer_phys);
}

bool usb_interrupt_completed(struct usb_interrupt_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd ||
      !pipe->dev->hcd->interrupt_completed)
    return false;
  bool completed = pipe->dev->hcd->interrupt_completed(pipe->dev->hcd, pipe);
  if (completed)
    pipe->dev->hcd->stats.interrupt_completed++;
  return completed;
}

int usb_interrupt_resubmit(struct usb_interrupt_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd ||
      !pipe->dev->hcd->interrupt_resubmit)
    return -1;
  return pipe->dev->hcd->interrupt_resubmit(pipe->dev->hcd, pipe);
}

void usb_interrupt_cancel(struct usb_interrupt_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd)
    return;
  if (pipe->dev->hcd->interrupt_cancel)
    pipe->dev->hcd->interrupt_cancel(pipe->dev->hcd, pipe);
  pipe->active = false;
  pipe->dev->hcd->stats.cancellations++;
}

void usb_device_discovered(struct usb_hcd *hcd, uint8_t port,
                           enum usb_speed speed) {
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: New device detected on port ");
  klog_uint64(port + 1);
  klog_puts(" (");
  klog_puts(usb_speed_name(speed));
  klog_puts(")\n");

  if (device_count >= MAX_USB_DEVICES)
    return;

  struct usb_device *dev = kmalloc(sizeof(struct usb_device));
  if (!dev)
    return;

  dev->address = 0; // Not yet assigned
  dev->port = port;
  dev->connected = true;
  dev->speed = speed;
  dev->generation = next_generation++;
  dev->configuration_value = 0;
  dev->configured = false;
  dev->hcd_data = NULL;
  dev->hcd = hcd;
  hcd->stats.devices_connected++;

  devices[device_count++] = dev;

  if (hcd->device_prepare && hcd->device_prepare(hcd, dev) < 0) {
    dev->connected = false;
    klog_puts("[USB] Host controller failed to prepare device\n");
    return;
  }


  usb_enumerate_device(dev);
}

void usb_enumerate_device(struct usb_device *dev) {
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Enumerating device...\n");

  struct usb_control_request req;

  // 1. Get first 8 bytes of Device Descriptor to get max packet size
  req.request_type = 0x80; // IN
  req.request = USB_REQ_GET_DESCRIPTOR;
  req.value = (USB_DESC_DEVICE << 8);
  req.index = 0;
  req.length = 8;

  int res = usb_control_transfer(dev, &req, &dev->desc, 8);
  if (res < 0) {
    klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " USB: Failed to get device descriptor (8 bytes)\n");
    return;
  }

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Max Packet Size: ");
  klog_uint64(dev->desc.max_packet_size);
  klog_puts("\n");

  // 2. Set Address
  uint8_t new_addr = device_count; // Simple addressing for now
  req.request_type = 0x00;         // OUT
  req.request = USB_REQ_SET_ADDRESS;
  req.value = new_addr;
  req.index = 0;
  req.length = 0;

  if (dev->hcd->address_device) {
    res = dev->hcd->address_device(dev->hcd, dev, new_addr);
    if (res == 0 && dev->address == 0)
      res = -1;
  } else {
    res = usb_control_transfer(dev, &req, NULL, 0);
    if (res == 0)
      dev->address = new_addr;
  }
  if (res < 0) {
    klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " USB: Failed to set address\n");
    return;
  }

  for (int i = 0; i < 1000; i++)
    io_wait(); // Wait for address to settle

  // 3. Get Full Device Descriptor
  req.request_type = 0x80; // IN
  req.request = USB_REQ_GET_DESCRIPTOR;
  req.value = (USB_DESC_DEVICE << 8);
  req.index = 0;
  req.length = 18;

  res = usb_control_transfer(dev, &req, &dev->desc, 18);
  if (res < 0) {
    klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " USB: Failed to get full device descriptor\n");
    return;
  }

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Device Vendor: ");
  klog_hex32(dev->desc.vendor_id);
  klog_puts(" Product: ");
  klog_hex32(dev->desc.product_id);
  klog_puts("\n");


  bool keyboard = usb_kbd_probe(dev);
  bool mouse = usb_mouse_probe(dev);
  if (keyboard) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Keyboard driver attached\n");
  }
  if (mouse) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Mouse driver attached\n");
  }
}

void usb_device_removed(struct usb_hcd *hcd, uint8_t port) {
  for (int i = 0; i < device_count; i++) {
    struct usb_device *dev = devices[i];
    if (!dev || dev->hcd != hcd || dev->port != port || !dev->connected)
      continue;
    dev->connected = false;
    hcd->stats.devices_removed++;
    usb_kbd_disconnect(dev);
    usb_mouse_disconnect(dev);
    if (hcd->device_removed)
      hcd->device_removed(hcd, dev);
    klog_puts("[USB] Device removed from port ");
    klog_uint64(port + 1);
    klog_puts("\n");
    return;
  }
}
