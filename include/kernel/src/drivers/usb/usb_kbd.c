
#include "usb_kbd.h"
#include "../../lock/lockdiag.h"
#include "../../console/console.h"
#include "../../console/klog.h"
#include "../../io/io.h"
#include "../../mm/dma_alloc.h"
#include "../input/evdev.h"
#include "../input/keyboard.h"
#include "ehci.h"
#include "ohci.h"
#include "uhci.h"
#include "usb.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// USB Descriptor Types for parsing

struct usb_config_descriptor {
  uint8_t length;
  uint8_t type;
  uint16_t total_length;
  uint8_t num_interfaces;
  uint8_t config_value;
  uint8_t config_string_idx;
  uint8_t attributes;
  uint8_t max_power;
} __attribute__((packed));

struct usb_interface_descriptor {
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

struct usb_endpoint_descriptor {
  uint8_t length;
  uint8_t type;
  uint8_t endpoint_address;
  uint8_t attributes;
  uint16_t max_packet_size;
  uint8_t interval;
} __attribute__((packed));

#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KEYBOARD 0x01

#define USB_REQ_SET_IDLE 0x0A
#define USB_REQ_SET_PROTOCOL 0x0B
#define HID_PROTOCOL_BOOT 0x00

// HID Usage ID to PS/2 Scancode mapping
// USB HID uses "Usage IDs" (from the HID Usage Tables spec) for key codes.
// We translate these to PS/2 Set 1 scancodes so we can feed them into the
// existing keyboard.c infrastructure.
//
// Table covers Usage IDs 0x00–0x65 (standard keyboard keys).
// 0xFF = no mapping / unused.

static const uint8_t hid_to_scancode[256] = {
    // 0x00-0x03: No Event, Error Roll Over, POST Fail, Error Undefined
    [0x00] = 0x00,
    [0x01] = 0x00,
    [0x02] = 0x00,
    [0x03] = 0x00,

    // 0x04-0x1D: Letters a-z → PS/2 scancodes
    [0x04] = 0x1E, // a → 0x1E
    [0x05] = 0x30, // b → 0x30
    [0x06] = 0x2E, // c → 0x2E
    [0x07] = 0x20, // d → 0x20
    [0x08] = 0x12, // e → 0x12
    [0x09] = 0x21, // f → 0x21
    [0x0A] = 0x22, // g → 0x22
    [0x0B] = 0x23, // h → 0x23
    [0x0C] = 0x17, // i → 0x17
    [0x0D] = 0x24, // j → 0x24
    [0x0E] = 0x25, // k → 0x25
    [0x0F] = 0x26, // l → 0x26
    [0x10] = 0x32, // m → 0x32
    [0x11] = 0x31, // n → 0x31
    [0x12] = 0x18, // o → 0x18
    [0x13] = 0x19, // p → 0x19
    [0x14] = 0x10, // q → 0x10
    [0x15] = 0x13, // r → 0x13
    [0x16] = 0x1F, // s → 0x1F
    [0x17] = 0x14, // t → 0x14
    [0x18] = 0x16, // u → 0x16
    [0x19] = 0x2F, // v → 0x2F
    [0x1A] = 0x11, // w → 0x11
    [0x1B] = 0x2D, // x → 0x2D
    [0x1C] = 0x15, // y → 0x15
    [0x1D] = 0x2C, // z → 0x2C

    // 0x1E-0x27: Numbers 1-9, 0
    [0x1E] = 0x02, // 1
    [0x1F] = 0x03, // 2
    [0x20] = 0x04, // 3
    [0x21] = 0x05, // 4
    [0x22] = 0x06, // 5
    [0x23] = 0x07, // 6
    [0x24] = 0x08, // 7
    [0x25] = 0x09, // 8
    [0x26] = 0x0A, // 9
    [0x27] = 0x0B, // 0

    // 0x28-0x38: Special keys
    [0x28] = 0x1C, // Enter
    [0x29] = 0x01, // Escape
    [0x2A] = 0x0E, // Backspace
    [0x2B] = 0x0F, // Tab
    [0x2C] = 0x39, // Space
    [0x2D] = 0x0C, // - _
    [0x2E] = 0x0D, // = +
    [0x2F] = 0x1A, // [ {
    [0x30] = 0x1B, // ] }
    [0x31] = 0x2B, // \ |
    [0x32] = 0x2B, // Non-US # ~  (same as backslash on US layout)
    [0x33] = 0x27, // ; :
    [0x34] = 0x28, // ' "
    [0x35] = 0x29, // ` ~
    [0x36] = 0x33, // , <
    [0x37] = 0x34, // . >
    [0x38] = 0x35, // / ?

    // 0x39: Caps Lock
    [0x39] = 0x3A,

    // 0x3A-0x45: Function keys F1-F12
    [0x3A] = 0x3B, // F1
    [0x3B] = 0x3C, // F2
    [0x3C] = 0x3D, // F3
    [0x3D] = 0x3E, // F4
    [0x3E] = 0x3F, // F5
    [0x3F] = 0x40, // F6
    [0x40] = 0x41, // F7
    [0x41] = 0x42, // F8
    [0x42] = 0x43, // F9
    [0x43] = 0x44, // F10
    [0x44] = 0x57, // F11
    [0x45] = 0x58, // F12

    // 0x46-0x48: Print Screen, Scroll Lock, Pause
    [0x46] = 0x00, // Print Screen (complex scancode, skip)
    [0x47] = 0x46, // Scroll Lock
    [0x48] = 0x00, // Pause (complex scancode, skip)

    // 0x49-0x4E: Insert, Home, Page Up, Delete, End, Page Down
    [0x49] = 0x52, // Insert       (extended)
    [0x4A] = 0x47, // Home         (extended)
    [0x4B] = 0x49, // Page Up      (extended)
    [0x4C] = 0x53, // Delete       (extended)
    [0x4D] = 0x4F, // End          (extended)
    [0x4E] = 0x51, // Page Down    (extended)

    // 0x4F-0x52: Arrow keys (extended)
    [0x4F] = 0x4D, // Right Arrow  (extended)
    [0x50] = 0x4B, // Left Arrow   (extended)
    [0x51] = 0x50, // Down Arrow   (extended)
    [0x52] = 0x48, // Up Arrow     (extended)

    // 0x53-0x63: Keypad
    [0x53] = 0x45, // Num Lock
    [0x54] = 0x35, // KP /         (extended)
    [0x55] = 0x37, // KP *
    [0x56] = 0x4A, // KP -
    [0x57] = 0x4E, // KP +
    [0x58] = 0x1C, // KP Enter     (extended)
    [0x59] = 0x4F, // KP 1
    [0x5A] = 0x50, // KP 2
    [0x5B] = 0x51, // KP 3
    [0x5C] = 0x4B, // KP 4
    [0x5D] = 0x4C, // KP 5
    [0x5E] = 0x4D, // KP 6
    [0x5F] = 0x47, // KP 7
    [0x60] = 0x48, // KP 8
    [0x61] = 0x49, // KP 9
    [0x62] = 0x52, // KP 0
    [0x63] = 0x53, // KP .

    // 0x64-0x65: Non-US \, Application
    [0x64] = 0x56, // Non-US \ |
    [0x65] = 0x00, // Application (no PS/2 equivalent)
};

// Which HID usage IDs are "extended" (E0-prefixed) in PS/2 scancode set 1.
// These need special treatment so evdev_ps2_to_keycode gets the right mapping.
static bool hid_is_extended(uint8_t usage) {
  switch (usage) {
  case 0x49: // Insert
  case 0x4A: // Home
  case 0x4B: // Page Up
  case 0x4C: // Delete
  case 0x4D: // End
  case 0x4E: // Page Down
  case 0x4F: // Right
  case 0x50: // Left
  case 0x51: // Down
  case 0x52: // Up
  case 0x54: // KP /
  case 0x58: // KP Enter
    return true;
  default:
    return false;
  }
}

// Keyboard state

#define MAX_USB_KEYBOARDS 4

struct usb_kbd_state {
  struct usb_device *dev;
  struct uhci_controller *hc; // Non-NULL only for UHCI-backed devices
  uint8_t ep_addr;     // Endpoint address (direction bit + endpoint number)
  uint8_t ep_number;   // Endpoint number (0-15)
  uint16_t max_packet; // Max packet size from endpoint descriptor
  uint8_t interval;    // Polling interval (in ms frames)
  uint8_t data_toggle; // DATA0/DATA1 toggle for interrupt IN

  uint8_t interface_number;
  bool caps_lock;

  // DMA buffer for interrupt transfer data (8 bytes for boot protocol)
  void *report_buf;
  uint32_t report_buf_phys;

  // Previous report for detecting press/release transitions
  struct usb_kbd_report prev_report;

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
  bool report_format_known;
  bool report_has_id;
  uint8_t last_pressed_usage;
  int repeat_delay_counter;
};

static struct usb_kbd_state keyboards[MAX_USB_KEYBOARDS];
static int kbd_count = 0;

// HID report → input events translation

// Modifier bit positions map to these pseudo-scancodes
static void process_modifier(uint8_t old_mods, uint8_t new_mods, uint8_t bit,
                             uint8_t scancode, bool is_extended) {
  bool was = (old_mods & bit) != 0;
  bool now = (new_mods & bit) != 0;
  if (was == now)
    return;

  // Push evdev event
  uint16_t kc = evdev_ps2_to_keycode(scancode, is_extended);
  if (kc) {
    evdev_device_t *kdev = evdev_get_keyboard();
    if (kdev) {
      if (evdev_report_key(kdev, kc, now))
        evdev_sync(kdev);
    }
  }

  // Push raw scancode event for games like Doom
  keyboard_push_scancode(scancode, is_extended, !now);

  /* Also feed the hang-report trigger from the modifier transition itself.
   * Right-Ctrl three times is the trigger, and this is the one place a
   * modifier-only HID report is guaranteed to be noticed - measured in QEMU, a
   * report that changes only the modifier byte produced no scancode events at
   * all, so a trigger that relied on the scancode funnel alone would never
   * fire.  Idempotent with the other entry points: they share one tap counter
   * and one cooldown. */
  lockdiag_keyboard_scancode(scancode, is_extended, !now);
}

static void process_modifiers(uint8_t old_mods, uint8_t new_mods) {
  process_modifier(old_mods, new_mods, USB_KBD_MOD_LCTRL, 0x1D, false);
  process_modifier(old_mods, new_mods, USB_KBD_MOD_LSHIFT, 0x2A, false);
  process_modifier(old_mods, new_mods, USB_KBD_MOD_LALT, 0x38, false);
  process_modifier(old_mods, new_mods, USB_KBD_MOD_RCTRL, 0x1D, true);
  process_modifier(old_mods, new_mods, USB_KBD_MOD_RSHIFT, 0x36, false);
  process_modifier(old_mods, new_mods, USB_KBD_MOD_RALT, 0x38, true);
  // GUI keys don't have PS/2 equivalents in our table, skip them
}

// Check if a usage code is present in a key array
static bool key_in_array(uint8_t usage, const uint8_t *keys, int count) {
  for (int i = 0; i < count; i++) {
    if (keys[i] == usage)
      return true;
  }
  return false;
}

// PS/2 scancode → character translation
// We reuse the existing scancode_to_char tables from keyboard.c.
// These are defined as extern because keyboard.c defines them.
extern const char scancode_to_char[];
extern const char scancode_to_char_shift[];

static void usb_kbd_update_leds(struct usb_kbd_state *kbd);

static void usb_kbd_decode_report(struct usb_kbd_state *kbd,
                                  const uint8_t *buf, uint16_t available,
                                  struct usb_kbd_report *report) {
  uint8_t offset = 0;

  /* A 9-byte packet is an 8-byte boot report prefixed by a Report ID.
     xHCI now supplies the actual transfer length, so this is unambiguous. */
  if (!kbd->report_format_known && available == 9) {
    kbd->report_has_id = true;
    kbd->report_format_known = true;
    klog_puts("[USB-KBD] 9-byte HID report has a leading Report ID\n");
    console_puts("[USB-KBD] 9-byte HID report has a leading Report ID\n");
  }

  /* Boot reports are normally eight bytes, but some otherwise boot-compatible
     keyboards keep a leading Report ID even after SET_PROTOCOL. If interpreted
     as modifiers, Report ID 1 makes every letter a Ctrl character while Enter
     appears to work normally. An idle or ordinary key report distinguishes
     the two layouts unambiguously. */
  if (!kbd->report_format_known && available >= 3) {
    if (buf[0] != 0 && buf[1] == 0 && buf[2] == 0) {
      kbd->report_has_id = true;
      kbd->report_format_known = true;
      klog_puts("[USB-KBD] HID report has leading Report ID=0x");
      klog_hex32(buf[0]);
      klog_puts("\n");
    } else if (buf[1] == 0 && buf[2] >= 4) {
      kbd->report_has_id = false;
      kbd->report_format_known = true;
      klog_puts("[USB-KBD] HID report uses standard 8-byte boot format\n");
      console_puts("[USB-KBD] HID report uses standard 8-byte boot format\n");
    }
  }

  if (kbd->report_has_id)
    offset = 1;
  report->modifiers = offset < available ? buf[offset] : 0;
  report->reserved = offset + 1 < available ? buf[offset + 1] : 0;
  for (int j = 0; j < 6; j++)
    report->keys[j] = offset + j + 2 < available ? buf[offset + j + 2] : 0;
}

static void usb_kbd_inject_key(struct usb_kbd_state *kbd, uint8_t usage,
                               struct usb_kbd_report *report) {
  uint8_t sc = hid_to_scancode[usage];
  if (sc == 0)
    return;

  bool extended = hid_is_extended(usage);

  // Push evdev event (for X11 / scancode mode)
  uint16_t kc = evdev_ps2_to_keycode(sc, extended);
  if (kc) {
    evdev_device_t *kdev = evdev_get_keyboard();
    if (kdev) {
      if (evdev_report_key(kdev, kc, true))
        evdev_sync(kdev);
    }
  }

  // Push ASCII character into keyboard ring buffer (for console)
  bool shift =
      (report->modifiers & (USB_KBD_MOD_LSHIFT | USB_KBD_MOD_RSHIFT)) != 0;
  bool ctrl =
      (report->modifiers & (USB_KBD_MOD_LCTRL | USB_KBD_MOD_RCTRL)) != 0;

  // Handle extended keys → escape sequences
  if (extended) {
    switch (usage) {
    case 0x52: { // Up
      const char seq[] = {'\x1B', '[', 'A'};
      keyboard_push_bytes(seq, 3);
      break;
    }
    case 0x51: { // Down
      const char seq[] = {'\x1B', '[', 'B'};
      keyboard_push_bytes(seq, 3);
      break;
    }
    case 0x4F: { // Right
      const char seq[] = {'\x1B', '[', 'C'};
      keyboard_push_bytes(seq, 3);
      break;
    }
    case 0x50: { // Left
      const char seq[] = {'\x1B', '[', 'D'};
      keyboard_push_bytes(seq, 3);
      break;
    }
    case 0x4A: { // Home
      const char seq[] = {'\x1B', '[', 'H'};
      keyboard_push_bytes(seq, 3);
      break;
    }
    case 0x4D: { // End
      const char seq[] = {'\x1B', '[', 'F'};
      keyboard_push_bytes(seq, 3);
      break;
    }
    case 0x4B: { // Page Up
      const char seq[] = {'\x1B', '[', '5', '~'};
      keyboard_push_bytes(seq, 4);
      break;
    }
    case 0x4E: { // Page Down
      const char seq[] = {'\x1B', '[', '6', '~'};
      keyboard_push_bytes(seq, 4);
      break;
    }
    case 0x4C: { // Delete
      const char seq[] = {'\x1B', '[', '3', '~'};
      keyboard_push_bytes(seq, 4);
      break;
    }
    case 0x49: { // Insert
      const char seq[] = {'\x1B', '[', '2', '~'};
      keyboard_push_bytes(seq, 4);
      break;
    }
    default:
      break;
    }
  } else if (sc < 88) {
    // Regular key — translate through scancode_to_char
    char c;
    if (shift) {
      c = scancode_to_char_shift[sc];
    } else {
      c = scancode_to_char[sc];
    }

    if (kbd->caps_lock) {
      if (c >= 'a' && c <= 'z')
        c -= 32;
      else if (c >= 'A' && c <= 'Z')
        c += 32;
    }

    if (ctrl && c >= 'a' && c <= 'z') {
      c = c - 'a' + 1;
    } else if (ctrl && c >= 'A' && c <= 'Z') {
      c = c - 'A' + 1;
    }

    if (c != 0) {
      keyboard_push_bytes(&c, 1);
    }
  }
}

static void usb_kbd_handle_repeat(struct usb_kbd_state *kbd) {
  if (kbd->last_pressed_usage == 0)
    return;

  if (kbd->repeat_delay_counter > 0) {
    kbd->repeat_delay_counter--;
  } else {
    // Repeat the key
    usb_kbd_inject_key(kbd, kbd->last_pressed_usage, &kbd->prev_report);
  }
}

static void usb_kbd_process_report(struct usb_kbd_state *kbd,
                                   struct usb_kbd_report *report) {
  struct usb_kbd_report *prev = &kbd->prev_report;

  // 1. Process modifier changes
  process_modifiers(prev->modifiers, report->modifiers);

  // 2. Detect newly released keys (were in prev, not in current)
  for (int i = 0; i < 6; i++) {
    uint8_t usage = prev->keys[i];
    if (usage < 4)
      continue; // skip No Event / Error codes
    if (key_in_array(usage, report->keys, 6))
      continue; // still pressed

    // Key was released
    if (usage == kbd->last_pressed_usage) {
      kbd->last_pressed_usage = 0;
    }

    uint8_t sc = hid_to_scancode[usage];
    if (sc != 0) {
      keyboard_push_scancode(sc, hid_is_extended(usage), true);
    }

    if (sc == 0)
      continue;

    bool extended = hid_is_extended(usage);
    uint16_t kc = evdev_ps2_to_keycode(sc, extended);
    if (kc) {
      evdev_device_t *kdev = evdev_get_keyboard();
      if (kdev) {
        if (evdev_report_key(kdev, kc, false))
          evdev_sync(kdev);
      }
    }
  }

  // 3. Detect newly pressed keys (in current, not in prev)
  for (int i = 0; i < 6; i++) {
    uint8_t usage = report->keys[i];
    if (usage < 4)
      continue;
    if (key_in_array(usage, prev->keys, 6))
      continue; // was already pressed

    // Key was just pressed
    if (usage >= 0x04 && usage < 0xE0 && usage != 0x39) {
      kbd->last_pressed_usage = usage;
      kbd->repeat_delay_counter = 12; // ~500ms delay at 40ms heartbeat
    }

    uint8_t sc_press = hid_to_scancode[usage];
    if (sc_press != 0) {
      keyboard_push_scancode(sc_press, hid_is_extended(usage), false);
    }

    usb_kbd_inject_key(kbd, usage, report);

    if (usage == 0x39) { // Caps Lock
      kbd->caps_lock = !kbd->caps_lock;
      usb_kbd_update_leds(kbd);
    }
  }

  // 4. Save current report as previous
  *prev = *report;
}

// LED Updates

static void usb_kbd_update_leds(struct usb_kbd_state *kbd) {
  struct usb_control_request req;
  uint8_t report = 0;

  if (kbd->caps_lock)
    report |= (1 << 1);

  // bmRequestType: 0x21 (Host-to-device, Class, Interface)
  // bRequest: 0x09 (SET_REPORT)
  // wValue: 0x0200 (Report Type: Output (0x02), Report ID: 0)
  // wIndex: Interface number
  // wLength: 1
  req.request_type = 0x21;
  req.request = 0x09;
  req.value = 0x0200;
  req.index = kbd->interface_number;
  req.length = 1;

  usb_control_transfer(kbd->dev, &req, &report, 1);
}

// Interrupt Transfer Setup
//
// UHCI schedules interrupt transfers by placing a QH into specific frame list
// slots. The bInterval from the endpoint descriptor tells us every how many
// milliseconds to poll. We place our QH in every N-th frame slot.
//
// We use QH pool index 1 (index 0 is used for control transfers).

static void usb_kbd_setup_interrupt_xfer(struct usb_kbd_state *kbd) {
  struct uhci_controller *hc = kbd->hc;

  // Allocate a dedicated QH from the pool (use index 1 + kbd index)
  int qh_idx = 1 + (int)(kbd - keyboards);
  kbd->int_qh = &hc->qh_pool[qh_idx];
  kbd->int_qh_phys = hc->qh_pool_phys + (qh_idx * sizeof(struct uhci_qh));

  // Allocate a TD for the interrupt IN transfer
  // Use TD pool slots starting at index 32 (to not conflict with control xfers)
  int td_idx = 32 + (int)(kbd - keyboards);
  kbd->int_td = &hc->td_pool[td_idx];
  kbd->int_td_phys = hc->td_pool_phys + (td_idx * sizeof(struct uhci_td));

  // Build the TD for an IN transfer from the interrupt endpoint
  kbd->int_td->link = TD_LINK_TERMINATE;
  kbd->int_td->status = TD_STATUS_ACTIVE | TD_STATUS_IOC | TD_STATUS_C_ERR;
  if (usb_speed_is_low(kbd->dev->speed))
    kbd->int_td->status |= TD_STATUS_LS;

  uint16_t max_len = kbd->max_packet - 1; // MaxLen field = actual_len - 1
  if (max_len > 7)
    max_len = 7; // Boot protocol is 8 bytes max

  kbd->int_td->token = ((uint32_t)max_len << 21) |
                       ((uint32_t)kbd->data_toggle << 19) |
                       ((uint32_t)kbd->ep_number << 15) |
                       ((uint32_t)kbd->dev->address << 8) | TD_PID_IN;
  kbd->int_td->buffer = kbd->report_buf_phys;

  // Point QH to our TD
  kbd->int_qh->head = QH_LINK_TERMINATE; // No horizontal link
  kbd->int_qh->element = kbd->int_td_phys;

  // Insert our QH into the frame list at every N-th slot
  // The interval determines how often this gets executed.
  // UHCI processes one frame every 1ms, so interval=N means every Nms.
  uint8_t interval = kbd->interval;
  if (interval == 0)
    interval = 8; // Default: 8ms if not specified
  if (interval > 128)
    interval = 128;

  // Round to power of 2 for cleaner scheduling
  uint8_t sched_interval = 1;
  while (sched_interval < interval && sched_interval < 128)
    sched_interval <<= 1;

  for (int i = 0; i < 1024; i += sched_interval) {
    // Chain our interrupt QH before the existing control QH.
    // Current frame list entry points to qh_pool[0] (control QH).
    // We make it point to our interrupt QH, which chains to qh_pool[0].
    kbd->int_qh->head = hc->frame_list[i]; // chain to existing
    hc->frame_list[i] = kbd->int_qh_phys | TD_LINK_QH;
  }

  klog_puts("[USB-KBD] Interrupt transfer scheduled (interval=");
  klog_uint64(sched_interval);
  klog_puts("ms, endpoint=");
  klog_uint64(kbd->ep_number);
  klog_puts(")\n");
}

// Resubmit the interrupt TD after processing
static void usb_kbd_resubmit_td(struct usb_kbd_state *kbd) {
  // Toggle DATA0/DATA1
  kbd->data_toggle ^= 1;

  uint16_t max_len = kbd->max_packet - 1;
  if (max_len > 7)
    max_len = 7;

  kbd->int_td->link = TD_LINK_TERMINATE;
  kbd->int_td->status = TD_STATUS_ACTIVE | TD_STATUS_IOC | TD_STATUS_C_ERR;
  if (usb_speed_is_low(kbd->dev->speed))
    kbd->int_td->status |= TD_STATUS_LS;

  kbd->int_td->token = ((uint32_t)max_len << 21) |
                       ((uint32_t)kbd->data_toggle << 19) |
                       ((uint32_t)kbd->ep_number << 15) |
                       ((uint32_t)kbd->dev->address << 8) | TD_PID_IN;
  kbd->int_td->buffer = kbd->report_buf_phys;

  // Re-point QH element to our TD
  __asm__ volatile("mfence" ::: "memory");
  kbd->int_qh->element = kbd->int_td_phys;
  __asm__ volatile("mfence" ::: "memory");
}

// Probe & Initialization

bool usb_kbd_probe(struct usb_device *dev) {
  int state_index = -1;
  for (int i = 0; i < kbd_count; i++)
    if (!keyboards[i].active) {
      state_index = i;
      break;
    }
  if (state_index < 0 && kbd_count < MAX_USB_KEYBOARDS)
    state_index = kbd_count++;
  if (state_index < 0)
    return false;

  // Only cast to uhci_controller if this device is actually on a UHCI HCD.
  struct uhci_controller *hc = NULL;
  // Check all UHCI controllers to see if priv matches
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
    klog_puts("[USB-KBD] Failed to get config descriptor header\n");
    return false;
  }

  struct usb_config_descriptor *cfg =
      (struct usb_config_descriptor *)config_buf;
  uint16_t total_len = cfg->total_length;
  if (total_len > sizeof(config_buf))
    total_len = sizeof(config_buf);

  // Read the full configuration descriptor bundle
  req.length = total_len;
  res = usb_control_transfer(dev, &req, config_buf, total_len);
  if (res < 0) {
    klog_puts("[USB-KBD] Failed to get full config descriptor\n");
    return false;
  }

  // 2. Parse descriptors to find HID Boot Keyboard interface
  uint8_t ep_addr = 0;
  uint16_t ep_max_packet = 0;
  uint8_t ep_interval = 0;
  uint8_t config_value = cfg->config_value;
  bool found_keyboard = false;
  bool in_keyboard_iface = false;

  uint8_t iface_num = 0;
  uint16_t offset = cfg->length;
  while (offset + 2 <= total_len) {
    uint8_t desc_len = config_buf[offset];
    uint8_t desc_type = config_buf[offset + 1];

    if (desc_len == 0)
      break;

    if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
      struct usb_interface_descriptor *iface =
          (struct usb_interface_descriptor *)&config_buf[offset];

      klog_puts("[USB-KBD] Interface: class=0x");
      klog_hex32(iface->interface_class);
      klog_puts(" subclass=0x");
      klog_hex32(iface->interface_subclass);
      klog_puts(" protocol=0x");
      klog_hex32(iface->interface_protocol);
      klog_puts("\n");

      if (iface->interface_class == USB_CLASS_HID &&
          iface->interface_subclass == USB_SUBCLASS_BOOT &&
          iface->interface_protocol == USB_PROTOCOL_KEYBOARD) {
        found_keyboard = true;
        in_keyboard_iface = true;
        iface_num = iface->interface_number;
      } else {
        in_keyboard_iface = false;
      }
    }

    if (desc_type == USB_DESC_ENDPOINT && desc_len >= 7 && in_keyboard_iface) {
      struct usb_endpoint_descriptor *ep =
          (struct usb_endpoint_descriptor *)&config_buf[offset];

      // Only want Interrupt IN endpoints
      if ((ep->attributes & 0x03) == 0x03 && (ep->endpoint_address & 0x80)) {
        ep_addr = ep->endpoint_address;
        ep_max_packet = ep->max_packet_size;
        ep_interval = ep->interval;

        klog_puts("[USB-KBD] Found Interrupt IN endpoint: addr=0x");
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

  if (!found_keyboard || ep_addr == 0) {
    // Not a keyboard — check class code as fallback
    if (dev->desc.device_class == USB_CLASS_HID) {
      klog_puts("[USB-KBD] HID device found but not a boot keyboard\n");
    }
    return false;
  }

  klog_puts("[USB-KBD] USB Boot Keyboard detected!\n");

  // 3. SET_CONFIGURATION
  req.request_type = 0x00;
  req.request = USB_REQ_SET_CONFIGURATION;
  req.value = config_value;
  req.index = 0;
  req.length = 0;

  if (!dev->configured || dev->configuration_value != config_value) {
    res = usb_control_transfer(dev, &req, NULL, 0);
    if (res < 0) {
      klog_puts("[USB-KBD] SET_CONFIGURATION failed\n");
      return false;
    }
    dev->configuration_value = config_value;
    dev->configured = true;
  }

  // Small settle delay
  for (int i = 0; i < 5000; i++)
    io_wait();

  // 4. SET_PROTOCOL to Boot Protocol (protocol 0)
  //    bmRequestType=0x21 (class, interface), bRequest=0x0B
  req.request_type = 0x21;
  req.request = USB_REQ_SET_PROTOCOL;
  req.value = HID_PROTOCOL_BOOT;
  req.index = iface_num;
  req.length = 0;

  res = usb_control_transfer(dev, &req, NULL, 0);
  if (res < 0) {
    klog_puts("[USB-KBD] SET_PROTOCOL failed (non-fatal)\n");
    // Not fatal — many keyboards work in boot protocol by default
  }

  // 5. SET_IDLE (duration=0 means report only on change)
  //    bmRequestType=0x21, bRequest=0x0A
  req.request_type = 0x21;
  req.request = USB_REQ_SET_IDLE;
  req.value = (10 << 8); // duration=10 (40ms), report_id=0
  req.index = iface_num;
  req.length = 0;

  res = usb_control_transfer(dev, &req, NULL, 0);
  if (res < 0) {
    klog_puts("[USB-KBD] SET_IDLE failed (non-fatal)\n");
  }

  // 6. Allocate DMA buffer for interrupt transfer reports
  uint64_t phys;
  void *buf = dma_alloc_page(&phys);
  if (!buf) {
    klog_puts("[USB-KBD] Failed to allocate DMA buffer\n");
    return false;
  }

  // Zero the buffer
  uint8_t *p = (uint8_t *)buf;
  for (int i = 0; i < 4096; i++)
    p[i] = 0;

  // 7. Set up keyboard state
  struct usb_kbd_state *kbd = &keyboards[state_index];
  kbd->dev = dev;
  kbd->hc = hc;
  kbd->ep_addr = ep_addr;
  kbd->ep_number = ep_addr & 0x0F;
  kbd->max_packet = ep_max_packet;
  kbd->interval = ep_interval;
  kbd->data_toggle = 0;
  kbd->report_buf = buf;
  kbd->report_buf_phys = (uint32_t)phys;
  kbd->interface_number = iface_num;
  kbd->caps_lock = false;
  kbd->report_format_known = false;
  kbd->report_has_id = false;
  kbd->active = true;

  // Zero out previous report
  for (int i = 0; i < 8; i++)
    ((uint8_t *)&kbd->prev_report)[i] = 0;

  // 8. Schedule the interrupt transfer
  kbd->generic_pipe = usb_interrupt_open(
      dev, kbd->ep_addr, kbd->max_packet, kbd->interval, kbd->report_buf,
      kbd->report_buf_phys);
  if (kbd->generic_pipe) {
    klog_puts("[USB-KBD] Generic HCD interrupt pipe active\n");
  } else if (kbd->hc != NULL) {
    // UHCI path
    usb_kbd_setup_interrupt_xfer(kbd);
  } else {
    bool pipe_found = false;

    // EHCI path — check if this device is on an EHCI controller
    for (int ci = 0; ci < ehci_get_controller_count(); ci++) {
      struct ehci_controller *ehc = ehci_get_controller(ci);
      if (ehc && dev->hcd->priv == ehc) {
        kbd->ehci_pipe = ehci_setup_int_in(
            ehc, dev->address, kbd->ep_number, kbd->max_packet, kbd->interval,
            usb_speed_is_low(dev->speed), kbd->report_buf, kbd->report_buf_phys);
        if (kbd->ehci_pipe)
          pipe_found = true;
        else
          klog_puts("[USB-KBD] Failed to set up EHCI interrupt pipe\n");
        break;
      }
    }

    // OHCI path — check if this device is on an OHCI controller
    if (!pipe_found) {
      for (int ci = 0; ci < ohci_get_controller_count(); ci++) {
        struct ohci_controller *ohc = ohci_get_controller(ci);
        if (ohc && dev->hcd->priv == ohc) {
          kbd->ohci_pipe = ohci_setup_int_in(
              ohc, dev->address, kbd->ep_number, kbd->max_packet, kbd->interval,
              usb_speed_is_low(dev->speed), kbd->report_buf, kbd->report_buf_phys);
          if (kbd->ohci_pipe)
            pipe_found = true;
          else
            klog_puts("[USB-KBD] Failed to set up OHCI interrupt pipe\n");
          break;
        }
      }
    }
  }

  klog_puts("[USB-KBD] Keyboard driver attached (addr=");
  klog_uint64(dev->address);
  klog_puts(", ep=");
  klog_uint64(kbd->ep_number);
  klog_puts(")\n");

  return true;
}

void usb_kbd_disconnect(struct usb_device *dev) {
  for (int i = 0; i < kbd_count; i++) {
    struct usb_kbd_state *kbd = &keyboards[i];
    if (!kbd->active || kbd->dev != dev)
      continue;
    if (kbd->generic_pipe)
      usb_interrupt_cancel(kbd->generic_pipe);
    kbd->generic_pipe = NULL;
    kbd->active = false;
    kbd->dev = NULL;
    klog_puts("[USB-KBD] Keyboard detached\n");
  }
}

uint32_t usb_kbd_active_count(void) {
  uint32_t active = 0;
  for (int i = 0; i < kbd_count; i++) {
    if (keyboards[i].active)
      active++;
  }
  return active;
}

const char *usb_kbd_report_format(void) {
  for (int i = 0; i < kbd_count; i++) {
    struct usb_kbd_state *kbd = &keyboards[i];
    if (!kbd->active)
      continue;
    if (!kbd->report_format_known)
      return "waiting for first key report";
    return kbd->report_has_id ? "9-byte report with Report ID"
                              : "standard 8-byte boot format";
  }
  return "no keyboard detected";
}

// Polling
// Called from the UHCI/OHCI IRQ handler to check if any keyboard has new data.

void usb_kbd_poll(void) {
  for (int i = 0; i < kbd_count; i++) {
    struct usb_kbd_state *kbd = &keyboards[i];
    if (!kbd->active)
      continue;

    if (kbd->generic_pipe) {
      if (!usb_interrupt_completed(kbd->generic_pipe))
        continue;
      uint8_t *buf = kbd->generic_pipe->buffer;
      struct usb_kbd_report report;
      uint16_t report_len = kbd->generic_pipe->actual_length
                                ? kbd->generic_pipe->actual_length
                                : kbd->generic_pipe->buffer_len;
      usb_kbd_decode_report(kbd, buf, report_len, &report);
      bool changed = report.modifiers != kbd->prev_report.modifiers;
      for (int j = 0; !changed && j < 6; j++)
        changed = report.keys[j] != kbd->prev_report.keys[j];
      if (changed)
        usb_kbd_process_report(kbd, &report);
      else
        usb_kbd_handle_repeat(kbd);
      usb_interrupt_resubmit(kbd->generic_pipe);
      continue;
    }

    // EHCI path
    if (kbd->ehci_pipe) {
      if (!ehci_int_pipe_completed(kbd->ehci_pipe))
        continue;

      uint8_t *buf = (uint8_t *)kbd->ehci_pipe->data_buf;
      struct usb_kbd_report report;
      usb_kbd_decode_report(kbd, buf, kbd->max_packet, &report);

      bool changed = (report.modifiers != kbd->prev_report.modifiers);
      if (!changed) {
        for (int j = 0; j < 6; j++) {
          if (report.keys[j] != kbd->prev_report.keys[j]) {
            changed = true;
            break;
          }
        }
      }

      if (changed)
        usb_kbd_process_report(kbd, &report);
      else
        usb_kbd_handle_repeat(kbd);

      ehci_int_pipe_resubmit(kbd->ehci_pipe);
      continue;
    }

    // OHCI path
    if (kbd->ohci_pipe) {
      if (!ohci_int_pipe_completed(kbd->ohci_pipe))
        continue;

      uint8_t *buf = (uint8_t *)kbd->ohci_pipe->data_buf;
      struct usb_kbd_report report;
      usb_kbd_decode_report(kbd, buf, kbd->max_packet, &report);

      bool changed = (report.modifiers != kbd->prev_report.modifiers);
      if (!changed) {
        for (int j = 0; j < 6; j++) {
          if (report.keys[j] != kbd->prev_report.keys[j]) {
            changed = true;
            break;
          }
        }
      }

      if (changed)
        usb_kbd_process_report(kbd, &report);
      else
        usb_kbd_handle_repeat(kbd);

      ohci_int_pipe_resubmit(kbd->ohci_pipe);
      continue;
    }

    // UHCI path
    if (!kbd->int_td)
      continue;

    if (kbd->int_td->status & TD_STATUS_ACTIVE)
      continue;

    if (kbd->int_td->status & TD_STATUS_HALTED) {
      klog_puts("[USB-KBD] Transfer error, retrying...\n");
      usb_kbd_resubmit_td(kbd);
      continue;
    }

    uint32_t actual_len = ((kbd->int_td->status + 1) & 0x7FF);
    if (actual_len >= 3 && actual_len <= 8) {
      struct usb_kbd_report report;
      uint8_t *buf = (uint8_t *)kbd->report_buf;

      report.modifiers = buf[0];
      report.reserved = buf[1];
      for (int j = 0; j < 6; j++) {
        report.keys[j] = (j + 2 < (int)actual_len) ? buf[j + 2] : 0;
      }

      bool changed = (report.modifiers != kbd->prev_report.modifiers);
      if (!changed) {
        for (int j = 0; j < 6; j++) {
          if (report.keys[j] != kbd->prev_report.keys[j]) {
            changed = true;
            break;
          }
        }
      }

      if (changed)
        usb_kbd_process_report(kbd, &report);
      else
        usb_kbd_handle_repeat(kbd);
    }

    usb_kbd_resubmit_td(kbd);
  }
}
