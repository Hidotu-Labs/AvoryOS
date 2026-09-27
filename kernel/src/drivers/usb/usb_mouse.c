/*
 * USB HID Mouse Driver
 *
 * Unlike the previous boot-protocol-only implementation, this driver parses
 * each HID interface's report descriptor and decodes whatever the device
 * actually sends.  That covers the three layouts seen in the wild:
 *
 *   1. Report protocol with a report ID prefix and 12/16-bit axes (gaming
 *      mice, wireless receivers, HID++ devices).
 *   2. Report protocol without a report ID (classic 3/4-byte layout).
 *   3. Boot protocol (3/4-byte layout), used as a fallback when the report
 *      descriptor cannot be fetched or parsed but the interface advertises
 *      boot-mouse capability (subclass 1, protocol 2).
 *
 * The descriptor parser tracks report IDs, per-report bit offsets, variable
 * fields (X/Y/wheel/pan/buttons), signedness and relative vs absolute axes,
 * so a report is extracted field by field instead of assuming byte offsets.
 *
 * Interrupt transfers are handled by the HCD:
 *   - xHCI (or any HCD exposing the generic pipe API) via usb_interrupt_open.
 *   - UHCI/OHCI/EHCI through their legacy per-controller pipes.
 *
 * Decoded motion/buttons are handed to mouse_apply_report(), which updates
 * the shared mouse state and pushes evdev events for Xorg.
 */

#include "usb_mouse.h"
#include "../../apic/lapic_timer.h"
#include "../../console/klog.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../mm/dma_alloc.h"
#include "../input/mouse.h"
#include "ehci.h"
#include "ohci.h"
#include "uhci.h"
#include "usb.h"
#include "xhci.h"
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
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_PROTOCOL_MOUSE 0x02

#define USB_REQ_SET_IDLE_M 0x0A
#define USB_REQ_SET_PROTOCOL_M 0x0B
#define HID_PROTOCOL_BOOT_M 0x00
#define HID_PROTOCOL_REPORT_M 0x01

// HID report descriptor item constants

#define HID_ITEM_LONG 0xFE
/* A Logitech Unifying receiver declares 16 buttons before X/Y/wheel/pan; a
 * 16-field cap silently drops the axes and the pointer never moves.  Keep
 * room for 24+ buttons plus all axes. */
#define HID_MAX_FIELDS 48
#define HID_MAX_REPORTS 4
#define HID_MAX_USAGES 32
#define HID_MAX_CANDIDATES 4

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_BUTTON 0x09
#define HID_GD_POINTER 0x01
#define HID_GD_MOUSE 0x02
#define HID_GD_X 0x30
#define HID_GD_Y 0x31
#define HID_GD_WHEEL 0x38
#define HID_GD_PAN 0x39

enum hid_field_kind {
  HID_FIELD_NONE = 0,
  HID_FIELD_X,
  HID_FIELD_Y,
  HID_FIELD_WHEEL,
  HID_FIELD_PAN,
  HID_FIELD_BUTTON,
};

struct hid_mouse_field {
  uint8_t kind;         // enum hid_field_kind
  uint8_t button_index; // 0-based HID button number
  uint32_t bit_offset;  // Offset inside the report (after any report ID)
  uint8_t bit_size;     // Field width in bits
  bool is_signed;       // Logical minimum was negative
  bool is_absolute;     // X/Y are absolute (touchpad-style) values
};

struct hid_mouse_report {
  uint8_t id;          // Report ID (0 when the device uses none)
  bool used;           // Slot contains a report
  uint32_t bit_offset; // Running offset while parsing
  uint32_t total_bits; // Report size in bits
  uint8_t field_count;
  uint8_t dropped_fields; // Mouse fields that did not fit HID_MAX_FIELDS
  struct hid_mouse_field fields[HID_MAX_FIELDS];
};

struct hid_mouse_layout {
  bool valid;
  bool has_report_id; // Reports are prefixed with their report ID byte
  uint8_t report_count;
  uint8_t button_count;
  uint8_t dropped_fields; // Sum of per-report dropped fields
  struct hid_mouse_report reports[HID_MAX_REPORTS];
};

// HID report descriptor parser
//
// Walks the item stream once and records the mouse-related fields of the
// application collection(s) whose usage is Generic Desktop Mouse/Pointer.
// Padding and non-mouse fields still advance the per-report bit offset.

struct hid_parser {
  uint32_t usage_page;
  int32_t logical_min;
  int32_t logical_max;
  uint32_t report_size;
  uint32_t report_count;
  uint8_t report_id;
  bool has_report_id;

  uint32_t usages[HID_MAX_USAGES];
  uint8_t usage_count;
  uint32_t usage_min;
  uint32_t usage_max;
  bool usage_range;

  uint8_t collection_depth;
  uint8_t mouse_depth;
  bool in_mouse;

  struct {
    uint32_t usage_page;
    int32_t logical_min;
    int32_t logical_max;
    uint32_t report_size;
    uint32_t report_count;
  } stack[4];
  uint8_t stack_depth;
};

static int32_t hid_sign_extend(uint32_t value, uint8_t size) {
  switch (size) {
  case 1:
    return (int8_t)(value & 0xFF);
  case 2:
    return (int16_t)(value & 0xFFFF);
  case 4:
    return (int32_t)value;
  default:
    return (int32_t)value;
  }
}

static void hid_clear_locals(struct hid_parser *p) {
  p->usage_count = 0;
  p->usage_range = false;
  p->usage_min = 0;
  p->usage_max = 0;
}

static struct hid_mouse_report *hid_report_slot(struct hid_mouse_layout *l,
                                                uint8_t id, bool create) {
  for (uint8_t i = 0; i < HID_MAX_REPORTS; i++) {
    if (l->reports[i].used && l->reports[i].id == id)
      return &l->reports[i];
  }
  if (!create)
    return NULL;
  for (uint8_t i = 0; i < HID_MAX_REPORTS; i++) {
    if (!l->reports[i].used) {
      memset(&l->reports[i], 0, sizeof(l->reports[i]));
      l->reports[i].used = true;
      l->reports[i].id = id;
      return &l->reports[i];
    }
  }
  return NULL;
}

static uint32_t hid_usage_at(const struct hid_parser *p, uint32_t index) {
  if (p->usage_count) {
    if (index < p->usage_count)
      return p->usages[index];
    return p->usages[p->usage_count - 1];
  }
  if (p->usage_range) {
    if (p->usage_min + index <= p->usage_max)
      return p->usage_min + index;
    return p->usage_max;
  }
  return 0;
}

static void hid_parse_input(struct hid_parser *p, struct hid_mouse_layout *l,
                            uint32_t flags) {
  uint32_t count = p->report_count;
  uint32_t size = p->report_size;
  if (!count || !size || size > 32)
    goto done;

  struct hid_mouse_report *rep = hid_report_slot(l, p->report_id, true);
  if (!rep)
    goto done;

  /* HID Input item flag bits: 0 = Data/Constant (0 means Data), 1 = Array/
     Variable (1 means Variable), 2 = Absolute/Relative (1 means Relative). */
  bool data = (flags & 0x01) == 0;
  bool variable = (flags & 0x02) != 0;
  bool relative = (flags & 0x04) != 0;
  uint32_t offset = rep->bit_offset;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t usage = hid_usage_at(p, i);
    uint16_t page =
        (usage >> 16) ? (uint16_t)(usage >> 16) : (uint16_t)p->usage_page;
    uint16_t id = (uint16_t)(usage & 0xFFFF);

    if (p->in_mouse && data && variable) {
      uint8_t kind = HID_FIELD_NONE;
      if (page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
        if (id == HID_GD_X)
          kind = HID_FIELD_X;
        else if (id == HID_GD_Y)
          kind = HID_FIELD_Y;
        else if (id == HID_GD_WHEEL)
          kind = HID_FIELD_WHEEL;
        else if (id == HID_GD_PAN)
          kind = HID_FIELD_PAN;
      } else if (page == HID_USAGE_PAGE_BUTTON && id >= 1 && id <= 16) {
        kind = HID_FIELD_BUTTON;
      }

      if (kind != HID_FIELD_NONE) {
        if (rep->field_count < HID_MAX_FIELDS) {
          struct hid_mouse_field *f = &rep->fields[rep->field_count++];
          f->kind = kind;
          f->button_index = (uint8_t)(id - 1);
          f->bit_offset = offset;
          f->bit_size = (uint8_t)size;
          f->is_signed = p->logical_min < 0;
          f->is_absolute = (kind == HID_FIELD_X || kind == HID_FIELD_Y) &&
                           !relative;
        } else {
          /* Dropping a field silently would turn a 16-button receiver into a
           * pointer that can never move; count it so the probe can say so. */
          if (rep->dropped_fields < 255)
            rep->dropped_fields++;
        }
      }
    }

    offset += size;
    if (offset > 0x00FFFFFFU) {
      offset = 0x00FFFFFFU;
      break;
    }
  }

  rep->bit_offset = offset;
  if (offset > rep->total_bits)
    rep->total_bits = offset;

done:
  hid_clear_locals(p);
}

static bool hid_parse_mouse_layout(const uint8_t *desc, uint32_t len,
                                   struct hid_mouse_layout *out) {
  memset(out, 0, sizeof(*out));

  struct hid_parser p;
  memset(&p, 0, sizeof(p));

  uint32_t i = 0;
  while (i < len) {
    uint8_t prefix = desc[i++];
    if (prefix == HID_ITEM_LONG) {
      if (i + 1 >= len)
        break;
      uint8_t size = desc[i];
      i += (uint32_t)size + 1;
      continue;
    }

    uint8_t size = prefix & 0x03;
    if (size == 3)
      size = 4; // Reserved encoding, treated as 4 bytes
    uint8_t type = (prefix >> 2) & 0x03; // 0 main, 1 global, 2 local
    uint8_t tag = (prefix >> 4) & 0x0F;
    if (i + size > len)
      break;

    uint32_t value = 0;
    for (uint8_t b = 0; b < size; b++)
      value |= (uint32_t)desc[i + b] << (8 * b);
    i += size;

    if (type == 0) { /* Main */
      if (tag == 8) { /* Input */
        hid_parse_input(&p, out, value);
      } else if (tag == 10) { /* Collection */
        p.collection_depth++;
        if ((value & 0xFF) == 0x01 /* Application */ && p.usage_count) {
          uint32_t usage = p.usages[0];
          uint16_t page = (usage >> 16) ? (uint16_t)(usage >> 16)
                                        : (uint16_t)p.usage_page;
          uint16_t id = (uint16_t)(usage & 0xFFFF);
          if (page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
              (id == HID_GD_MOUSE || id == HID_GD_POINTER)) {
            p.in_mouse = true;
            p.mouse_depth = p.collection_depth;
          }
        }
        hid_clear_locals(&p);
      } else if (tag == 12) { /* End Collection */
        if (p.in_mouse && p.collection_depth == p.mouse_depth)
          p.in_mouse = false;
        if (p.collection_depth)
          p.collection_depth--;
        hid_clear_locals(&p);
      } else { /* Output, Feature, reserved */
        hid_clear_locals(&p);
      }
    } else if (type == 1) { /* Global */
      switch (tag) {
      case 0:
        p.usage_page = value;
        break;
      case 1:
        p.logical_min = hid_sign_extend(value, size);
        break;
      case 2:
        p.logical_max = hid_sign_extend(value, size);
        break;
      case 7:
        p.report_size = value;
        break;
      case 8:
        p.report_id = (uint8_t)value;
        p.has_report_id = true;
        break;
      case 9:
        p.report_count = value;
        break;
      case 10: /* Push */
        if (p.stack_depth < 4) {
          p.stack[p.stack_depth].usage_page = p.usage_page;
          p.stack[p.stack_depth].logical_min = p.logical_min;
          p.stack[p.stack_depth].logical_max = p.logical_max;
          p.stack[p.stack_depth].report_size = p.report_size;
          p.stack[p.stack_depth].report_count = p.report_count;
          p.stack_depth++;
        }
        break;
      case 11: /* Pop */
        if (p.stack_depth) {
          p.stack_depth--;
          p.usage_page = p.stack[p.stack_depth].usage_page;
          p.logical_min = p.stack[p.stack_depth].logical_min;
          p.logical_max = p.stack[p.stack_depth].logical_max;
          p.report_size = p.stack[p.stack_depth].report_size;
          p.report_count = p.stack[p.stack_depth].report_count;
        }
        break;
      default:
        break;
      }
    } else if (type == 2) { /* Local */
      if (tag == 0) {
        if (p.usage_count < HID_MAX_USAGES)
          p.usages[p.usage_count++] = value;
      } else if (tag == 1) {
        p.usage_min = value;
        p.usage_range = true;
      } else if (tag == 2) {
        p.usage_max = value;
        p.usage_range = true;
      }
    }
  }

  // Keep only reports that carry at least one mouse field.
  uint8_t buttons = 0;
  uint8_t used_reports = 0;
  uint8_t dropped = 0;
  for (uint8_t r = 0; r < HID_MAX_REPORTS; r++) {
    struct hid_mouse_report *rep = &out->reports[r];
    if (!rep->used)
      continue;
    if (!rep->field_count) {
      rep->used = false;
      continue;
    }
    used_reports++;
    dropped += rep->dropped_fields;
    for (uint8_t f = 0; f < rep->field_count; f++) {
      if (rep->fields[f].kind == HID_FIELD_BUTTON &&
          rep->fields[f].button_index + 1 > buttons)
        buttons = (uint8_t)(rep->fields[f].button_index + 1);
    }
  }

  out->has_report_id = p.has_report_id;
  out->report_count = used_reports;
  out->button_count = buttons > 16 ? 16 : buttons;
  out->dropped_fields = dropped;
  out->valid = used_reports > 0;
  return out->valid;
}

// Synthesize the mandatory 3-byte boot mouse report layout.  Byte 0 holds the
// buttons, bytes 1/2 signed X/Y.  A fourth wheel byte is included when the
// endpoint is large enough because many boot-capable mice keep sending it.
static void hid_build_boot_layout(struct hid_mouse_layout *l,
                                  uint16_t max_packet) {
  memset(l, 0, sizeof(*l));
  l->valid = true;
  l->has_report_id = false;
  l->report_count = 1;
  l->button_count = 3;

  struct hid_mouse_report *rep = &l->reports[0];
  rep->used = true;
  rep->id = 0;

  for (uint8_t b = 0; b < 3; b++) {
    struct hid_mouse_field *f = &rep->fields[rep->field_count++];
    f->kind = HID_FIELD_BUTTON;
    f->button_index = b;
    f->bit_offset = b;
    f->bit_size = 1;
  }

  struct hid_mouse_field *x = &rep->fields[rep->field_count++];
  x->kind = HID_FIELD_X;
  x->bit_offset = 8;
  x->bit_size = 8;
  x->is_signed = true;

  struct hid_mouse_field *y = &rep->fields[rep->field_count++];
  y->kind = HID_FIELD_Y;
  y->bit_offset = 16;
  y->bit_size = 8;
  y->is_signed = true;

  rep->total_bits = 24;
  if (max_packet >= 4) {
    /* Wheel is not part of the boot protocol; many devices still send it as a
     * fourth byte.  Keep the field but leave total_bits at 24 so a 3-byte
     * report is still accepted, and let the per-field bounds check skip the
     * wheel when the packet is short. */
    struct hid_mouse_field *w = &rep->fields[rep->field_count++];
    w->kind = HID_FIELD_WHEEL;
    w->bit_offset = 24;
    w->bit_size = 8;
    w->is_signed = true;
  }
}

static const char *hid_field_name(uint8_t kind) {
  switch (kind) {
  case HID_FIELD_X:
    return "X";
  case HID_FIELD_Y:
    return "Y";
  case HID_FIELD_WHEEL:
    return "wheel";
  case HID_FIELD_PAN:
    return "pan";
  case HID_FIELD_BUTTON:
    return "button";
  default:
    return "?";
  }
}

static void hid_log_layout(const struct hid_mouse_layout *l) {
  klog_puts("[USB-MOUSE] Layout: ");
  klog_puts(l->has_report_id ? "report-id, " : "no report-id, ");
  klog_uint64(l->report_count);
  klog_puts(" report(s), ");
  klog_uint64(l->button_count);
  klog_puts(" button(s)\n");
  if (l->dropped_fields) {
    klog_puts("[USB-MOUSE] WARNING: dropped ");
    klog_uint64(l->dropped_fields);
    klog_puts(" mouse field(s): raise HID_MAX_FIELDS\n");
  }

  for (uint8_t r = 0; r < HID_MAX_REPORTS; r++) {
    const struct hid_mouse_report *rep = &l->reports[r];
    if (!rep->used)
      continue;
    klog_puts("[USB-MOUSE]   report id=");
    klog_uint64(rep->id);
    klog_puts(" size=");
    klog_uint64((rep->total_bits + 7) / 8);
    klog_puts(" bytes, fields:");
    for (uint8_t f = 0; f < rep->field_count; f++) {
      const struct hid_mouse_field *field = &rep->fields[f];
      klog_puts(" ");
      klog_puts(hid_field_name(field->kind));
      if (field->kind == HID_FIELD_BUTTON) {
        klog_puts("#");
        klog_uint64(field->button_index + 1);
      }
      klog_puts("@");
      klog_uint64(field->bit_offset);
      klog_puts("+");
      klog_uint64(field->bit_size);
      if (field->is_signed)
        klog_puts("s");
    }
    klog_puts("\n");
  }
}

// Mouse state

#define MAX_USB_MICE 4
#define USB_MOUSE_REPORT_LOGS 8
#define USB_MOUSE_EVENT_LOG 24

/* One raw interrupt completion (or recovery step) kept for /proc/usb_mouse.
 * A silent mouse has no reports to decode, so the completion record - code,
 * length and bytes - is the only evidence of what the endpoint is doing. */
struct usb_mouse_event {
  uint32_t ms;
  uint8_t code; // xHCI completion code, or 0xF0 + recovery stage
  uint8_t len;
  uint8_t data[12];
};

struct usb_mouse_state {
  struct usb_device *dev;
  struct uhci_controller *hc; // Non-NULL only for UHCI-backed devices
  uint8_t ep_addr;            // Endpoint address (direction bit + number)
  uint8_t ep_number;          // Endpoint number (0-15)
  uint16_t max_packet;        // Max packet size from endpoint descriptor
  uint8_t interval;           // Polling interval
  uint8_t data_toggle;        // DATA0/DATA1 toggle for UHCI interrupt IN
  uint8_t interface_number;

  // DMA buffer for interrupt transfer data
  void *report_buf;
  uint64_t report_buf_phys;

  // Decoded HID layout (descriptor or boot fallback)
  struct hid_mouse_layout layout;
  struct hid_mouse_layout report_layout; // descriptor layout, kept for auto-detect
  bool have_report_layout;
  bool boot_capable;
  bool using_boot;       // active layout is the synthesized boot layout
  uint32_t reports_decoded;
  bool boot_layout;      // layout was synthesized from the boot protocol
  int8_t halt_result;    // CLEAR_FEATURE(ENDPOINT_HALT) result
  const char *transport; // "generic", "uhci", "ehci", "ohci" or "none"
  uint64_t attach_ms;
  bool silent_warned;
  int8_t get_report_result; // GET_REPORT(input) at probe time
  uint8_t get_report[8];

  // Last raw interrupt report, kept for /proc/usb.  When a device's reports
  // do not match the parsed layout this is the only way to see the real
  // format without a serial capture.
  uint8_t last_report[16];
  uint8_t last_report_len;
  uint32_t unknown_reports;

  // Verbose runtime counters (reported at /proc/usb).  A mouse with zero
  // reports needs to distinguish "pipe never completed" from "completed with
  // no payload" from "payload did not match the layout".
  uint32_t poll_calls;
  uint32_t pipe_completions;   // Generic-pipe transfers seen as completed
  uint32_t zero_len_completions;
  uint32_t resubmit_failures;
  uint32_t reports_logged;
  uint32_t parse_failures;
  uint32_t recovery_stage;     // Silent-endpoint recovery state machine
  uint64_t next_recovery_ms;
  uint64_t last_report_ms;
  uint32_t get_protocol_result;
  uint8_t get_protocol_value;
  uint32_t set_idle_result;

  // Last completions / recovery steps, raw (see /proc/usb_mouse).
  struct usb_mouse_event event_log[USB_MOUSE_EVENT_LOG];
  uint8_t event_head;
  uint8_t event_count;

  // Control-endpoint fallback.  Some receivers answer GET_REPORT while never
  // completing the interrupt transfer; after the silent-endpoint recovery
  // gives up, poll GET_REPORT and decode whatever comes back.
  bool control_poll_active;
  bool interrupt_proven; // Endpoint delivered at least one report ever
  uint32_t control_polls;
  uint32_t control_poll_failures;
  int8_t last_control_result;
  uint64_t next_control_poll_ms;
  uint8_t last_control_report[16];
  uint8_t last_control_len;
  bool have_last_control_report;

  // Absolute axis carry for touchpad-style devices
  int32_t abs_x;
  int32_t abs_y;
  bool abs_x_valid;
  bool abs_y_valid;

  // Interrupt transfer scheduling (UHCI path)
  struct uhci_td *int_td;
  uint32_t int_td_phys;
  struct uhci_qh *int_qh;
  uint32_t int_qh_phys;

  // Interrupt transfer scheduling (OHCI/EHCI paths)
  struct ohci_int_pipe *ohci_pipe;
  struct ehci_int_pipe *ehci_pipe;
  struct usb_interrupt_pipe *generic_pipe;

  bool active;
};

static struct usb_mouse_state mice[MAX_USB_MICE];
static int mouse_count = 0;
/* xHCI calls usb_mouse_poll() from both its IRQ and timer paths. With SMP the
 * timer watchdog may run on a different CPU while an IRQ is consuming the
 * same completion, so only one caller may inspect/resubmit each pipe at once.
 * This is a try-gate: a concurrent poll can be skipped and the next tick will
 * observe any completion that remains pending. */
static volatile uint8_t usb_mouse_polling;

// Report decoding

static uint32_t hid_extract_bits(const uint8_t *buf, uint32_t bit_offset,
                                 uint8_t bit_size) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < bit_size && i < 32; i++) {
    uint32_t bit = bit_offset + i;
    if (buf[bit >> 3] & (1u << (bit & 7)))
      value |= 1u << i;
  }
  return value;
}

static int32_t hid_field_value(const uint8_t *buf,
                               const struct hid_mouse_field *f,
                               uint32_t base_bits) {
  uint32_t raw =
      hid_extract_bits(buf, base_bits + f->bit_offset, f->bit_size);
  if (f->is_signed && f->bit_size > 0 && f->bit_size < 32) {
    uint32_t sign = 1u << (f->bit_size - 1);
    if (raw & sign)
      raw |= ~((1u << f->bit_size) - 1);
  }
  return (int32_t)raw;
}

struct hid_mouse_delta {
  int32_t dx;
  int32_t dy;
  int32_t wheel;
  int32_t pan;
  uint32_t buttons;
  bool valid;
};

// Extract one report's fields into a delta.  Returns false when the transfer
// is not the mouse report (unknown report ID) or is too short to hold it.
// True when a packet matches a layout's report ID and is long enough for that
// report.  Used to detect a device that ignored SET_PROTOCOL(boot) and kept
// sending its report-protocol format.
static bool hid_report_matches(const struct hid_mouse_layout *l,
                               const uint8_t *buf, uint16_t len) {
  if (!l || !l->valid || !buf || len == 0)
    return false;
  uint8_t id = 0;
  uint32_t base_bits = 0;
  if (l->has_report_id) {
    id = buf[0];
    base_bits = 8;
  }
  for (uint8_t i = 0; i < HID_MAX_REPORTS; i++) {
    if (!l->reports[i].used || l->reports[i].id != id)
      continue;
    return (uint32_t)len * 8 >= base_bits + l->reports[i].total_bits;
  }
  return false;
}

static uint16_t usb_mouse_layout_report_id(const struct hid_mouse_layout *l) {
  for (uint8_t r = 0; r < HID_MAX_REPORTS; r++)
    if (l->reports[r].used)
      return l->reports[r].id;
  return 0;
}

static bool usb_mouse_parse_report(struct usb_mouse_state *ms,
                                   const uint8_t *buf, uint16_t len,
                                   struct hid_mouse_delta *out) {
  memset(out, 0, sizeof(*out));
  if (!buf || len == 0)
    return false;

  /* Adapt to the format the device actually sends.  A boot-capable device
   * left in boot protocol by firmware sends 3/4-byte reports; one in report
   * protocol sends its leading report ID.  Detect both directions from the
   * packet instead of trusting a protocol request. */
  if (ms->using_boot) {
    if (ms->have_report_layout &&
        hid_report_matches(&ms->report_layout, buf, len)) {
      ms->layout = ms->report_layout;
      ms->using_boot = false;
      ms->boot_layout = false;
      klog_puts("[USB-MOUSE] Switching to report layout\n");
    }
  } else if (ms->boot_capable && len <= 4 &&
             !hid_report_matches(&ms->layout, buf, len)) {
    struct hid_mouse_layout boot;
    hid_build_boot_layout(&boot, ms->max_packet);
    ms->layout = boot;
    ms->using_boot = true;
    ms->boot_layout = true;
    klog_puts("[USB-MOUSE] Short reports: using boot layout\n");
  }

  const struct hid_mouse_layout *layout = &ms->layout;
  if (!layout->valid) {
    ms->parse_failures++;
    if (ms->parse_failures <= 4)
      usb_dbgf("[USB-MOUSE] report dropped: no valid layout (len=%u)\n", len);
    return false;
  }

  /* Remember the raw report for /proc/usb before any interpretation. */
  ms->last_report_len = len > sizeof(ms->last_report)
                            ? (uint8_t)sizeof(ms->last_report)
                            : (uint8_t)len;
  memcpy(ms->last_report, buf, ms->last_report_len);

  uint8_t report_id = 0;
  uint32_t base_bits = 0;
  if (layout->has_report_id) {
    report_id = buf[0];
    base_bits = 8;
  }

  const struct hid_mouse_report *rep = NULL;
  for (uint8_t i = 0; i < HID_MAX_REPORTS; i++) {
    if (layout->reports[i].used && layout->reports[i].id == report_id) {
      rep = &layout->reports[i];
      break;
    }
  }
  if (!rep || (uint32_t)len * 8 < base_bits + rep->total_bits) {
    ms->unknown_reports++;
    ms->parse_failures++;
    if (ms->unknown_reports <= 16) {
      klogf("[USB-MOUSE] Unrecognized report len=%u id=%u expected_id=%u "
            "expected_bits=%u has_report_id=%u\n",
            len, report_id,
            rep ? rep->id : usb_mouse_layout_report_id(layout),
            rep ? rep->total_bits : 0, layout->has_report_id ? 1 : 0);
      usb_debug_hexdump("[USB-MOUSE]   raw", buf, len, 16);
    }
    return false; // Not the mouse report, or a truncated transfer
  }

  for (uint8_t i = 0; i < rep->field_count; i++) {
    const struct hid_mouse_field *f = &rep->fields[i];
    /* Optional trailing fields (a wheel byte absent from a 3-byte boot
     * report) are skipped rather than read past the transfer. */
    if (base_bits + f->bit_offset + f->bit_size > (uint32_t)len * 8)
      continue;
    int32_t value = hid_field_value(buf, f, base_bits);
    switch (f->kind) {
    case HID_FIELD_X:
      if (f->is_absolute) {
        if (ms->abs_x_valid)
          out->dx += value - ms->abs_x;
        ms->abs_x = value;
        ms->abs_x_valid = true;
      } else {
        out->dx += value;
      }
      break;
    case HID_FIELD_Y:
      if (f->is_absolute) {
        if (ms->abs_y_valid)
          out->dy += value - ms->abs_y;
        ms->abs_y = value;
        ms->abs_y_valid = true;
      } else {
        out->dy += value;
      }
      break;
    case HID_FIELD_WHEEL:
      out->wheel += value;
      break;
    case HID_FIELD_PAN:
      out->pan += value;
      break;
    case HID_FIELD_BUTTON:
      if (value)
        out->buttons |= 1u << f->button_index;
      break;
    default:
      break;
    }
  }

  out->valid = true;
  return true;
}

static void usb_mouse_decode(struct usb_mouse_state *ms, const uint8_t *buf,
                             uint16_t len) {
  struct hid_mouse_delta delta;
  if (!usb_mouse_parse_report(ms, buf, len, &delta))
    return;

  ms->reports_decoded++;
  ms->last_report_ms = lapic_timer_get_ms();
  ms->reports_logged++;
  if (ms->reports_logged <= 16 || (ms->reports_logged & 0xFFU) == 0) {
    usb_dbgf("[USB-MOUSE] report #%u len=%u dx=%d dy=%d wheel=%d pan=%d "
          "buttons=0x%X layout=%s\n",
          ms->reports_logged, len, delta.dx, delta.dy, delta.wheel, delta.pan,
          delta.buttons, ms->using_boot ? "boot" : "report");
    usb_debug_hexdump("[USB-MOUSE]   raw", buf, len, 16);
  }

  mouse_apply_report(delta.dx, delta.dy, delta.wheel, delta.pan,
                     delta.buttons);
}

// UHCI interrupt transfer setup
//
// Uses QH pool indices 8+ to avoid conflicts with keyboard (1-4) and control
// transfers (0), and TD pool indices starting at 48+ (keyboard uses 32-35).

static void usb_mouse_setup_interrupt_xfer(struct usb_mouse_state *mouse) {
  struct uhci_controller *hc = mouse->hc;

  int qh_idx = 8 + (int)(mouse - mice);
  mouse->int_qh = &hc->qh_pool[qh_idx];
  mouse->int_qh_phys = hc->qh_pool_phys + (qh_idx * sizeof(struct uhci_qh));

  int td_idx = 48 + (int)(mouse - mice);
  mouse->int_td = &hc->td_pool[td_idx];
  mouse->int_td_phys = hc->td_pool_phys + (td_idx * sizeof(struct uhci_td));

  uint16_t max_len = mouse->max_packet ? mouse->max_packet - 1 : 7;
  if (max_len > 0x7FF)
    max_len = 0x7FF;

  mouse->int_td->link = TD_LINK_TERMINATE;
  mouse->int_td->status = TD_STATUS_ACTIVE | TD_STATUS_IOC | TD_STATUS_C_ERR;
  if (usb_speed_is_low(mouse->dev->speed))
    mouse->int_td->status |= TD_STATUS_LS;

  mouse->int_td->token = ((uint32_t)max_len << 21) |
                         ((uint32_t)mouse->data_toggle << 19) |
                         ((uint32_t)mouse->ep_number << 15) |
                         ((uint32_t)mouse->dev->address << 8) | TD_PID_IN;
  mouse->int_td->buffer = (uint32_t)mouse->report_buf_phys;

  mouse->int_qh->head = QH_LINK_TERMINATE;
  mouse->int_qh->element = mouse->int_td_phys;

  uint8_t interval = mouse->interval;
  if (interval == 0)
    interval = 8;
  if (interval > 128)
    interval = 128;

  uint8_t sched_interval = 1;
  while (sched_interval < interval && sched_interval < 128)
    sched_interval <<= 1;

  for (int i = 0; i < 1024; i += sched_interval) {
    mouse->int_qh->head = hc->frame_list[i];
    hc->frame_list[i] = mouse->int_qh_phys | TD_LINK_QH;
  }

  klog_puts("[USB-MOUSE] Interrupt transfer scheduled (interval=");
  klog_uint64(sched_interval);
  klog_puts("ms, endpoint=");
  klog_uint64(mouse->ep_number);
  klog_puts(")\n");
}

static void usb_mouse_resubmit_td(struct usb_mouse_state *mouse) {
  mouse->data_toggle ^= 1;

  uint16_t max_len = mouse->max_packet ? mouse->max_packet - 1 : 7;
  if (max_len > 0x7FF)
    max_len = 0x7FF;

  mouse->int_td->link = TD_LINK_TERMINATE;
  mouse->int_td->status = TD_STATUS_ACTIVE | TD_STATUS_IOC | TD_STATUS_C_ERR;
  if (usb_speed_is_low(mouse->dev->speed))
    mouse->int_td->status |= TD_STATUS_LS;

  mouse->int_td->token = ((uint32_t)max_len << 21) |
                         ((uint32_t)mouse->data_toggle << 19) |
                         ((uint32_t)mouse->ep_number << 15) |
                         ((uint32_t)mouse->dev->address << 8) | TD_PID_IN;
  mouse->int_td->buffer = (uint32_t)mouse->report_buf_phys;

  __asm__ volatile("mfence" ::: "memory");
  mouse->int_qh->element = mouse->int_td_phys;
  __asm__ volatile("mfence" ::: "memory");
}

// Probe & initialization

struct mouse_candidate {
  uint8_t iface_num;
  uint8_t ep_addr;
  uint8_t ep_number;
  uint16_t max_packet;
  uint8_t interval;
  uint8_t protocol;
  uint8_t subclass;
  uint8_t alt_setting;           // interface alternate setting with the EP
  bool boot_capable;             // subclass 1 + protocol 2
  bool layout_ok;                // report descriptor parsed into layout
  struct hid_mouse_layout layout; // valid once layout_ok
};

// Last probe failure, reported at /proc/usb.  Real hardware has no serial
// capture by default, so the reason a mouse was not claimed must survive boot.
static const char *mouse_last_probe_failure = "not probed";

// SET_IDLE(0,0): report only on change.  Linux's usbhid sends this right
// before reading the report descriptor; the device's interrupt reporting is
// unchanged without it, but matching the proven sequence removes a variable.
static int usb_mouse_set_idle(struct usb_device *dev, uint8_t iface) {
  struct usb_control_request req;
  req.request_type = 0x21; // Host-to-device, class, interface
  req.request = USB_REQ_SET_IDLE_M;
  req.value = 0; // duration 0, report ID 0
  req.index = iface;
  req.length = 0;
  return usb_control_transfer(dev, &req, NULL, 0);
}

// GET_PROTOCOL (HID 7.2.4).  A boot-capable interface answers 0 (boot) or 1
// (report).  The driver adapts either way, but the value is the single most
// useful fact when a mouse is silent: a device left in boot protocol and a
// device in report protocol need different report layouts.
static int usb_mouse_get_protocol(struct usb_device *dev, uint8_t iface,
                                  uint8_t *value) {
  uint8_t buf[2];
  memset(buf, 0, sizeof(buf));
  struct usb_control_request req;
  req.request_type = 0xA1; // Device-to-host, class, interface
  req.request = 0x03;      // GET_PROTOCOL
  req.value = 0;
  req.index = iface;
  req.length = 1;
  int result = usb_control_transfer(dev, &req, buf, 1);
  if (value)
    *value = buf[0];
  klogf("[USB-MOUSE] GET_PROTOCOL iface=%u -> res=%d value=%u\n", iface,
        result, buf[0]);
  return result;
}

// SET_PROTOCOL (HID 7.2.5): 0 = boot, 1 = report.  Used only by the silent
// endpoint recovery path, never during a normal probe.
static int usb_mouse_set_protocol(struct usb_device *dev, uint8_t iface,
                                  uint8_t protocol) {
  struct usb_control_request req;
  req.request_type = 0x21; // Host-to-device, class, interface
  req.request = USB_REQ_SET_PROTOCOL_M;
  req.value = protocol;
  req.index = iface;
  req.length = 0;
  int result = usb_control_transfer(dev, &req, NULL, 0);
  klogf("[USB-MOUSE] SET_PROTOCOL iface=%u protocol=%u -> res=%d\n", iface,
        protocol, result);
  return result;
}

// CLEAR_FEATURE(ENDPOINT_HALT).  Firmware or a previous OS hand-off can leave
// the interrupt endpoint halted; without this the pipe never completes.
static int usb_mouse_clear_halt(struct usb_device *dev, uint8_t ep_addr) {
  struct usb_control_request req;
  req.request_type = 0x02; // Host-to-device, standard, endpoint
  req.request = USB_REQ_CLEAR_FEATURE;
  req.value = 0; // ENDPOINT_HALT
  req.index = ep_addr;
  req.length = 0;
  return usb_control_transfer(dev, &req, NULL, 0);
}

bool usb_mouse_probe(struct usb_device *dev) {
  if (!dev || !dev->hcd)
    return false;

  klogf("[USB-MOUSE] driver build %s %s\n", __DATE__, __TIME__);
  klogf("[USB-MOUSE] probe begin addr=%u port=%u speed=%s hcd=%s vid=0x%04X "
        "pid=0x%04X class=0x%02X\n",
        dev->address, dev->port + 1, usb_speed_name(dev->speed),
        dev->hcd->name ? dev->hcd->name : "?", dev->desc.vendor_id,
        dev->desc.product_id, dev->desc.device_class);

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
  klogf("[USB-MOUSE] using state slot %d (mouse_count=%d)\n", state_index,
        mouse_count);

  // Only cast to uhci_controller if this device is actually on a UHCI HCD.
  struct uhci_controller *hc = NULL;
  for (int i = 0; i < uhci_get_controller_count(); i++) {
    struct uhci_controller *candidate = uhci_get_controller(i);
    if (candidate && dev->hcd->priv == candidate) {
      hc = candidate;
      break;
    }
  }

  // 1. Read the Configuration Descriptor
  uint8_t config_buf[256];
  struct usb_control_request req;
  req.request_type = 0x80;
  req.request = USB_REQ_GET_DESCRIPTOR;
  req.value = (USB_DESC_CONFIGURATION << 8) | 0;
  req.index = 0;
  req.length = 9;

  if (usb_control_transfer(dev, &req, config_buf, 9) < 0) {
    klog_puts("[USB-MOUSE] Failed to get config descriptor header\n");
    return false;
  }
  usb_debug_hexdump("[USB-MOUSE] config header", config_buf, 9, 9);

  struct usb_config_descriptor_m *cfg =
      (struct usb_config_descriptor_m *)config_buf;
  uint16_t total_len = cfg->total_length;
  if (total_len > sizeof(config_buf))
    total_len = sizeof(config_buf);
  klogf("[USB-MOUSE] config total_len=%u num_interfaces=%u config_value=%u "
        "attributes=0x%02X max_power=%u\n",
        cfg->total_length, cfg->num_interfaces, cfg->config_value,
        cfg->attributes, cfg->max_power);

  req.length = total_len;
  if (usb_control_transfer(dev, &req, config_buf, total_len) < 0) {
    klog_puts("[USB-MOUSE] Failed to get full config descriptor\n");
    return false;
  }
  usb_debug_hexdump("[USB-MOUSE] config bundle", config_buf, total_len, 96);

  // 2. Walk interfaces and collect HID mouse candidates with their interrupt
  //    IN endpoints.  The report descriptor is deliberately NOT fetched here:
  //    class requests to an unconfigured device STALL on some real hardware
  //    (Linux fetches report descriptors only after SET_CONFIGURATION).
  struct mouse_candidate candidates[HID_MAX_CANDIDATES];
  int candidate_count = 0;
  struct mouse_candidate *current = NULL;

  uint16_t offset = cfg->length;
  while (offset + 2 <= total_len) {
    uint8_t desc_len = config_buf[offset];
    uint8_t desc_type = config_buf[offset + 1];

    if (desc_len == 0)
      break;

    usb_dbgf("[USB-MOUSE-DBG] desc off=%u len=%u type=0x%02X\n", offset, desc_len,
          desc_type);

    if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
      struct usb_interface_descriptor_m *iface =
          (struct usb_interface_descriptor_m *)&config_buf[offset];
      current = NULL;

      klog_puts("[USB-MOUSE] Interface: class=0x");
      klog_hex32(iface->interface_class);
      klog_puts(" subclass=0x");
      klog_hex32(iface->interface_subclass);
      klog_puts(" protocol=0x");
      klog_hex32(iface->interface_protocol);
      klog_puts("\n");
      usb_dbgf("[USB-MOUSE-DBG]   iface num=%u alt=%u endpoints=%u\n",
            iface->interface_number, iface->alternate_setting,
            iface->num_endpoints);

      if (iface->interface_class == USB_CLASS_HID &&
          iface->interface_protocol != USB_PROTOCOL_KEYBOARD &&
          candidate_count < HID_MAX_CANDIDATES) {
        struct mouse_candidate *cand = &candidates[candidate_count];
        memset(cand, 0, sizeof(*cand));
        cand->iface_num = iface->interface_number;
        cand->protocol = iface->interface_protocol;
        cand->subclass = iface->interface_subclass;
        cand->alt_setting = iface->alternate_setting;
        cand->boot_capable = iface->interface_protocol == USB_PROTOCOL_MOUSE &&
                             iface->interface_subclass == USB_SUBCLASS_BOOT;
        current = cand;
        candidate_count++;
        usb_dbgf("[USB-MOUSE-DBG]   candidate#%d iface=%u protocol=%u "
              "subclass=%u alt=%u boot_capable=%u\n",
              candidate_count - 1, cand->iface_num, cand->protocol,
              cand->subclass, cand->alt_setting, cand->boot_capable ? 1 : 0);
      } else {
        usb_dbgf("[USB-MOUSE-DBG]   iface not a mouse candidate (class=%u "
              "protocol=%u candidates=%d)\n",
              iface->interface_class, iface->interface_protocol,
              candidate_count);
      }
    } else if (desc_type == USB_DESC_ENDPOINT && desc_len >= 7 && current &&
               current->ep_addr == 0) {
      struct usb_endpoint_descriptor_m *ep =
          (struct usb_endpoint_descriptor_m *)&config_buf[offset];

      // Only want Interrupt IN endpoints
      if ((ep->attributes & 0x03) == 0x03 && (ep->endpoint_address & 0x80)) {
        current->ep_addr = ep->endpoint_address;
        current->ep_number = ep->endpoint_address & 0x0F;
        current->max_packet = ep->max_packet_size;
        current->interval = ep->interval;

        klog_puts("[USB-MOUSE] Interrupt IN endpoint: addr=0x");
        klog_hex32(current->ep_addr);
        klog_puts(" maxpkt=");
        klog_uint64(current->max_packet);
        klog_puts(" interval=");
        klog_uint64(current->interval);
        klog_puts("\n");
      } else {
        usb_dbgf("[USB-MOUSE-DBG]   ignored endpoint addr=0x%02X attr=0x%02X "
              "maxpkt=%u interval=%u\n",
              ep->endpoint_address, ep->attributes, ep->max_packet_size,
              ep->interval);
      }
    }

    offset += desc_len;
  }

  // Keep only candidates with a usable endpoint.
  int usable = 0;
  for (int i = 0; i < candidate_count; i++) {
    if (!candidates[i].ep_addr || !candidates[i].max_packet) {
      usb_dbgf("[USB-MOUSE-DBG] drop candidate iface=%u ep=0x%02X maxpkt=%u "
            "(no usable interrupt IN)\n",
            candidates[i].iface_num, candidates[i].ep_addr,
            candidates[i].max_packet);
      continue;
    }
    if (candidates[i].max_packet > 1024)
      candidates[i].max_packet = 1024;
    if (usable != i)
      candidates[usable] = candidates[i];
    usable++;
  }
  candidate_count = usable;
  if (!candidate_count) {
    mouse_last_probe_failure = "no HID mouse interface with interrupt IN";
    klog_puts("[USB-MOUSE] No HID mouse interface found\n");
    return false;
  }
  usb_dbgf("[USB-MOUSE-DBG] %d usable candidate(s)\n", candidate_count);

  // 3. SET_CONFIGURATION before touching interface class requests.
  if (!dev->configured || dev->configuration_value != cfg->config_value) {
    req.request_type = 0x00;
    req.request = USB_REQ_SET_CONFIGURATION;
    req.value = cfg->config_value;
    req.index = 0;
    req.length = 0;

    if (usb_control_transfer(dev, &req, NULL, 0) < 0) {
      mouse_last_probe_failure = "SET_CONFIGURATION failed";
      klog_puts("[USB-MOUSE] SET_CONFIGURATION failed\n");
      return false;
    }
    dev->configuration_value = cfg->config_value;
    dev->configured = true;
    usb_dbgf("[USB-MOUSE-DBG] SET_CONFIGURATION value=%u done\n",
          cfg->config_value);
  } else {
    usb_dbgf("[USB-MOUSE-DBG] already configured value=%u\n",
          dev->configuration_value);
  }

  // Small settle delay
  for (int i = 0; i < 5000; i++)
    io_wait();

  // 4. Fetch and parse each candidate's report descriptor.  Retry once: a
  //    device that just changed configuration can NAK/STALL the first
  //    request.  No class requests are issued first: the Logitech receiver
  //    STALLs SET_IDLE, and a STALL on the control endpoint makes the
  //    following descriptor fetch fail too.  1024 bytes covers the large
  //    descriptors gaming mice ship; the parse stops at the descriptor end.
  for (int i = 0; i < candidate_count; i++) {
    struct mouse_candidate *cand = &candidates[i];
    uint8_t desc[1024];
    int res = -1;
    for (int attempt = 0; attempt < 2 && res < 0; attempt++) {
      usb_dbgf("[USB-MOUSE-DBG] report descriptor fetch iface=%u attempt=%d\n",
            cand->iface_num, attempt + 1);
      res = usb_hid_get_report_descriptor(dev, cand->iface_num, desc,
                                          sizeof(desc));
      if (res < 0)
        for (int j = 0; j < 2000; j++)
          io_wait();
    }
    if (res < 0) {
      klog_puts("[USB-MOUSE] Report descriptor fetch failed for iface ");
      klog_uint64(cand->iface_num);
      klog_puts("\n");
      continue;
    }
    usb_debug_hexdump("[USB-MOUSE-DBG] report descriptor", desc, 96, 96);
    cand->layout_ok = hid_parse_mouse_layout(desc, sizeof(desc), &cand->layout);
    usb_dbgf("[USB-MOUSE-DBG] parse iface=%u -> %s (reports=%u buttons=%u "
          "report_id=%u dropped=%u)\n",
          cand->iface_num, cand->layout_ok ? "ok" : "no mouse collection",
          cand->layout.report_count, cand->layout.button_count,
          cand->layout.has_report_id ? 1 : 0, cand->layout.dropped_fields);
    if (!cand->layout_ok)
      klog_puts("[USB-MOUSE] No mouse collection in report descriptor\n");
  }

  // 5. Pick a candidate: a decodable layout wins; otherwise a boot-capable
  //    interface can still be driven with the synthesized boot layout.
  //    Boot-capable interfaces are preferred because they can fall back.
  int best = -1;
  for (int i = 0; i < candidate_count; i++) {
    if (!candidates[i].layout_ok)
      continue;
    if (best < 0 || (candidates[i].boot_capable && !candidates[best].boot_capable))
      best = i;
  }
  if (best < 0) {
    for (int i = 0; i < candidate_count; i++) {
      if (!candidates[i].boot_capable)
        continue;
      best = i;
      break;
    }
  }
  if (best < 0) {
    mouse_last_probe_failure = "no decodable report layout";
    klog_puts("[USB-MOUSE] No decodable report layout\n");
    return false;
  }
  struct mouse_candidate *chosen = &candidates[best];
  usb_dbgf("[USB-MOUSE-DBG] chosen candidate#%d iface=%u ep=0x%02X maxpkt=%u "
        "interval=%u protocol=%u subclass=%u boot_capable=%u layout_ok=%u\n",
        best, chosen->iface_num, chosen->ep_addr, chosen->max_packet,
        chosen->interval, chosen->protocol, chosen->subclass,
        chosen->boot_capable ? 1 : 0, chosen->layout_ok ? 1 : 0);

  klog_puts("[USB-MOUSE] HID mouse detected (iface=");
  klog_uint64(chosen->iface_num);
  klog_puts(", protocol=");
  klog_uint64(chosen->protocol);
  klog_puts(", layout=");
  klog_puts(chosen->layout_ok ? "report" : "boot-fallback");
  klog_puts(")\n");

  // 6. Do NOT send SET_PROTOCOL.  Linux's usbhid never sends it, and the
  //    Logitech Unifying receiver's mouse interface stops forwarding reports
  //    when a protocol request is issued on the device.  The bus reset in
  //    enumeration leaves the device in report protocol, so the parsed report
  //    layout is correct; if firmware left it in boot protocol the decoder
  //    notices the short reports and switches automatically.  (SET_IDLE(0,0)
  //    was already sent before the report descriptor fetch, like Linux.)
  struct hid_mouse_layout report_layout = chosen->layout;
  struct hid_mouse_layout layout = report_layout;
  bool using_boot = false;

  if (!layout.valid && chosen->boot_capable) {
    hid_build_boot_layout(&layout, chosen->max_packet);
    using_boot = true;
    klog_puts("[USB-MOUSE] Using boot protocol fallback layout\n");
  }

  if (!layout.valid) {
    mouse_last_probe_failure = "no usable layout";
    klog_puts("[USB-MOUSE] No usable report layout\n");
    return false;
  }

  // 6b. SET_IDLE(0,0) *after* the report descriptor has been read.  The
  //     receiver STALLs this request, so issuing it first would poison the
  //     control endpoint for the descriptor fetch.  Its failure is harmless:
  //     idle only controls repeat reports, not movement.
  int set_idle_result = usb_mouse_set_idle(dev, chosen->iface_num);
  usb_dbgf("[USB-MOUSE-DBG] SET_IDLE iface=%u -> res=%d\n", chosen->iface_num,
        set_idle_result);

  // 6b2. GET_PROTOCOL: 0 = boot, 1 = report.  Purely diagnostic, but it is
  //      the first thing to check when reports do not match the layout.
  uint8_t protocol_value = 0xFF;
  int get_protocol_result =
      usb_mouse_get_protocol(dev, chosen->iface_num, &protocol_value);

  // 6c. Ask the device for its current input report.  This is the decisive
  //     test for a silent interrupt endpoint: a receiver with a live mouse
  //     answers here even while the interrupt pipe is quiet, while one with
  //     no active mouse returns zeros or STALLs.  The result is reported at
  //     /proc/usb.  Use the parsed report ID: report ID 0 is invalid on a
  //     device that only declares report ID 2, and the guaranteed STALL would
  //     poison EP0 for every request that follows.
  uint16_t get_report_id = 0;
  for (uint8_t r = 0; r < HID_MAX_REPORTS; r++) {
    if (layout.reports[r].used) {
      get_report_id = layout.reports[r].id;
      break;
    }
  }
  uint8_t get_report[16];
  memset(get_report, 0, sizeof(get_report));
  struct usb_control_request greq;
  greq.request_type = 0xA1; // IN | class | interface
  greq.request = 0x01;      // GET_REPORT
  greq.value = 0x0100 | get_report_id; // Input report, parsed report ID
  greq.index = chosen->iface_num;
  greq.length = sizeof(get_report);
  usb_dbgf("[USB-MOUSE-DBG] GET_REPORT(input) iface=%u report_id=%u\n",
        chosen->iface_num, get_report_id);
  int8_t get_report_result =
      (int8_t)usb_control_transfer(dev, &greq, get_report, sizeof(get_report));

  // 8. Some devices put the interrupt endpoint in a non-default alternate
  //    setting; without SET_INTERFACE the pipe would be configured but the
  //    device would never report.
  if (chosen->alt_setting != 0) {
    struct usb_control_request alt_req;
    alt_req.request_type = 0x01; // Host-to-device, standard, interface
    alt_req.request = 0x0B;      // SET_INTERFACE
    alt_req.value = chosen->alt_setting;
    alt_req.index = chosen->iface_num;
    alt_req.length = 0;
    if (usb_control_transfer(dev, &alt_req, NULL, 0) < 0)
      klog_puts("[USB-MOUSE] SET_INTERFACE failed (non-fatal)\n");
  } else {
    usb_dbg_puts("[USB-MOUSE-DBG] SET_INTERFACE skipped (alt=0)\n");
  }

  hid_log_layout(&layout);

  // 9. Allocate the DMA report buffer.  Keep it below 4GB: 32-bit-only
  //    controllers (and the UHCI fallback) would otherwise DMA to a
  //    truncated address.
  uint64_t phys;
  void *buf = dma_alloc_page_flags(DMA_FLAG_32BIT, &phys);
  if (!buf) {
    mouse_last_probe_failure = "DMA buffer allocation failed";
    klog_puts("[USB-MOUSE] Failed to allocate DMA buffer\n");
    return false;
  }
  memset(buf, 0, 4096);
  usb_dbgf("[USB-MOUSE-DBG] DMA report buffer virt=%p phys=0x%llX\n", buf,
        (unsigned long long)phys);

  // 10. Fill in the mouse state.
  struct usb_mouse_state *ms = &mice[state_index];
  memset(ms, 0, sizeof(*ms));
  ms->dev = dev;
  ms->hc = hc;
  ms->ep_addr = chosen->ep_addr;
  ms->ep_number = chosen->ep_number;
  ms->max_packet = chosen->max_packet;
  ms->interval = chosen->interval;
  ms->data_toggle = 0;
  ms->interface_number = chosen->iface_num;
  ms->report_buf = buf;
  ms->report_buf_phys = phys;
  ms->layout = layout;
  ms->report_layout = report_layout;
  ms->have_report_layout = report_layout.valid;
  ms->boot_capable = chosen->boot_capable;
  ms->using_boot = using_boot;
  ms->boot_layout = !chosen->layout_ok || using_boot;
  ms->halt_result = 0; // updated below if a recovery attempt is made
  ms->transport = "none";
  ms->attach_ms = lapic_timer_get_ms();
  ms->next_recovery_ms = ms->attach_ms + 3000;
  ms->get_report_result = get_report_result;
  memcpy(ms->get_report, get_report, sizeof(ms->get_report));
  ms->get_protocol_result = (uint32_t)get_protocol_result;
  ms->get_protocol_value = protocol_value;
  ms->set_idle_result = (uint32_t)set_idle_result;
  ms->active = true;

  klog_puts("[USB-MOUSE] GET_REPORT(input) ");
  if (get_report_result < 0) {
    klog_puts("failed - device returned no data\n");
  } else {
    klog_puts("ok\n");
  }
  usb_debug_hexdump("[USB-MOUSE-DBG] get_report bytes", ms->get_report,
                    sizeof(ms->get_report), sizeof(ms->get_report));

  // 11. Start the interrupt transfer.  If opening the pipe fails, try one
  //     recovery pass that clears a stale endpoint halt (firmware or a
  //     previous driver can leave one behind) and open again.  Configure
  //     Endpoint normally resets the endpoint, so this is only a fallback.
  int8_t halt_result = 0;
  usb_dbgf("[USB-MOUSE-DBG] opening interrupt pipe ep=0x%02X maxpkt=%u "
        "interval=%u\n",
        ms->ep_addr, ms->max_packet, ms->interval);
  ms->generic_pipe = usb_interrupt_open(
      dev, ms->ep_addr, ms->max_packet, ms->interval, ms->report_buf,
      ms->report_buf_phys);
  if (!ms->generic_pipe) {
    usb_dbg_puts("[USB-MOUSE-DBG] pipe open failed, clearing endpoint halt\n");
    halt_result = (int8_t)usb_mouse_clear_halt(dev, chosen->ep_addr);
    ms->halt_result = halt_result;
    usb_dbgf("[USB-MOUSE-DBG] CLEAR_FEATURE(HALT) ep=0x%02X -> %d\n",
          chosen->ep_addr, halt_result);
    if (halt_result >= 0) {
      ms->generic_pipe = usb_interrupt_open(
          dev, ms->ep_addr, ms->max_packet, ms->interval, ms->report_buf,
          ms->report_buf_phys);
      usb_dbgf("[USB-MOUSE-DBG] reopen after halt clear -> %s\n",
            ms->generic_pipe ? "ok" : "FAIL");
    }
  }
  if (ms->generic_pipe) {
    ms->transport = "generic";
    klog_puts("[USB-MOUSE] Generic HCD interrupt pipe active\n");
  } else if (ms->hc != NULL) {
    ms->transport = "uhci";
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
        if (ms->ehci_pipe) {
          ms->transport = "ehci";
          pipe_found = true;
        } else
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
          if (ms->ohci_pipe) {
            ms->transport = "ohci";
            pipe_found = true;
          } else
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
  klog_puts(", iface=");
  klog_uint64(ms->interface_number);
  klog_puts(", transport=");
  klog_puts(ms->transport);
  klog_puts(")\n");
  usb_dbgf("[USB-MOUSE-DBG] attached addr=%u ep=0x%02X dci=%u iface=%u "
        "maxpkt=%u interval=%u layout=%s reports=%u buttons=%u "
        "report_id=%u boot_capable=%u using_boot=%u protocol=%u "
        "get_report=%d set_idle=%d\n",
        dev->address, ms->ep_addr, ms->ep_number * 2 + 1, ms->interface_number,
        ms->max_packet, ms->interval, ms->using_boot ? "boot" : "report",
        ms->layout.report_count, ms->layout.button_count,
        ms->layout.has_report_id ? 1 : 0, ms->boot_capable ? 1 : 0,
        ms->using_boot ? 1 : 0, protocol_value, get_report_result,
        set_idle_result);

  mouse_last_probe_failure = "none";
  return true;
}

void usb_mouse_disconnect(struct usb_device *dev) {
  for (int i = 0; i < mouse_count; i++) {
    struct usb_mouse_state *ms = &mice[i];
    if (!ms->active || ms->dev != dev)
      continue;
    usb_dbgf("[USB-MOUSE-DBG] disconnect state=%d addr=%u ep=0x%02X "
          "completions=%u reports=%u unknown=%u\n",
          i, dev->address, ms->ep_addr, ms->pipe_completions,
          ms->reports_decoded, ms->unknown_reports);
    if (ms->generic_pipe)
      usb_interrupt_cancel(ms->generic_pipe);
    ms->generic_pipe = NULL;
    ms->active = false;
    ms->dev = NULL;
    klog_puts("[USB-MOUSE] Mouse detached\n");
  }
}

// Polling
//
// Called from the HCD IRQ handlers (and the xHCI watchdog) to decode one
// completed report per mouse and resubmit its interrupt transfer.

static void usb_mouse_poll_pipe(struct usb_mouse_state *ms, const void *buf,
                                uint16_t len) {
  if (!buf || len == 0)
    return;
  usb_mouse_decode(ms, (const uint8_t *)buf, len);
}

// Silent-endpoint recovery
//
// A configured interrupt endpoint that never completes a transfer is NAKing:
// the device has nothing to send (mouse asleep, off, unpaired, wrong protocol)
// or the endpoint is stuck.  Escalate through the cheap recovery steps once
// every few seconds, logging each one, so a single boot on real hardware shows
// exactly which step (if any) brings the mouse to life.

static void usb_mouse_log_event(struct usb_mouse_state *ms, uint8_t code,
                                uint16_t len, const uint8_t *data) {
  struct usb_mouse_event *ev = &ms->event_log[ms->event_head];
  ev->ms = (uint32_t)lapic_timer_get_ms();
  ev->code = code;
  ev->len = len > sizeof(ev->data) ? (uint8_t)sizeof(ev->data) : (uint8_t)len;
  if (data && ev->len)
    memcpy(ev->data, data, ev->len);
  else
    memset(ev->data, 0, sizeof(ev->data));
  ms->event_head = (uint8_t)((ms->event_head + 1) % USB_MOUSE_EVENT_LOG);
  if (ms->event_count < USB_MOUSE_EVENT_LOG)
    ms->event_count++;
}

static uint16_t usb_mouse_report_bytes(const struct hid_mouse_layout *l) {
  if (!l->valid)
    return 0;
  uint16_t id = usb_mouse_layout_report_id(l);
  for (uint8_t r = 0; r < HID_MAX_REPORTS; r++) {
    if (l->reports[r].used && l->reports[r].id == id) {
      uint16_t bytes = (uint16_t)((l->reports[r].total_bits + 7) / 8);
      if (l->has_report_id)
        bytes++;
      return bytes;
    }
  }
  return 0;
}

// Ask the device for its current input report over the control endpoint.
// This is the fallback for a receiver whose interrupt endpoint stays silent
// even though GET_REPORT works (see /proc/usb_mouse's get_report bytes).
static void usb_mouse_control_poll(struct usb_mouse_state *ms) {
  if (!ms->active || !ms->dev)
    return;
  uint8_t report[16];
  memset(report, 0, sizeof(report));
  uint16_t id = usb_mouse_layout_report_id(&ms->layout);
  struct usb_control_request req;
  req.request_type = 0xA1; // IN | class | interface
  req.request = 0x01;      // GET_REPORT
  req.value = 0x0100 | id; // Input report, current layout's report ID
  req.index = ms->interface_number;
  req.length = sizeof(report);
  int res = usb_control_transfer(ms->dev, &req, report, sizeof(report));
  ms->control_polls++;
  ms->last_control_result = (int8_t)res;
  if (res < 0) {
    ms->control_poll_failures++;
    if (ms->control_poll_failures <= 3)
      klogf("[USB-MOUSE] control poll failed (%d), failure %u\n", res,
            ms->control_poll_failures);
    return;
  }
  ms->control_poll_failures = 0;

  /* Use the longer of the boot and report layouts so the decoder's
   * report-format auto-detect sees the real report even if the active layout
   * was switched to boot by the recovery. */
  uint16_t len = usb_mouse_report_bytes(&ms->layout);
  if (ms->have_report_layout) {
    uint16_t report_len = usb_mouse_report_bytes(&ms->report_layout);
    if (report_len > len)
      len = report_len;
  }
  if (len == 0 || len > sizeof(report))
    return;
  /* A device that keeps returning the same relative-delta report would make
   * the cursor drift if replayed on every poll; skip exact repeats. */
  if (ms->have_last_control_report && len == ms->last_control_len &&
      memcmp(report, ms->last_control_report, len) == 0)
    return;
  memcpy(ms->last_control_report, report, len);
  ms->last_control_len = (uint8_t)len;
  ms->have_last_control_report = true;
  usb_mouse_decode(ms, report, len);
}

static void usb_mouse_recover_silent(struct usb_mouse_state *ms) {
  if (!ms->active || !ms->dev)
    return;
  uint64_t now = lapic_timer_get_ms();
  if (!ms->next_recovery_ms)
    ms->next_recovery_ms = ms->attach_ms + 3000;
  if (now < ms->next_recovery_ms)
    return;
  struct usb_device *dev = ms->dev;
  usb_mouse_log_event(
      ms, (uint8_t)(0xF0 + (ms->recovery_stage > 3 ? 3 : ms->recovery_stage)),
      0, NULL);

  switch (ms->recovery_stage) {
  case 0: {
    klogf("[USB-MOUSE] recovery 1/4: no reports after %llu ms, clearing "
          "endpoint halt ep=0x%02X\n",
          (unsigned long long)(now - ms->attach_ms), ms->ep_addr);
    int res = usb_mouse_clear_halt(dev, ms->ep_addr);
    klogf("[USB-MOUSE] recovery 1/4: CLEAR_FEATURE(HALT) -> %d\n", res);
    ms->recovery_stage = 1;
    ms->next_recovery_ms = now + 3000;
    break;
  }
  case 1: {
    klogf("[USB-MOUSE] recovery 2/4: reopening interrupt pipe ep=0x%02X\n",
          ms->ep_addr);
    if (ms->generic_pipe) {
      usb_interrupt_cancel(ms->generic_pipe);
      ms->generic_pipe = NULL;
    }
    ms->generic_pipe = usb_interrupt_open(
        dev, ms->ep_addr, ms->max_packet, ms->interval, ms->report_buf,
        ms->report_buf_phys);
    klogf("[USB-MOUSE] recovery 2/4: reopen -> %s\n",
          ms->generic_pipe ? "ok" : "FAIL");
    ms->recovery_stage = 2;
    ms->next_recovery_ms = now + 3000;
    break;
  }
  case 2: {
    if (ms->boot_capable) {
      klogf("[USB-MOUSE] recovery 3/4: switching iface=%u to boot protocol\n",
            ms->interface_number);
      int res = usb_mouse_set_protocol(dev, ms->interface_number,
                                       HID_PROTOCOL_BOOT_M);
      if (res >= 0) {
        struct hid_mouse_layout boot;
        hid_build_boot_layout(&boot, ms->max_packet);
        ms->layout = boot;
        ms->using_boot = true;
        ms->boot_layout = true;
      }
      klogf("[USB-MOUSE] recovery 3/4: SET_PROTOCOL(boot) -> %d layout=%s\n",
            res, ms->using_boot ? "boot" : "report");
    } else {
      klog_puts("[USB-MOUSE] recovery 3/4: not boot-capable, skipping "
                "SET_PROTOCOL\n");
    }
    ms->recovery_stage = 3;
    ms->next_recovery_ms = now + 3000;
    break;
  }
  default: {
    if (!ms->silent_warned) {
      ms->silent_warned = true;
      uint8_t protocol = 0xFF;
      usb_mouse_get_protocol(dev, ms->interface_number, &protocol);
      uint8_t report[16];
      memset(report, 0, sizeof(report));
      uint16_t id = usb_mouse_layout_report_id(&ms->layout);
      struct usb_control_request req;
      req.request_type = 0xA1;
      req.request = 0x01;
      req.value = 0x0100 | id;
      req.index = ms->interface_number;
      req.length = sizeof(report);
      int res = usb_control_transfer(dev, &req, report, sizeof(report));
      klogf("[USB-MOUSE] recovery 4/4: still silent after %llu ms; "
            "GET_PROTOCOL=%u GET_REPORT(id=%u)=%d\n",
            (unsigned long long)(now - ms->attach_ms), protocol, id, res);
      usb_debug_hexdump("[USB-MOUSE] recovery GET_REPORT", report,
                        sizeof(report), sizeof(report));
      klog_puts("[USB-MOUSE] endpoint is NAKing: mouse asleep, off, "
                "unpaired, or the receiver is not forwarding its reports\n");
    }
    ms->next_recovery_ms = now + 30000;
    break;
  }
  }
}

void usb_mouse_poll(void) {
  if (__atomic_test_and_set(&usb_mouse_polling, __ATOMIC_ACQUIRE))
    return;

  for (int i = 0; i < mouse_count; i++) {
    struct usb_mouse_state *ms = &mice[i];
    if (!ms->active)
      continue;
    if (ms->generic_pipe) {
      ms->poll_calls++;
      if (!usb_interrupt_completed(ms->generic_pipe)) {
        /* No completion: the endpoint is NAKing or stuck.  Drive the silent
         * endpoint recovery state machine and keep polling. */
        usb_mouse_recover_silent(ms);
        uint64_t now = lapic_timer_get_ms();
        /* The recovery steps have all run and the interrupt endpoint has
         * never delivered a single report: fall back to control-endpoint
         * polling.  Once the endpoint proves it works (one report), silence
         * just means the device is idle and the fallback stays off. */
        if (!ms->control_poll_active && !ms->interrupt_proven &&
            ms->recovery_stage >= 3 && ms->attach_ms &&
            now - ms->attach_ms > 10000) {
          ms->control_poll_active = true;
          ms->next_control_poll_ms = now;
          klog_puts("[USB-MOUSE] interrupt endpoint still silent; polling "
                    "GET_REPORT as a fallback\n");
        }
        if (ms->control_poll_active && now >= ms->next_control_poll_ms) {
          usb_mouse_control_poll(ms);
          /* 50 Hz while the device answers, 1 Hz after a failure so a
           * misbehaving device cannot stall the timer for long. */
          ms->next_control_poll_ms =
              now + (ms->control_poll_failures ? 1000 : 20);
        }
        continue;
      }
      ms->pipe_completions++;
      if (!ms->interrupt_proven) {
        ms->interrupt_proven = true;
        klog_puts("[USB-MOUSE] interrupt endpoint delivering reports\n");
      }
      if (ms->control_poll_active) {
        ms->control_poll_active = false;
        ms->have_last_control_report = false;
        klog_puts("[USB-MOUSE] interrupt endpoint recovered; stopping "
                  "GET_REPORT polling\n");
      }
      uint16_t raw_len = ms->generic_pipe->actual_length;
      uint16_t len = raw_len ? raw_len : ms->generic_pipe->buffer_len;
      usb_mouse_log_event(ms, ms->generic_pipe->last_completion, raw_len,
                          ms->generic_pipe->buffer);
      if (raw_len == 0)
        ms->zero_len_completions++;
      if (ms->pipe_completions <= 16) {
        usb_dbgf("[USB-MOUSE-DBG] completion ep=0x%02X code=%u actual=%u "
              "buflen=%u using=%u\n",
              ms->ep_addr, ms->generic_pipe->last_completion, raw_len,
              ms->generic_pipe->buffer_len, len);
        usb_debug_hexdump("[USB-MOUSE-DBG]   buffer",
                          ms->generic_pipe->buffer, len, 16);
      }
      usb_mouse_poll_pipe(ms, ms->generic_pipe->buffer, len);
      if (usb_interrupt_resubmit(ms->generic_pipe) < 0)
        ms->resubmit_failures++;
      continue;
    }

    // EHCI path
    if (ms->ehci_pipe) {
      if (!ehci_int_pipe_completed(ms->ehci_pipe))
        continue;
      usb_mouse_poll_pipe(ms, ms->ehci_pipe->data_buf, ms->max_packet);
      ehci_int_pipe_resubmit(ms->ehci_pipe);
      continue;
    }

    // OHCI path
    if (ms->ohci_pipe) {
      if (!ohci_int_pipe_completed(ms->ohci_pipe))
        continue;
      usb_mouse_poll_pipe(ms, ms->ohci_pipe->data_buf, ms->max_packet);
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
    if (actual_len >= 3 && actual_len <= ms->max_packet)
      usb_mouse_poll_pipe(ms, ms->report_buf, (uint16_t)actual_len);

    usb_mouse_resubmit_td(ms);
  }

  __atomic_clear(&usb_mouse_polling, __ATOMIC_RELEASE);
}

// Layout self-test
//
// Compiled in with `make USB_MOUSE_SELFTEST=1`.  Parses report descriptors
// from the three device classes the driver has to support and decodes sample
// reports through the same code path the interrupt handlers use, so the
// parser/decoder can be exercised without the corresponding hardware.

#if defined(USB_MOUSE_SELFTEST) && USB_MOUSE_SELFTEST

static int usb_mouse_selftest_failures;

static void usb_mouse_selftest_check(const char *name, bool ok) {
  klog_puts(ok ? "[USB-MOUSE-TEST] PASS " : "[USB-MOUSE-TEST] FAIL ");
  klog_puts(name);
  klog_puts("\n");
  if (!ok)
    usb_mouse_selftest_failures++;
}

// Boot-protocol mouse: 3 buttons, 8-bit X/Y, wheel, no report ID.
static const uint8_t hid_test_boot_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xC0, 0xC0};

// Report-protocol gaming mouse: Report ID 1, 16-bit X/Y, 8 buttons,
// wheel and AC pan.
static const uint8_t hid_test_gaming_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08,
    0x75, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00,
    0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0x09, 0x39,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0};

// Relative touchpad: absolute X/Y with logical range 0..32767.
static const uint8_t hid_test_touchpad_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81,
    0x02, 0xC0, 0xC0};

// Logitech Unifying receiver mouse report: Report ID 2, 16 buttons declared
// before the axes, 12-bit X/Y, wheel and AC pan.  The 16 buttons alone used
// to fill the parser's field table and silently drop X/Y, leaving a pointer
// that could never move.
static const uint8_t hid_test_receiver_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10,
    0x75, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01,
    0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0x09, 0x39,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0};

static void usb_mouse_selftest_layout(const char *name, const uint8_t *desc,
                                      uint32_t len, bool expect_report_id,
                                      uint8_t expect_buttons) {
  struct hid_mouse_layout layout;
  bool parsed = hid_parse_mouse_layout(desc, len, &layout);
  usb_mouse_selftest_check(name, parsed &&
                                     layout.has_report_id == expect_report_id &&
                                     layout.button_count == expect_buttons &&
                                     layout.dropped_fields == 0);
  if (parsed)
    hid_log_layout(&layout);
}

void usb_mouse_selftest(void) {
  usb_mouse_selftest_failures = 0;
  klog_puts("[USB-MOUSE-TEST] Running HID mouse parser self-test\n");

  usb_mouse_selftest_layout("boot layout", hid_test_boot_descriptor,
                            sizeof(hid_test_boot_descriptor), false, 3);
  usb_mouse_selftest_layout("gaming layout", hid_test_gaming_descriptor,
                            sizeof(hid_test_gaming_descriptor), true, 8);
  usb_mouse_selftest_layout("touchpad layout", hid_test_touchpad_descriptor,
                            sizeof(hid_test_touchpad_descriptor), false, 3);
  usb_mouse_selftest_layout("receiver layout", hid_test_receiver_descriptor,
                            sizeof(hid_test_receiver_descriptor), true, 16);

  // Decode a boot report: buttons=0x01, X=+5, Y=-3, wheel=+1.
  struct usb_mouse_state ms;
  memset(&ms, 0, sizeof(ms));
  hid_build_boot_layout(&ms.layout, 4);
  const uint8_t boot_report[4] = {0x01, 0x05, 0xFD, 0x01};
  struct hid_mouse_delta delta;
  bool ok = usb_mouse_parse_report(&ms, boot_report, sizeof(boot_report),
                                   &delta) &&
            delta.dx == 5 && delta.dy == -3 && delta.wheel == 1 &&
            delta.buttons == 0x01;
  usb_mouse_selftest_check("boot report decode", ok);

  // A 3-byte boot report (no wheel byte) must still decode.
  memset(&ms, 0, sizeof(ms));
  hid_build_boot_layout(&ms.layout, 20);
  const uint8_t boot3_report[3] = {0x01, 0x05, 0xFD};
  ok = usb_mouse_parse_report(&ms, boot3_report, sizeof(boot3_report),
                              &delta) &&
       delta.dx == 5 && delta.dy == -3 && delta.wheel == 0 &&
       delta.buttons == 0x01;
  usb_mouse_selftest_check("3-byte boot report decode", ok);

  // A boot-capable device that ignored SET_PROTOCOL(boot) keeps sending its
  // report layout; the driver must notice the report ID and switch back.
  memset(&ms, 0, sizeof(ms));
  hid_parse_mouse_layout(hid_test_receiver_descriptor,
                         sizeof(hid_test_receiver_descriptor),
                         &ms.report_layout);
  ms.have_report_layout = ms.report_layout.valid;
  ms.using_boot = true;
  hid_build_boot_layout(&ms.layout, 20);
  const uint8_t auto_report[8] = {0x02, 0x01, 0x00, 0x64,
                                  0x80, 0xF3, 0x01, 0xFF};
  ok = usb_mouse_parse_report(&ms, auto_report, sizeof(auto_report), &delta) &&
       !ms.using_boot && delta.dx == 100 && delta.dy == -200 &&
       delta.buttons == 0x01;
  usb_mouse_selftest_check("report-format auto-detect", ok);

  // Decode a gaming report: report id 1, buttons 0x03, X=+0x1234,
  // Y=-0x0100, wheel=-1, pan=+2.
  memset(&ms, 0, sizeof(ms));
  hid_parse_mouse_layout(hid_test_gaming_descriptor,
                         sizeof(hid_test_gaming_descriptor), &ms.layout);
  const uint8_t gaming_report[8] = {0x01, 0x03, 0x34, 0x12,
                                    0x00, 0xFF, 0xFF, 0x02};
  ok = usb_mouse_parse_report(&ms, gaming_report, sizeof(gaming_report),
                              &delta) &&
       delta.dx == 0x1234 && delta.dy == -0x100 && delta.wheel == -1 &&
       delta.pan == 2 && delta.buttons == 0x03;
  usb_mouse_selftest_check("gaming report decode", ok);

  // A touchpad reports motion as the difference between successive absolute
  // positions.  The first report only establishes the reference, so it must
  // produce no motion.
  memset(&ms, 0, sizeof(ms));
  hid_parse_mouse_layout(hid_test_touchpad_descriptor,
                         sizeof(hid_test_touchpad_descriptor), &ms.layout);
  const uint8_t touch_report_a[5] = {0x00, 0x00, 0x00, 0x64, 0x00};
  const uint8_t touch_report_b[5] = {0x00, 0x00, 0x00, 0x32, 0x00};
  ok = usb_mouse_parse_report(&ms, touch_report_a, sizeof(touch_report_a),
                              &delta) &&
       delta.dx == 0 && delta.dy == 0;
  ok = ok && usb_mouse_parse_report(&ms, touch_report_b,
                                    sizeof(touch_report_b), &delta) &&
       delta.dx == 0 && delta.dy == -50;
  usb_mouse_selftest_check("touchpad absolute decode", ok);

  // A Logitech Unifying receiver report: Report ID 2, 16 buttons declared
  // first, then 12-bit X/Y, wheel and pan.  This is the layout whose buttons
  // used to consume the whole field table and hide the axes.
  memset(&ms, 0, sizeof(ms));
  hid_parse_mouse_layout(hid_test_receiver_descriptor,
                         sizeof(hid_test_receiver_descriptor), &ms.layout);
  const uint8_t receiver_report[8] = {0x02, 0x01, 0x00, 0x64,
                                      0x80, 0xF3, 0x01, 0xFF};
  ok = usb_mouse_parse_report(&ms, receiver_report, sizeof(receiver_report),
                              &delta) &&
       delta.dx == 100 && delta.dy == -200 && delta.wheel == 1 &&
       delta.pan == -1 && delta.buttons == 0x01;
  usb_mouse_selftest_check("receiver report decode", ok);

  // A report with an unknown ID must be ignored, not decoded.
  memset(&ms, 0, sizeof(ms));
  hid_parse_mouse_layout(hid_test_gaming_descriptor,
                         sizeof(hid_test_gaming_descriptor), &ms.layout);
  const uint8_t wrong_report[8] = {0x02, 0x03, 0x34, 0x12,
                                   0x00, 0xFF, 0xFF, 0x02};
  ok = !usb_mouse_parse_report(&ms, wrong_report, sizeof(wrong_report),
                               &delta);
  usb_mouse_selftest_check("unknown report id ignored", ok);

  // A truncated transfer must be ignored.
  ok = !usb_mouse_parse_report(&ms, gaming_report, 3, &delta);
  usb_mouse_selftest_check("truncated report ignored", ok);

  klog_puts(usb_mouse_selftest_failures ? "[USB-MOUSE-TEST] FAILED\n"
                                        : "[USB-MOUSE-TEST] PASSED\n");
}

#endif // USB_MOUSE_SELFTEST

// Diagnostics (/proc/usb)

void usb_mouse_diag(struct usb_diag *d) {
  usb_diag_printf(d, "USB mouse driver:\n");
  usb_diag_printf(d, "  build=%s %s\n", __DATE__, __TIME__);
  usb_diag_printf(d, "  last_probe_failure=%s\n", mouse_last_probe_failure);
  int active = 0;
  for (int i = 0; i < mouse_count; i++) {
    struct usb_mouse_state *ms = &mice[i];
    if (!ms->active)
      continue;
    active++;
    usb_diag_printf(d,
                    "  mouse%d: addr=%u iface=%u ep=0x%02X maxpkt=%u "
                    "interval=%u transport=%s layout=%s reports=%u "
                    "unknown=%u\n",
                    i, ms->dev ? ms->dev->address : 0, ms->interface_number,
                    ms->ep_addr, ms->max_packet, ms->interval, ms->transport,
                    ms->using_boot ? "boot" : "report", ms->reports_decoded,
                    ms->unknown_reports);
    usb_diag_printf(d,
                    "    halt_result=%d report_id=%s buttons=%u "
                    "boot_capable=%u\n",
                    ms->halt_result,
                    ms->layout.has_report_id ? "yes" : "no",
                    ms->layout.button_count, ms->boot_capable ? 1 : 0);
    usb_diag_printf(d,
                    "    counters: polls=%u completions=%u zero_len=%u "
                    "resubmit_fail=%u parse_fail=%u recovery_stage=%u "
                    "silent_warned=%u\n",
                    ms->poll_calls, ms->pipe_completions,
                    ms->zero_len_completions, ms->resubmit_failures,
                    ms->parse_failures, ms->recovery_stage,
                    ms->silent_warned ? 1 : 0);
    usb_diag_printf(d,
                    "    protocol: get_protocol_res=%d value=%u set_idle=%d "
                    "get_report=%d\n",
                    (int)ms->get_protocol_result, ms->get_protocol_value,
                    (int)ms->set_idle_result, ms->get_report_result);
    usb_diag_printf(d,
                    "    control_poll: active=%u polls=%u failures=%u "
                    "last_res=%d\n",
                    ms->control_poll_active ? 1 : 0, ms->control_polls,
                    ms->control_poll_failures, (int)ms->last_control_result);
    if (ms->generic_pipe) {
      struct usb_interrupt_pipe *p = ms->generic_pipe;
      usb_diag_printf(d,
                      "    pipe: active=%u len=%u actual=%u completions=%u "
                      "errors=%u zero=%u last_cc=%u last_len=%u\n",
                      p->active ? 1 : 0, p->buffer_len, p->actual_length,
                      p->completions, p->errors, p->zero_length,
                      p->last_completion, p->last_length);
      xhci_interrupt_pipe_diag(d, p);
    }
    if (ms->event_count) {
      uint8_t start =
          (uint8_t)((ms->event_head + USB_MOUSE_EVENT_LOG - ms->event_count) %
                    USB_MOUSE_EVENT_LOG);
      usb_diag_printf(d, "    events (last %u):\n", ms->event_count);
      for (uint8_t e = 0; e < ms->event_count; e++) {
        struct usb_mouse_event *ev =
            &ms->event_log[(start + e) % USB_MOUSE_EVENT_LOG];
        if (ev->code >= 0xF0) {
          usb_diag_printf(d, "      t=%ums recovery stage=%u\n", ev->ms,
                          ev->code - 0xF0);
          continue;
        }
        usb_diag_printf(d, "      t=%ums cc=%u len=%u bytes=", ev->ms, ev->code,
                        ev->len);
        for (uint8_t b = 0; b < ev->len && b < sizeof(ev->data); b++)
          usb_diag_printf(d, "%02X ", ev->data[b]);
        usb_diag_printf(d, "\n");
      }
    } else {
      usb_diag_printf(d, "    events: none (no interrupt completion yet)\n");
    }
    /* Layout detail: the parsed field map is what decides whether a received
     * report can be decoded at all, so print every report and field. */
    for (uint8_t r = 0; r < HID_MAX_REPORTS; r++) {
      const struct hid_mouse_report *rep = &ms->layout.reports[r];
      if (!rep->used)
        continue;
      usb_diag_printf(d,
                      "    layout report id=%u bits=%u fields=%u dropped=%u\n",
                      rep->id, rep->total_bits, rep->field_count,
                      rep->dropped_fields);
      for (uint8_t f = 0; f < rep->field_count; f++) {
        const struct hid_mouse_field *field = &rep->fields[f];
        usb_diag_printf(d,
                        "      field %s#%u bit=%u size=%u signed=%u abs=%u\n",
                        hid_field_name(field->kind), field->button_index + 1,
                        field->bit_offset, field->bit_size,
                        field->is_signed ? 1 : 0, field->is_absolute ? 1 : 0);
      }
    }
    if (ms->last_report_len) {
      usb_diag_printf(d, "    last_report len=%u bytes=", ms->last_report_len);
      for (uint8_t b = 0; b < ms->last_report_len; b++)
        usb_diag_printf(d, "%02X ", ms->last_report[b]);
      usb_diag_printf(d, "\n");
    }
    usb_diag_printf(d, "    get_report_result=%d bytes=",
                    ms->get_report_result);
    for (uint8_t b = 0; b < sizeof(ms->get_report); b++)
      usb_diag_printf(d, "%02X ", ms->get_report[b]);
    usb_diag_printf(d, "\n");
  }
  if (!active)
    usb_diag_printf(d, "  none active\n");
}
