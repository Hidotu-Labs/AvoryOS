#include "usb.h"
#include "../../console/klog.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../lock/spinlock.h"
#include "../../mm/heap.h"
#include "uhci.h"
#include "usb_kbd.h"
#include "usb_mouse.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_USB_DEVICES 128
static struct usb_device *devices[MAX_USB_DEVICES];
static int device_count = 0;
static uint32_t next_generation = 1;

void usb_enumerate_device(struct usb_device *dev);

static bool usb_initialized = false;

bool usb_verbose = false;

void usb_set_verbose(bool on) {
  usb_verbose = on;
  klogf("[USB] verbose tracing %s\n", on ? "enabled" : "disabled");
}

bool usb_get_verbose(void) { return usb_verbose; }

void usb_init(void) {
  if (usb_initialized)
    return;
  usb_initialized = true;
  {
    extern const char *kernel_boot_cmdline;
    if (kernel_boot_cmdline && strstr(kernel_boot_cmdline, "usbdebug"))
      usb_verbose = true;
  }
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Subsystem initialized.\n");
  klogf("[USB] core build %s %s\n", __DATE__, __TIME__);
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
  usb_dbgf("[USB-CTL] dev=%u hcd=%s type=0x%02X req=0x%02X val=0x%04X idx=%u "
        "len=%u -> %d\n",
        dev->address, dev->hcd->name ? dev->hcd->name : "?", req->request_type,
        req->request, req->value, req->index, len, result);
  if (result >= 0 && data && len && (req->request_type & 0x80U))
    usb_debug_hexdump("[USB-CTL]   in", data, len, 32);
  return result;
}

int usb_hid_get_report_descriptor(struct usb_device *dev, uint8_t iface_num,
                                  uint8_t *buf, uint16_t len) {
  if (!dev || !buf || !len)
    return -1;

  struct usb_control_request req;
  req.request_type = 0x81; /* IN | standard | interface */
  req.request = USB_REQ_GET_DESCRIPTOR;
  req.value = (0x22 << 8) | 0; /* HID report descriptor */
  req.index = iface_num;
  req.length = len;

  /* The xHCI HCD zero-fills the transfer buffer before the DATA stage, but
   * UHCI/OHCI copy back the full requested length even when the device sent
   * less, so the tail would otherwise hold stale transfer-buffer contents.
   * Callers walk items until they run out of buffer; 0x00 is not a valid item
   * prefix, so zero padding is inert. */
  memset(buf, 0, len);
  int result = usb_control_transfer(dev, &req, buf, len);
  klogf("[USB-HID] report descriptor iface=%u req_len=%u -> %s\n", iface_num,
        len, result < 0 ? "FAIL" : "ok");
  return result < 0 ? -1 : 0;
}

bool usb_hid_usage_matches(struct usb_device *dev, uint8_t iface_num,
                           uint8_t usage) {
  uint8_t buf[256];
  if (usb_hid_get_report_descriptor(dev, iface_num, buf, sizeof(buf)) < 0)
    return false;

  /* Minimal item walk: remember the last Usage Page; a Usage on the Generic
   * Desktop page (0x01) matching the requested usage identifies the
   * application collection (0x06 keyboard, 0x02 mouse, ...). */
  uint32_t usage_page = 0;
  uint32_t page_stack[4];
  uint8_t stack_depth = 0;
  uint32_t n = sizeof(buf);
  uint32_t i = 0;
  while (i < n) {
    uint8_t prefix = buf[i++];
    if (prefix == 0xFE) { /* long item: size follows, then payload */
      if (i + 1 >= n)
        break;
      uint8_t size = buf[i];
      i += (uint32_t)size + 1;
      continue;
    }
    uint8_t size = prefix & 0x03;
    uint8_t type = (prefix >> 2) & 0x03; /* 0 main, 1 global, 2 local */
    uint8_t tag = (prefix >> 4) & 0x0F;
    if (size == 3)
      size = 4; /* reserved encoding, treated as 4 bytes */
    if (i + size > n)
      break;
    uint32_t value = 0;
    for (uint8_t b = 0; b < size; b++)
      value |= (uint32_t)buf[i + b] << (8 * b);

    if (type == 1) { /* Global */
      if (tag == 0)
        usage_page = value; /* Usage Page */
      else if (tag == 10 && stack_depth < 4) /* Push */
        page_stack[stack_depth++] = usage_page;
      else if (tag == 11 && stack_depth > 0) /* Pop */
        usage_page = page_stack[--stack_depth];
    } else if (type == 2 && tag == 0) { /* Local: Usage */
      uint32_t page = (value >> 16) ? (value >> 16) : usage_page;
      uint32_t id = value & 0xFFFF;
      if (page == 0x01 && id == usage)
        return true;
    }
    i += size;
  }
  return false;
}

void usb_debug_hexdump(const char *tag, const void *data, uint32_t len,
                       uint32_t max_bytes) {
  if (!usb_verbose)
    return;
  static const char hex[] = "0123456789ABCDEF";
  const uint8_t *p = (const uint8_t *)data;
  uint32_t n = len < max_bytes ? len : max_bytes;
  klog_puts(tag);
  klog_puts(" len=");
  klog_uint64(len);
  klog_puts(" bytes=");
  if (!p) {
    klog_puts("(null)\n");
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    klog_putchar(hex[(p[i] >> 4) & 0xF]);
    klog_putchar(hex[p[i] & 0xF]);
    klog_putchar(' ');
  }
  if (n < len)
    klog_puts("...");
  klog_putchar('\n');
}

struct usb_interrupt_pipe *usb_interrupt_open(
    struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
    uint8_t interval, void *buffer, uint64_t buffer_phys) {
  if (!dev || !dev->connected || !dev->hcd || !dev->hcd->interrupt_open) {
    usb_dbgf("[USB-INT] open dev=%u ep=0x%02X maxpkt=%u interval=%u REJECTED "
          "(dev=%p connected=%u hcd=%p)\n",
          dev ? dev->address : 0, endpoint, max_packet, interval, (void *)dev,
          dev ? dev->connected : 0, dev ? (void *)dev->hcd : NULL);
    return NULL;
  }
  struct usb_interrupt_pipe *pipe = dev->hcd->interrupt_open(
      dev->hcd, dev, endpoint, max_packet, interval, buffer, buffer_phys);
  usb_dbgf("[USB-INT] open dev=%u ep=0x%02X maxpkt=%u interval=%u buf=0x%llX "
        "-> %s\n",
        dev->address, endpoint, max_packet, interval,
        (unsigned long long)buffer_phys, pipe ? "ok" : "FAIL");
  if (pipe) {
    pipe->completions = 0;
    pipe->errors = 0;
    pipe->zero_length = 0;
    pipe->last_completion = 0;
    pipe->last_length = 0;
  }
  return pipe;
}

bool usb_interrupt_completed(struct usb_interrupt_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd ||
      !pipe->dev->hcd->interrupt_completed)
    return false;
  bool completed = pipe->dev->hcd->interrupt_completed(pipe->dev->hcd, pipe);
  if (completed) {
    uint16_t len = pipe->actual_length;
    pipe->dev->hcd->stats.interrupt_completed++;
    pipe->completions++;
    pipe->last_length = len;
    if (len == 0)
      pipe->zero_length++;
    if (pipe->completions <= 4 || (pipe->completions & 0xFFFU) == 0)
      usb_dbgf("[USB-INT] completed dev=%u ep=0x%02X len=%u code=%u n=%u\n",
            pipe->dev->address, pipe->endpoint, len,
            pipe->last_completion, pipe->completions);
  }
  return completed;
}

int usb_interrupt_resubmit(struct usb_interrupt_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd ||
      !pipe->dev->hcd->interrupt_resubmit) {
    usb_dbgf("[USB-INT] resubmit ep=0x%02X REJECTED\n", pipe ? pipe->endpoint : 0);
    return -1;
  }
  int result = pipe->dev->hcd->interrupt_resubmit(pipe->dev->hcd, pipe);
  if (result < 0)
    usb_dbgf("[USB-INT] resubmit dev=%u ep=0x%02X -> FAIL %d\n",
          pipe->dev->address, pipe->endpoint, result);
  return result;
}

void usb_interrupt_cancel(struct usb_interrupt_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd)
    return;
  usb_dbgf("[USB-INT] cancel dev=%u ep=0x%02X\n", pipe->dev->address,
        pipe->endpoint);
  if (pipe->dev->hcd->interrupt_cancel)
    pipe->dev->hcd->interrupt_cancel(pipe->dev->hcd, pipe);
  pipe->active = false;
  pipe->dev->hcd->stats.cancellations++;
}

void usb_device_discovered(struct usb_hcd *hcd, uint8_t port,
                           enum usb_speed speed) {  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: New device detected on port ");
  klog_uint64(port + 1);
  klog_puts(" (");
  klog_puts(usb_speed_name(speed));
  klog_puts(")\n");

  if (device_count >= MAX_USB_DEVICES)
    return;

  /* One device per port: a duplicate discovery (port-status event racing the
   * boot scan) used to create a second entry whose enumeration reset the port
   * out from under the first one. */
  for (int i = 0; i < device_count; i++) {
    struct usb_device *existing = devices[i];
    if (existing && existing->hcd == hcd && existing->port == port &&
        existing->connected && !existing->enumerate_failed)
      return;
  }

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
  dev->claimed_by = 0;
  dev->enumerate_attempts = 0;
  dev->enumerate_failed = false;
  hcd->stats.devices_connected++;
  usb_dbgf("[USB-DBG] discovered port=%u speed=%s hcd=%s gen=%u index=%d\n",
        port + 1, usb_speed_name(speed), hcd->name ? hcd->name : "?",
        dev->generation, device_count);

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
  dev->enumerate_attempts++;
  dev->enumerate_failed = false;

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
    usb_dbgf("[USB-DBG] first 8 bytes failed for port=%u speed=%s hcd=%s\n",
          dev->port + 1, usb_speed_name(dev->speed),
          dev->hcd && dev->hcd->name ? dev->hcd->name : "?");
    dev->enumerate_failed = true;
    return;
  }
  usb_debug_hexdump("[USB-DBG] first descriptor", &dev->desc, 8, 8);

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
    usb_dbgf("[USB-DBG] set address failed requested=%u\n", new_addr);
    dev->enumerate_failed = true;
    return;
  }
  usb_dbgf("[USB-DBG] address assigned=%u requested=%u\n", dev->address,
        new_addr);

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
    dev->enumerate_failed = true;
    return;
  }
  usb_debug_hexdump("[USB-DBG] device descriptor", &dev->desc, 18, 18);

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Device Vendor: ");
  klog_hex32(dev->desc.vendor_id);
  klog_puts(" Product: ");
  klog_hex32(dev->desc.product_id);
  klog_puts("\n");
  usb_dbgf("[USB-DBG] class=0x%02X subclass=0x%02X protocol=0x%02X "
        "bcdUSB=0x%04X bMaxPacket0=%u num_configs=%u\n",
        dev->desc.device_class, dev->desc.device_subclass,
        dev->desc.device_protocol, dev->desc.usb_version,
        dev->desc.max_packet_size, dev->desc.num_configurations);

  /* USB hubs (class 0x09) are not enumerated yet: the devices attached to
   * their downstream ports are invisible to the host drivers.  Detect and
   * report them so a machine where the keyboard is on a root port and the
   * mouse is behind an internal hub is diagnosable from /proc/usb. */
  if (dev->desc.device_class == 0x09) {
    dev->claimed_by = 0x04;
    klog_puts(KLOG_CLR_YELLOW "[ WARN ]" KLOG_CLR_RESET
              " USB: Hub detected; downstream devices are NOT enumerated\n");
    return;
  }

  bool keyboard = usb_kbd_probe(dev);
  bool mouse = usb_mouse_probe(dev);
  if (keyboard) {
    dev->claimed_by |= 0x01;
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Keyboard driver attached\n");
  }
  if (mouse) {
    dev->claimed_by |= 0x02;
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Mouse driver attached\n");
  }
  if (!keyboard && !mouse)
    klogf("[USB] no driver claimed vid=0x%04X pid=0x%04X class=0x%02X "
          "(kbd=%u mouse=%u)\n",
          dev->desc.vendor_id, dev->desc.product_id, dev->desc.device_class,
          keyboard ? 1 : 0, mouse ? 1 : 0);
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

void usb_retry_failed_enumerations(void) {
  for (int i = 0; i < device_count; i++) {
    struct usb_device *dev = devices[i];
    if (!dev || !dev->connected || !dev->enumerate_failed)
      continue;
    if (dev->enumerate_attempts >= 4) {
      /* Give up loudly once so /proc/usb's boot excerpt explains why a port
       * shows a device that never got claimed. */
      if (dev->enumerate_attempts == 4) {
        dev->enumerate_attempts = 5;
        klogf("[USB] giving up on port=%u after %u enumeration attempts\n",
              dev->port + 1, dev->enumerate_attempts - 1);
      }
      continue;
    }
    klogf("[USB] retrying enumeration port=%u hcd=%s attempt=%u\n",
          dev->port + 1, dev->hcd && dev->hcd->name ? dev->hcd->name : "?",
          dev->enumerate_attempts + 1);
    usb_enumerate_device(dev);
  }
}

// Diagnostics (/proc/usb)

void usb_diag_printf(struct usb_diag *d, const char *fmt, ...) {
  if (!d || !d->data || d->len >= d->cap)
    return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(d->data + d->len, d->cap - d->len, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((uint32_t)n >= d->cap - d->len)
    d->len = d->cap;
  else
    d->len += (uint32_t)n;
}

void usb_core_diag(struct usb_diag *d) {
  usb_diag_printf(d, "USB core build: %s %s\n", __DATE__, __TIME__);
  usb_diag_printf(d, "USB devices (core):\n");
  if (!device_count) {
    usb_diag_printf(d, "  none enumerated\n");
    return;
  }
  for (int i = 0; i < device_count; i++) {
    struct usb_device *dev = devices[i];
    if (!dev)
      continue;
    const char *driver = "none";
    if (dev->claimed_by & 0x04)
      driver = "hub";
    else if (dev->claimed_by & 0x03)
      driver = (dev->claimed_by & 0x03) == 0x03 ? "kbd+mouse" :
               (dev->claimed_by & 0x01) ? "keyboard" : "mouse";
    usb_diag_printf(d,
                    "  addr=%u port=%u speed=%s connected=%u configured=%u "
                    "vid=0x%04X pid=0x%04X class=0x%02X driver=%s\n",
                    dev->address, dev->port + 1, usb_speed_name(dev->speed),
                    dev->connected ? 1 : 0, dev->configured ? 1 : 0,
                    dev->desc.vendor_id, dev->desc.product_id,
                    dev->desc.device_class, driver);
    if (dev->enumerate_failed || dev->enumerate_attempts > 1)
      usb_diag_printf(d,
                      "    enumeration: attempts=%u failed=%u (watchdog "
                      "retries up to 4)\n",
                      dev->enumerate_attempts, dev->enumerate_failed ? 1 : 0);
    if (dev->hcd)
      usb_diag_printf(d,
                      "    hcd=%s stats: ctl=%llu/%llu fail=%llu "
                      "int_completed=%llu cancel=%llu connected=%llu "
                      "removed=%llu\n",
                      dev->hcd->name ? dev->hcd->name : "?",
                      (unsigned long long)dev->hcd->stats.control_completed,
                      (unsigned long long)dev->hcd->stats.control_submitted,
                      (unsigned long long)dev->hcd->stats.control_failed,
                      (unsigned long long)dev->hcd->stats.interrupt_completed,
                      (unsigned long long)dev->hcd->stats.cancellations,
                      (unsigned long long)dev->hcd->stats.devices_connected,
                      (unsigned long long)dev->hcd->stats.devices_removed);
  }
}

// Append the USB/XHCI lines from the frozen boot log.  This makes `cat
// /proc/usb` self-contained: the probe decisions and the structured state
// arrive in one command, with no shell pipeline (some keyboard layouts have
// no pipe key) and no serial capture.
static void usb_diag_boot_lines(struct usb_diag *d) {
  static char logbuf[256 * 1024];
  uint32_t n = klog_bootlog_read(0, sizeof(logbuf) - 1, (uint8_t *)logbuf);
  if (!n)
    return;
  logbuf[n] = '\0';

  /* Keep the last 256 matching lines.  The verbose bring-up trace is long,
   * and the earliest mouse-probe decisions are exactly what a failing real
   * machine needs; 48 lines used to drop them.  The arrays are static because
   * this runs on a 16 KiB kernel stack: 256 pointers + 256 lengths + a line
   * buffer used to be ~3.5 KiB of stack per read.  The cache lock serializes
   * the build, so static storage is safe. */
  enum { USB_DIAG_LINES = 256 };
  static const char *lines[USB_DIAG_LINES];
  static uint32_t lens[USB_DIAG_LINES];
  int count = 0;
  int head = 0;

  const char *p = logbuf;
  const char *end = logbuf + n;
  while (p < end) {
    const char *nl = p;
    while (nl < end && *nl != '\n')
      nl++;
    uint32_t len = (uint32_t)(nl - p);
    if (len) {
      static char line[512];
      uint32_t copy = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
      memcpy(line, p, copy);
      line[copy] = '\0';
      bool match = strstr(line, "[USB") || strstr(line, "[XHCI") ||
                   strstr(line, "[MOUSE") || strstr(line, "USB:") ||
                   strstr(line, "EHCI") || strstr(line, "UHCI") ||
                   strstr(line, "OHCI") || strstr(line, "Hub detected");
      /* The per-transfer trace is useful live but floods the excerpt and
       * pushes the probe/enumeration decisions out of the 256-line window. */
      if (match && (strstr(line, "xfer evt") || strstr(line, "submit dci") ||
                    strstr(line, "resubmit dci")))
        match = false;
      if (match) {
        lines[head] = p;
        lens[head] = len + (nl < end ? 1 : 0);
        head = (head + 1) % USB_DIAG_LINES;
        if (count < USB_DIAG_LINES)
          count++;
      }
    }
    p = nl < end ? nl + 1 : end;
  }

  if (!count)
    return;
  usb_diag_printf(d, "USB-related boot log (oldest first, last %d lines):\n",
                  count);
  int start = (head - count + USB_DIAG_LINES) % USB_DIAG_LINES;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % USB_DIAG_LINES;
    static char line[512];
    uint32_t copy = lens[idx] < sizeof(line) - 1 ? lens[idx]
                                                 : sizeof(line) - 1;
    memcpy(line, lines[idx], copy);
    line[copy] = '\0';
    usb_diag_printf(d, "  %s", line);
    if (line[copy ? copy - 1 : 0] != '\n')
      usb_diag_printf(d, "\n");
  }
}

uint32_t usb_diag_read(uint32_t offset, uint32_t size, uint8_t *buffer) {
  static char cache[65536];
  static uint32_t cache_len;
  static spinlock_t cache_lock = SPINLOCK_INIT;

  if (offset == 0 && spinlock_try_acquire(&cache_lock)) {
    struct usb_diag d = {cache, sizeof(cache), 0};
    usb_core_diag(&d);
    xhci_diag(&d);
    ehci_diag(&d);
    usb_kbd_diag(&d);
    usb_mouse_diag(&d);
    usb_diag_boot_lines(&d);
    cache_len = d.len;
    spinlock_release(&cache_lock);
  }

  if (offset >= cache_len || !buffer)
    return 0;
  uint32_t n = cache_len - offset;
  if (n > size)
    n = size;
  for (uint32_t i = 0; i < n; i++)
    buffer[i] = (uint8_t)cache[offset + i];
  return n;
}

static uint32_t usb_diag_copy_out(const char *cache, uint32_t cache_len,
                                  uint32_t offset, uint32_t size,
                                  uint8_t *buffer) {
  if (offset >= cache_len || !buffer)
    return 0;
  uint32_t n = cache_len - offset;
  if (n > size)
    n = size;
  for (uint32_t i = 0; i < n; i++)
    buffer[i] = (uint8_t)cache[offset + i];
  return n;
}

uint32_t usb_mouse_diag_read(uint32_t offset, uint32_t size, uint8_t *buffer) {
  static char cache[16384];
  static uint32_t cache_len;
  static spinlock_t cache_lock = SPINLOCK_INIT;

  if (offset == 0 && spinlock_try_acquire(&cache_lock)) {
    struct usb_diag d = {cache, sizeof(cache), 0};
    usb_mouse_diag(&d);
    cache_len = d.len;
    spinlock_release(&cache_lock);
  }
  return usb_diag_copy_out(cache, cache_len, offset, size, buffer);
}

uint32_t usb_xhci_diag_read(uint32_t offset, uint32_t size, uint8_t *buffer) {
  static char cache[32768];
  static uint32_t cache_len;
  static spinlock_t cache_lock = SPINLOCK_INIT;

  if (offset == 0 && spinlock_try_acquire(&cache_lock)) {
    struct usb_diag d = {cache, sizeof(cache), 0};
    xhci_diag(&d);
    cache_len = d.len;
    spinlock_release(&cache_lock);
  }
  return usb_diag_copy_out(cache, cache_len, offset, size, buffer);
}
