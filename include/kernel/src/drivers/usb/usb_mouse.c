/*
 * USB HID Mouse Driver — Phase 5
 *
 * Implements a USB Boot Protocol mouse driver on top of the UHCI stack.
 * After device enumeration (Phase 4), this driver:
 *
 *   1. Reads the Configuration, Interface and Endpoint descriptors to find
 *      an HID mouse interface with an IN interrupt endpoint.
 *   2. Sends SET_CONFIGURATION, SET_PROTOCOL (Boot Protocol), and SET_IDLE.
 *   3. Sets up a persistent UHCI interrupt transfer queue that polls the
 *      mouse at the endpoint's bInterval rate.
 *   4. On each poll tick, reads the 3/4-byte Boot Mouse Report and
 *      translates button/motion data into the kernel's existing mouse
 *      input subsystem (mouse_state_t + evdev events).
 *
 * The USB Boot Mouse Report format:
 *   Byte 0: Buttons (bit0=Left, bit1=Right, bit2=Middle)
 *   Byte 1: X displacement (signed int8)
 *   Byte 2: Y displacement (signed int8)
 *   Byte 3: Wheel (signed int8, optional)
 */

#include "usb_mouse.h"
#include "../../console/klog.h"
#include "../../fb/framebuffer.h"
#include "../../io/io.h"
#include "../../mm/dma_alloc.h"
#include "../input/evdev.h"
#include "../input/mouse.h"
#include "ehci.h"
#include "ohci.h"
#include "uhci.h"
#include "usb.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// USB Descriptor Types (shared with usb_kbd.c)

struct usb_config_descriptor_m {
  uint8_t length;
  uint8_t type;
  uint16_t total_length;
  uint8_t num_interfaces;
  uint8_t config_value;
  uint8_t config_string_idx;
  uint8_t attributes;
  uint8_t max_power;
} __attribute__((packed));

struct usb_interface_descriptor_m {
  uint8_t length;
  uint8_t type;
  uint8_t interface_number;
  uint8_t alternate_setting;
  uint8_t num_endpoints;
  uint8_t interface_class;
  uint8_t interface_subclass;
  uint8_t interface_protocol;
  uint8_t interface_string_idx;
} __attribute__((packed));

struct usb_endpoint_descriptor_m {
  uint8_t length;
  uint8_t type;
  uint8_t endpoint_address;
  uint8_t attributes;
  uint16_t max_packet_size;
  uint8_t interval;
} __attribute__((packed));

#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_MOUSE 0x02

#define USB_REQ_SET_IDLE_M 0x0A
#define USB_REQ_SET_PROTOCOL_M 0x0B
#define HID_PROTOCOL_BOOT_M 0x00

// Mouse state

#define MAX_USB_MICE 4

struct usb_mouse_state {
  struct usb_device *dev;
  struct uhci_controller *hc; // Non-NULL only for UHCI-backed devices
  uint8_t ep_addr;     // Endpoint address (direction bit + endpoint number)
  uint8_t ep_number;   // Endpoint number (0-15)
  uint16_t max_packet; // Max packet size from endpoint descriptor
  uint8_t interval;    // Polling interval (in ms frames)
  uint8_t data_toggle; // DATA0/DATA1 toggle for interrupt IN
  uint8_t interface_number;

  // DMA buffer for interrupt transfer data
  void *report_buf;
  uint32_t report_buf_phys;

  // Previous button state for detecting press/release transitions
  uint8_t prev_buttons;

  // Interrupt transfer scheduling (UHCI path)
  struct uhci_td *int_td;
  uint32_t int_td_phys;
  struct uhci_qh *int_qh;
  uint32_t int_qh_phys;

  // Interrupt transfer scheduling (OHCI path)
  struct ohci_int_pipe *ohci_pipe;

  // Interrupt transfer scheduling (EHCI path)
  struct ehci_int_pipe *ehci_pipe;
  struct usb_interrupt_pipe *generic_pipe;

  bool active;
};

static struct usb_mouse_state mice[MAX_USB_MICE];
static int mouse_count = 0;

// Report Processing

static void usb_mouse_process_report(struct usb_mouse_state *mouse,
                                     uint8_t *buf, uint32_t len) {
  uint8_t buttons = buf[0];
  int32_t dx = (int8_t)buf[1];
  int32_t dy = (int8_t)buf[2];
  int32_t wheel = 0;
  if (len >= 4)
    wheel = (int8_t)buf[3];

  // USB Boot Protocol: positive Y = down (matches screen convention).
  // No inversion needed (unlike PS/2 where positive Y = up).

  // Update the global mouse state (same struct used by PS/2 mouse)
  // We read the current state and update it. The mouse_get_state() function
  // returns this global struct so cursor painting code uses it.
  mouse_state_t state = mouse_get_state();

  state.x += dx;
  state.y += dy;

  // Clamp to screen bounds
  uint32_t width = fb_get_width();
  uint32_t height = fb_get_height();

  if (state.x < 0)
    state.x = 0;
  if (state.y < 0)
    state.y = 0;
  if (width > 0 && state.x >= (int32_t)width)
    state.x = width - 1;
  if (height > 0 && state.y >= (int32_t)height)
    state.y = height - 1;

  state.left_button = (buttons & USB_MOUSE_BTN_LEFT) != 0;
  state.right_button = (buttons & USB_MOUSE_BTN_RIGHT) != 0;
  state.middle_button = (buttons & USB_MOUSE_BTN_MIDDLE) != 0;

  // Write back — use the setter if available, or update directly
  // Since mouse.c exposes mouse_get_state() but not a setter,
  // we'll push through evdev which is what X11 uses.
  // For console/raw consumers, they also use evdev or the PS/2 mouse.

  // Push evdev events (for X11 / Xorg)
  evdev_device_t *mdev = evdev_get_mouse();
  if (mdev) {
    // Relative motion events
    if (dx != 0)
      evdev_push_event(mdev, EV_REL, REL_X, dx);
    if (dy != 0)
      evdev_push_event(mdev, EV_REL, REL_Y, dy);

    // Scroll wheel
    if (wheel != 0)
      evdev_push_event(mdev, EV_REL, REL_WHEEL, wheel);

    // Button state change events (only emit on transitions)
    bool cur_left = (buttons & USB_MOUSE_BTN_LEFT) != 0;
    bool cur_right = (buttons & USB_MOUSE_BTN_RIGHT) != 0;
    bool cur_middle = (buttons & USB_MOUSE_BTN_MIDDLE) != 0;

    bool prev_left = (mouse->prev_buttons & USB_MOUSE_BTN_LEFT) != 0;
    bool prev_right = (mouse->prev_buttons & USB_MOUSE_BTN_RIGHT) != 0;
    bool prev_middle = (mouse->prev_buttons & USB_MOUSE_BTN_MIDDLE) != 0;

    bool changed = dx != 0 || dy != 0 || wheel != 0;
    if (cur_left != prev_left)
      changed |= evdev_report_key(mdev, BTN_LEFT, cur_left);
    if (cur_right != prev_right)
      changed |= evdev_report_key(mdev, BTN_RIGHT, cur_right);
    if (cur_middle != prev_middle)
      changed |= evdev_report_key(mdev, BTN_MIDDLE, cur_middle);

    // SYN_REPORT marks the end of this event batch
    if (changed)
      evdev_sync(mdev);
  }

  // Update previous button state
  mouse->prev_buttons = buttons;
}

// Interrupt Transfer Setup
//
// Uses QH pool indices 8+ to avoid conflicts with keyboard (1-4) and
// control transfers (0). TD pool indices start at 48+ to avoid keyboard
// (32-35).

static void usb_mouse_setup_interrupt_xfer(struct usb_mouse_state *mouse) {
  struct uhci_controller *hc = mouse->hc;

  // Allocate a dedicated QH from the pool (use index 8 + mouse index)
  int qh_idx = 8 + (int)(mouse - mice);
  mouse->int_qh = &hc->qh_pool[qh_idx];
  mouse->int_qh_phys = hc->qh_pool_phys + (qh_idx * sizeof(struct uhci_qh));

  // Allocate a TD for the interrupt IN transfer
  // Use TD pool slots starting at index 48 (keyboards use 32-35)
  int td_idx = 48 + (int)(mouse - mice);
  mouse->int_td = &hc->td_pool[td_idx];
  mouse->int_td_phys = hc->td_pool_phys + (td_idx * sizeof(struct uhci_td));

  // Build the TD for an IN transfer from the interrupt endpoint
  mouse->int_td->link = TD_LINK_TERMINATE;
  mouse->int_td->status = TD_STATUS_ACTIVE | TD_STATUS_IOC | TD_STATUS_C_ERR;
  if (usb_speed_is_low(mouse->dev->speed))
    mouse->int_td->status |= TD_STATUS_LS;

  uint16_t max_len = mouse->max_packet - 1; // MaxLen field = actual_len - 1
  if (max_len > 7)
    max_len = 7; // Boot protocol is at most 4-8 bytes

  mouse->int_td->token = ((uint32_t)max_len << 21) |
                         ((uint32_t)mouse->data_toggle << 19) |
                         ((uint32_t)mouse->ep_number << 15) |
                         ((uint32_t)mouse->dev->address << 8) | TD_PID_IN;
  mouse->int_td->buffer = mouse->report_buf_phys;

  // Point QH to our TD
  mouse->int_qh->head = QH_LINK_TERMINATE; // No horizontal link
  mouse->int_qh->element = mouse->int_td_phys;

  // Insert our QH into the frame list at every N-th slot
  uint8_t interval = mouse->interval;
  if (interval == 0)
    interval = 8; // Default: 8ms if not specified
  if (interval > 128)
    interval = 128;

  // Round to power of 2 for cleaner scheduling
  uint8_t sched_interval = 1;
  while (sched_interval < interval && sched_interval < 128)
    sched_interval <<= 1;

  for (int i = 0; i < 1024; i += sched_interval) {
    // Chain our mouse interrupt QH before the existing entry.
    // The frame list may already point to the keyboard QH or control QH.
    // We chain: framelist → mouse_qh → (existing chain)
    mouse->int_qh->head = hc->frame_list[i]; // chain to existing
    hc->frame_list[i] = mouse->int_qh_phys | TD_LINK_QH;
  }

  klog_puts("[USB-MOUSE] Interrupt transfer scheduled (interval=");
  klog_uint64(sched_interval);
  klog_puts("ms, endpoint=");
  klog_uint64(mouse->ep_number);
  klog_puts(")\n");
}

// Resubmit the interrupt TD after processing
static void usb_mouse_resubmit_td(struct usb_mouse_state *mouse) {
  // Toggle DATA0/DATA1
  mouse->data_toggle ^= 1;

  uint16_t max_len = mouse->max_packet - 1;
  if (max_len > 7)
    max_len = 7;

  mouse->int_td->link = TD_LINK_TERMINATE;
  mouse->int_td->status = TD_STATUS_ACTIVE | TD_STATUS_IOC | TD_STATUS_C_ERR;
  if (usb_speed_is_low(mouse->dev->speed))
    mouse->int_td->status |= TD_STATUS_LS;

  mouse->int_td->token = ((uint32_t)max_len << 21) |
                         ((uint32_t)mouse->data_toggle << 19) |
                         ((uint32_t)mouse->ep_number << 15) |
                         ((uint32_t)mouse->dev->address << 8) | TD_PID_IN;
  mouse->int_td->buffer = mouse->report_buf_phys;

  // Re-point QH element to our TD
  __asm__ volatile("mfence" ::: "memory");
  mouse->int_qh->element = mouse->int_td_phys;
  __asm__ volatile("mfence" ::: "memory");
}

// Probe & Initialization

bool usb_mouse_probe(struct usb_device *dev) {
  int state_index = -1;
  for (int i = 0; i < mouse_count; i++)
    if (!mice[i].active) {
      state_index = i;
      break;
    }
  if (state_index < 0 && mouse_count < MAX_USB_MICE)
    state_index = mouse_count++;
  if (state_index < 0)
    return false;

  // Only cast to uhci_controller if this device is actually on a UHCI HCD.
  struct uhci_controller *hc = NULL;
  for (int i = 0; i < uhci_get_controller_count(); i++) {
    struct uhci_controller *candidate = uhci_get_controller(i);
    if (candidate && dev->hcd->priv == candidate) {
      hc = candidate;
      break;
    }
  }
  struct usb_control_request req;

  // 1. Read the Configuration Descriptor (first 9 bytes to get total length)
  uint8_t config_buf[256];
  req.request_type = 0x80;
  req.request = USB_REQ_GET_DESCRIPTOR;
  req.value = (USB_DESC_CONFIGURATION << 8) | 0;
  req.index = 0;
  req.length = 9;

  int res = usb_control_transfer(dev, &req, config_buf, 9);
  if (res < 0) {
    klog_puts("[USB-MOUSE] Failed to get config descriptor header\n");
    return false;
  }

  struct usb_config_descriptor_m *cfg =
      (struct usb_config_descriptor_m *)config_buf;
  uint16_t total_len = cfg->total_length;
  if (total_len > sizeof(config_buf))
    total_len = sizeof(config_buf);

  // Read the full configuration descriptor bundle
  req.length = total_len;
  res = usb_control_transfer(dev, &req, config_buf, total_len);
  if (res < 0) {
    klog_puts("[USB-MOUSE] Failed to get full config descriptor\n");
    return false;
  }

  // 2. Parse descriptors to find HID Boot Mouse interface
  uint8_t ep_addr = 0;
  uint16_t ep_max_packet = 0;
  uint8_t ep_interval = 0;
  uint8_t config_value = cfg->config_value;
  bool found_mouse = false;
  bool in_mouse_iface = false;
  uint8_t iface_num = 0;

  uint16_t offset = cfg->length;
  while (offset + 2 <= total_len) {
    uint8_t desc_len = config_buf[offset];
    uint8_t desc_type = config_buf[offset + 1];

    if (desc_len == 0)
      break;

    if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
      struct usb_interface_descriptor_m *iface =
          (struct usb_interface_descriptor_m *)&config_buf[offset];

      klog_puts("[USB-MOUSE] Interface: class=0x");
      klog_hex32(iface->interface_class);
      klog_puts(" subclass=0x");
      klog_hex32(iface->interface_subclass);
      klog_puts(" protocol=0x");
      klog_hex32(iface->interface_protocol);
      klog_puts("\n");

      if (iface->interface_class == USB_CLASS_HID &&
          iface->interface_subclass == USB_SUBCLASS_BOOT &&
          iface->interface_protocol == USB_PROTOCOL_MOUSE) {
        found_mouse = true;
        in_mouse_iface = true;
        iface_num = iface->interface_number;
      } else {
        in_mouse_iface = false;
      }
    }

    if (desc_type == USB_DESC_ENDPOINT && desc_len >= 7 && in_mouse_iface) {
      struct usb_endpoint_descriptor_m *ep =
          (struct usb_endpoint_descriptor_m *)&config_buf[offset];

      // Only want Interrupt IN endpoints
      if ((ep->attributes & 0x03) == 0x03 && (ep->endpoint_address & 0x80)) {
        ep_addr = ep->endpoint_address;
        ep_max_packet = ep->max_packet_size;
        ep_interval = ep->interval;

        klog_puts("[USB-MOUSE] Found Interrupt IN endpoint: addr=0x");
        klog_hex32(ep_addr);
        klog_puts(" maxpkt=");
        klog_uint64(ep_max_packet);
        klog_puts(" interval=");
        klog_uint64(ep_interval);
        klog_puts("ms\n");
      }
    }

    offset += desc_len;
  }

  if (!found_mouse || ep_addr == 0) {
    return false;
  }

  klog_puts("[USB-MOUSE] USB Boot Mouse detected!\n");

  // 3. SET_CONFIGURATION
  req.request_type = 0x00;
  req.request = USB_REQ_SET_CONFIGURATION;
  req.value = config_value;
  req.index = 0;
  req.length = 0;

  if (!dev->configured || dev->configuration_value != config_value) {
    res = usb_control_transfer(dev, &req, NULL, 0);
    if (res < 0) {
      klog_puts("[USB-MOUSE] SET_CONFIGURATION failed\n");
      return false;
    }
    dev->configuration_value = config_value;
    dev->configured = true;
  }

  // Small settle delay
  for (int i = 0; i < 5000; i++)
    io_wait();

  // 4. SET_PROTOCOL to Boot Protocol (protocol 0)
  req.request_type = 0x21;
  req.request = USB_REQ_SET_PROTOCOL_M;
  req.value = HID_PROTOCOL_BOOT_M;
  req.index = iface_num;
  req.length = 0;

  res = usb_control_transfer(dev, &req, NULL, 0);
  if (res < 0) {
    klog_puts("[USB-MOUSE] SET_PROTOCOL failed (non-fatal)\n");
  }

  // 5. SET_IDLE (duration=0 means report only on change)
  req.request_type = 0x21;
  req.request = USB_REQ_SET_IDLE_M;
  req.value = 0;
  req.index = iface_num;
  req.length = 0;

  res = usb_control_transfer(dev, &req, NULL, 0);
  if (res < 0) {
    klog_puts("[USB-MOUSE] SET_IDLE failed (non-fatal)\n");
  }

  // 6. Allocate DMA buffer for interrupt transfer reports
  uint64_t phys;
  void *buf = dma_alloc_page(&phys);
  if (!buf) {
    klog_puts("[USB-MOUSE] Failed to allocate DMA buffer\n");
    return false;
  }

  // Zero the buffer
  uint8_t *p = (uint8_t *)buf;
  for (int i = 0; i < 4096; i++)
    p[i] = 0;

  // 7. Set up mouse state
  struct usb_mouse_state *ms = &mice[state_index];
  ms->dev = dev;
  ms->hc = hc;
  ms->ep_addr = ep_addr;
  ms->ep_number = ep_addr & 0x0F;
  ms->max_packet = ep_max_packet;
  ms->interval = ep_interval;
  ms->data_toggle = 0;
  ms->interface_number = iface_num;
  ms->report_buf = buf;
  ms->report_buf_phys = (uint32_t)phys;
  ms->prev_buttons = 0;
  ms->active = true;

  // 8. Schedule the interrupt transfer
  ms->generic_pipe = usb_interrupt_open(
      dev, ms->ep_addr, ms->max_packet, ms->interval, ms->report_buf,
      ms->report_buf_phys);
  if (ms->generic_pipe) {
    klog_puts("[USB-MOUSE] Generic HCD interrupt pipe active\n");
  } else if (ms->hc != NULL) {
    // UHCI path
    usb_mouse_setup_interrupt_xfer(ms);
  } else {
    bool pipe_found = false;

    // EHCI path — check if this device is on an EHCI controller
    for (int ci = 0; ci < ehci_get_controller_count(); ci++) {
      struct ehci_controller *ehc = ehci_get_controller(ci);
      if (ehc && dev->hcd->priv == ehc) {
        ms->ehci_pipe = ehci_setup_int_in(
            ehc, dev->address, ms->ep_number, ms->max_packet, ms->interval,
            usb_speed_is_low(dev->speed), ms->report_buf, ms->report_buf_phys);
        if (ms->ehci_pipe)
          pipe_found = true;
        else
          klog_puts("[USB-MOUSE] Failed to set up EHCI interrupt pipe\n");
        break;
      }
    }

    // OHCI path — check if this device is on an OHCI controller
    if (!pipe_found) {
      for (int ci = 0; ci < ohci_get_controller_count(); ci++) {
        struct ohci_controller *ohc = ohci_get_controller(ci);
        if (ohc && dev->hcd->priv == ohc) {
          ms->ohci_pipe = ohci_setup_int_in(
              ohc, dev->address, ms->ep_number, ms->max_packet, ms->interval,
              usb_speed_is_low(dev->speed), ms->report_buf, ms->report_buf_phys);
          if (ms->ohci_pipe)
            pipe_found = true;
          else
            klog_puts("[USB-MOUSE] Failed to set up OHCI interrupt pipe\n");
          break;
        }
      }
    }
  }

  klog_puts("[USB-MOUSE] Mouse driver attached (addr=");
  klog_uint64(dev->address);
  klog_puts(", ep=");
  klog_uint64(ms->ep_number);
  klog_puts(")\n");

  return true;
}

void usb_mouse_disconnect(struct usb_device *dev) {
  for (int i = 0; i < mouse_count; i++) {
    struct usb_mouse_state *ms = &mice[i];
    if (!ms->active || ms->dev != dev)
      continue;
    if (ms->generic_pipe)
      usb_interrupt_cancel(ms->generic_pipe);
    ms->generic_pipe = NULL;
    ms->active = false;
    ms->dev = NULL;
    klog_puts("[USB-MOUSE] Mouse detached\n");
  }
}

// Polling
// Called from the UHCI IRQ handler on IOC completion to check if
// any mouse has new data.

void usb_mouse_poll(void) {
  for (int i = 0; i < mouse_count; i++) {
    struct usb_mouse_state *ms = &mice[i];
    if (!ms->active)
      continue;

    if (ms->generic_pipe) {
      if (!usb_interrupt_completed(ms->generic_pipe))
        continue;
      uint8_t *buf = ms->generic_pipe->buffer;
      bool has_motion = buf[1] != 0 || buf[2] != 0;
      bool has_wheel = ms->max_packet >= 4 && buf[3] != 0;
      bool btn_changed = buf[0] != ms->prev_buttons;
      if (has_motion || has_wheel || btn_changed)
        usb_mouse_process_report(ms, buf, ms->max_packet);
      usb_interrupt_resubmit(ms->generic_pipe);
      continue;
    }

    // EHCI path
    if (ms->ehci_pipe) {
      if (!ehci_int_pipe_completed(ms->ehci_pipe))
        continue;

      uint8_t *buf = (uint8_t *)ms->ehci_pipe->data_buf;
      bool has_motion = (buf[1] != 0) || (buf[2] != 0);
      bool has_wheel = (ms->max_packet >= 4 && buf[3] != 0);
      bool btn_changed = (buf[0] != ms->prev_buttons);

      if (has_motion || has_wheel || btn_changed)
        usb_mouse_process_report(ms, buf, ms->max_packet);

      ehci_int_pipe_resubmit(ms->ehci_pipe);
      continue;
    }

    // OHCI path
    if (ms->ohci_pipe) {
      if (!ohci_int_pipe_completed(ms->ohci_pipe))
        continue;

      uint8_t *buf = (uint8_t *)ms->ohci_pipe->data_buf;
      bool has_motion = (buf[1] != 0) || (buf[2] != 0);
      bool has_wheel = (ms->max_packet >= 4 && buf[3] != 0);
      bool btn_changed = (buf[0] != ms->prev_buttons);

      if (has_motion || has_wheel || btn_changed)
        usb_mouse_process_report(ms, buf, ms->max_packet);

      ohci_int_pipe_resubmit(ms->ohci_pipe);
      continue;
    }

    // UHCI path
    if (!ms->int_td)
      continue;

    if (ms->int_td->status & TD_STATUS_ACTIVE)
      continue;

    if (ms->int_td->status & TD_STATUS_HALTED) {
      usb_mouse_resubmit_td(ms);
      continue;
    }

    uint32_t actual_len = ((ms->int_td->status + 1) & 0x7FF);
    if (actual_len >= 3 && actual_len <= 8) {
      uint8_t *buf = (uint8_t *)ms->report_buf;
      bool has_motion = (buf[1] != 0) || (buf[2] != 0);
      bool has_wheel = (actual_len >= 4 && buf[3] != 0);
      bool btn_changed = (buf[0] != ms->prev_buttons);

      if (has_motion || has_wheel || btn_changed)
        usb_mouse_process_report(ms, buf, actual_len);
    }

    usb_mouse_resubmit_td(ms);
  }
}
