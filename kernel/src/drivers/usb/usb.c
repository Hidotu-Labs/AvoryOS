#include "usb.h"
#include "../../console/klog.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../lock/spinlock.h"
#include "../../mm/heap.h"
#include "uhci.h"
#include "usb_kbd.h"
#include "usb_mouse.h"
#include "usb_audio.h"
#ifdef USE_RUST_USB
#include "usb_rust_glue.h"
/* Rust mass-storage class driver (drivers/usb/storage).  Declared here until
 * usb_rust_glue.h carries them.  The probe runs in enumeration context: it
 * reads the configuration descriptor, records the device and wakes the
 * worker, and returns true when the device has a BOT interface. */
bool rust_usb_storage_probe(void *handle);
void rust_usb_storage_removed(void *handle);

/* True when `option` (for example "usb_storage_probe=1") is a whole word on
 * the kernel command line. */
static bool usb_cmdline_has_option(const char *option) {
  extern const char *kernel_boot_cmdline;
  if (!kernel_boot_cmdline)
    return false;
  const char *p = kernel_boot_cmdline;
  size_t option_len = strlen(option);
  while ((p = strstr(p, option)) != NULL) {
    if ((p == kernel_boot_cmdline || p[-1] == ' ') &&
        (p[option_len] == '\0' || p[option_len] == ' '))
      return true;
    p += option_len;
  }
  return false;
}
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_USB_DEVICES 128
static struct usb_device *devices[MAX_USB_DEVICES];
static int device_count = 0;
static uint32_t next_generation = 1;

/* Guards devices[], device_count and the topology fields of its entries.  It
 * is never held across enumeration, control transfers, or driver hooks. */
static spinlock_t topo_lock = SPINLOCK_INIT;
#define USB_MAX_PORTS_PER_HUB 15
#define USB_MAX_BUSES 8
static const struct usb_hcd *bus_hcds[USB_MAX_BUSES];
static int bus_count = 0;

void usb_enumerate_device(struct usb_device *dev);
static bool usb_topology_selftest_requested(void);

static bool usb_initialized = false;

void usb_init(void) {
  if (usb_initialized)
    return;
  usb_initialized = true;
#ifdef USE_RUST_USB
  rust_usb_hub_init();
#endif
#ifdef USB_AUDIO_SELFTEST
  usb_audio_selftest();
#endif
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Subsystem initialized.\n");
  klogf("[USB] core build %s %s\n", __DATE__, __TIME__);
  for (int i = 0; i < MAX_USB_DEVICES; i++)
    devices[i] = NULL;
  if (usb_topology_selftest_requested())
    usb_topology_selftest();
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

struct usb_interrupt_pipe *usb_interrupt_open(
    struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
    uint8_t interval, void *buffer, uint64_t buffer_phys) {
  if (!dev || !dev->connected || !dev->hcd || !dev->hcd->interrupt_open) {
    return NULL;
  }
  struct usb_interrupt_pipe *pipe = dev->hcd->interrupt_open(
      dev->hcd, dev, endpoint, max_packet, interval, buffer, buffer_phys);
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
  }
  return completed;
}

int usb_interrupt_resubmit(struct usb_interrupt_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd ||
      !pipe->dev->hcd->interrupt_resubmit) {
    return -1;
  }
  int result = pipe->dev->hcd->interrupt_resubmit(pipe->dev->hcd, pipe);
  return result;
}

void usb_interrupt_cancel(struct usb_interrupt_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd)
    return;
  if (pipe->dev->hcd->interrupt_cancel)
    pipe->dev->hcd->interrupt_cancel(pipe->dev->hcd, pipe);
  pipe->active = false;
  pipe->dev->hcd->stats.cancellations++;
}

struct usb_isoch_stream *usb_isoch_open(struct usb_device *dev,
                                        uint8_t endpoint,
                                        uint16_t max_packet,
                                        uint8_t interval) {
  if (!dev || !dev->connected || !dev->hcd || !dev->hcd->isoch_open)
    return NULL;
  return dev->hcd->isoch_open(dev->hcd, dev, endpoint, max_packet, interval);
}

int usb_isoch_submit(struct usb_isoch_stream *stream,
                     struct usb_isoch_packet *packet) {
  if (!stream ||
      !__atomic_load_n(&stream->active, __ATOMIC_ACQUIRE) || !stream->dev ||
      !packet ||
      !stream->dev->hcd || !stream->dev->hcd->isoch_submit ||
      !packet->buffer || !packet->buffer_phys || !packet->length)
    return -1;
  packet->actual_length = 0;
  packet->completion_code = 0;
  __atomic_store_n(&packet->completed, false, __ATOMIC_RELEASE);
  return stream->dev->hcd->isoch_submit(stream->dev->hcd, stream, packet);
}

bool usb_isoch_completed(struct usb_isoch_packet *packet) {
  return packet && __atomic_load_n(&packet->completed, __ATOMIC_ACQUIRE);
}

bool usb_isoch_cancel(struct usb_isoch_stream *stream) {
  if (!stream)
    return false;
  if (!stream->dev)
    return true;
  if (!stream->dev->hcd)
    return false;
  if (stream->dev->hcd->isoch_cancel)
    return stream->dev->hcd->isoch_cancel(stream->dev->hcd, stream);
  return false;
}

int usb_hub_configure(struct usb_device *hub, uint8_t nports, bool multi_tt,
                      uint8_t tt_think_time) {
  int rc;
  if (!hub || !hub->connected || !hub->is_hub || !hub->hcd ||
      !hub->hcd->hub_configure)
    return -1;
  rc = hub->hcd->hub_configure(hub->hcd, hub, nports, multi_tt,
                               tt_think_time);
  if (rc == 0) {
    hub->num_ports = nports;
    hub->multi_tt = multi_tt;
  }
  return rc;
}

struct usb_bulk_pipe *usb_bulk_open(struct usb_device *dev, uint8_t endpoint,
                                    uint16_t max_packet, uint8_t max_burst) {
  if (!dev || !dev->connected || !dev->hcd || !dev->hcd->bulk_open ||
      !max_packet)
    return NULL;
  return dev->hcd->bulk_open(dev->hcd, dev, endpoint, max_packet, max_burst);
}

int usb_bulk_transfer(struct usb_bulk_pipe *pipe, void *buf, uint64_t phys,
                      uint32_t len, uint32_t *actual, uint32_t timeout_ms) {
  if (actual)
    *actual = 0;
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->connected ||
      !pipe->dev->hcd || !pipe->dev->hcd->bulk_transfer)
    return USB_BULK_GONE;
  return pipe->dev->hcd->bulk_transfer(pipe->dev->hcd, pipe, buf, phys, len,
                                       actual, timeout_ms);
}

int usb_clear_halt(struct usb_bulk_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd ||
      !pipe->dev->hcd->bulk_clear_halt)
    return USB_BULK_GONE;
  struct usb_device *dev = pipe->dev;
  int res = dev->hcd->bulk_clear_halt(dev->hcd, pipe);
  if (res < 0)
    return res;
  struct usb_control_request req = {
      .request_type = 0x02, /* OUT | standard | endpoint */
      .request = USB_REQ_CLEAR_FEATURE,
      .value = 0, /* ENDPOINT_HALT */
      .index = pipe->endpoint,
      .length = 0,
  };
  return usb_control_transfer(dev, &req, NULL, 0) < 0 ? USB_BULK_ERR
                                                      : USB_BULK_OK;
}

void usb_bulk_close(struct usb_bulk_pipe *pipe) {
  if (!pipe || !pipe->active || !pipe->dev || !pipe->dev->hcd ||
      !pipe->dev->hcd->bulk_close)
    return;
  pipe->dev->hcd->bulk_close(pipe->dev->hcd, pipe);
}

/* Bus numbers: the nth host controller to bring up a root port is bus n, as in
 * Linux's "1-3.2.4".  Entries are never removed.  Caller holds topo_lock. */
static uint8_t usb_bus_number_locked(const struct usb_hcd *hcd, bool assign) {
  for (int i = 0; i < bus_count; i++)
    if (bus_hcds[i] == hcd)
      return (uint8_t)(i + 1);
  if (!assign || bus_count >= USB_MAX_BUSES)
    return 0;
  bus_hcds[bus_count++] = hcd;
  return (uint8_t)bus_count;
}

static uint8_t usb_bus_number(const struct usb_hcd *hcd) {
  spinlock_acquire(&topo_lock);
  uint8_t bus = usb_bus_number_locked(hcd, false);
  spinlock_release(&topo_lock);
  return bus;
}

/* "<bus>-<root>[.<hub port>...]".  The buffer is always large enough: the
 * longest form, "255-255.15.15.15.15.15", is 22 characters. */
static void usb_topo_format_path(char buf[32], uint8_t bus,
                                 const struct usb_device *dev) {
  int n = snprintf(buf, 32, "%u-%u", bus, dev->root_port);
  for (uint8_t k = 0; k < dev->depth; k++)
    n += snprintf(buf + n, 32 - n, ".%u", (dev->route >> (4 * k)) & 0xF);
}

/* Fill the topology fields of dev, which the caller has zeroed.  `parent` is
 * NULL for a root port, in which case root_port0 is the 0-based root port.
 * Pure computation: it reads the parent and touches no table.  Returns -1 when
 * the attachment would exceed USB_MAX_DEPTH or the port is not a valid hub
 * port. */
static int usb_topo_fill(struct usb_device *dev, struct usb_hcd *hcd,
                         struct usb_device *parent, uint8_t parent_port,
                         uint8_t root_port0, enum usb_speed speed) {
  if (!parent) {
    dev->parent = NULL;
    dev->parent_port = 0;
    dev->root_port = (uint8_t)(root_port0 + 1);
    dev->port = root_port0;
    dev->depth = 0;
    dev->route = 0;
  } else {
    if (!parent->connected || !parent->is_hub || parent->depth >= USB_MAX_DEPTH)
      return -1;
    if (parent_port < 1 || parent_port > USB_MAX_PORTS_PER_HUB)
      return -1;
    if (parent->num_ports && parent_port > parent->num_ports)
      return -1;
    dev->parent = parent;
    dev->parent_port = parent_port;
    dev->root_port = parent->root_port;
    dev->port = parent->port;
    dev->depth = (uint8_t)(parent->depth + 1);
    dev->route = parent->route |
                 ((uint32_t)parent_port << (4 * parent->depth));
  }
  dev->hcd = hcd;
  dev->speed = speed;

  /* Transaction translator: an LS or FS device is served by the TT of the
   * nearest HS hub above it.  Walk up until that hub; tt_port is the port on
   * that hub that leads down to this device's branch. */
  dev->tt_hub = NULL;
  dev->tt_port = 0;
  if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
    struct usb_device *child = dev;
    for (struct usb_device *anc = dev->parent; anc; anc = anc->parent) {
      if (anc->speed == USB_SPEED_HIGH && anc->is_hub) {
        dev->tt_hub = anc;
        dev->tt_port = child->parent_port;
        break;
      }
      child = anc;
    }
  }
  return 0;
}

/* Find the connected device with the given identity, or NULL.  A root device
 * is identified by its root port (`port`); a child by its parent port. */
static struct usb_device *usb_topo_find_locked(struct usb_hcd *hcd,
                                               struct usb_device *parent,
                                               uint8_t port) {
  for (int i = 0; i < MAX_USB_DEVICES; i++) {
    struct usb_device *d = devices[i];
    if (!d || d->hcd != hcd || d->parent != parent)
      continue;
    if (parent ? d->parent_port != port : d->port != port)
      continue;
    if (d->connected)
      return d;
  }
  return NULL;
}

/* Shared attach path for root ports (parent NULL) and hub children.  Table
 * edits happen under topo_lock; allocation, device_prepare and enumeration run
 * outside it. */
static int usb_device_attach(struct usb_hcd *hcd, struct usb_device *parent,
                             uint8_t parent_port, uint8_t root_port0,
                             enum usb_speed speed) {
  uint8_t identity = parent ? parent_port : root_port0;
  struct usb_device *spare = kmalloc(sizeof(struct usb_device));
  struct usb_device *dev = NULL;
  struct usb_device topo;
  char path[32];

  spinlock_acquire(&topo_lock);
  if (!parent)
    usb_bus_number_locked(hcd, true);
  memset(&topo, 0, sizeof(topo));
  if (usb_topo_fill(&topo, hcd, parent, parent_port, root_port0, speed) < 0) {
    spinlock_release(&topo_lock);
    if (spare)
      kfree(spare);
    return -1;
  }

  /* One device per identity: a duplicate discovery (port-status event racing
   * the boot scan) must not create a second entry whose enumeration resets the
   * port out from under the first one. */
  struct usb_device *existing = usb_topo_find_locked(hcd, parent, identity);
  if (existing && !existing->enumerate_failed) {
    spinlock_release(&topo_lock);
    if (spare)
      kfree(spare);
    return 0;
  }
  for (int i = 0; i < MAX_USB_DEVICES && !dev; i++) {
    struct usb_device *d = devices[i];
    if (d && !d->connected && d->hcd == hcd && d->parent == parent &&
        (parent ? d->parent_port == identity : d->port == identity) &&
        usb_audio_device_reusable(d))
      dev = d;
  }
  if (!dev) {
    if (device_count >= MAX_USB_DEVICES || !spare) {
      spinlock_release(&topo_lock);
      if (spare)
        kfree(spare);
      return -1;
    }
    dev = spare;
    spare = NULL;
    devices[device_count++] = dev;
  }
  if (spare)
    kfree(spare);

  *dev = topo;
  dev->connected = true;
  dev->generation = next_generation++;
  hcd->stats.devices_connected++;
  if (parent)
    usb_topo_format_path(path, usb_bus_number_locked(hcd, false), dev);
  spinlock_release(&topo_lock);

  if (parent) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: New device detected on ");
    klog_puts(path);
    klog_puts(" (");
    klog_puts(usb_speed_name(speed));
    klog_puts(")\n");
  }

  if (hcd->device_prepare && hcd->device_prepare(hcd, dev) < 0) {
    spinlock_acquire(&topo_lock);
    dev->connected = false;
    spinlock_release(&topo_lock);
    klog_puts("[USB] Host controller failed to prepare device\n");
    return -1;
  }

  usb_enumerate_device(dev);
  return 0;
}

void usb_device_discovered(struct usb_hcd *hcd, uint8_t port,
                           enum usb_speed speed) {
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: New device detected on port ");
  klog_uint64(port + 1);
  klog_puts(" (");
  klog_puts(usb_speed_name(speed));
  klog_puts(")\n");
  (void)usb_device_attach(hcd, NULL, 0, port, speed);
}

int usb_hub_child_discovered(struct usb_device *hub, uint8_t port,
                             enum usb_speed speed) {
  if (!hub || !hub->hcd)
    return -1;
  return usb_device_attach(hub->hcd, hub, port, 0, speed);
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
    dev->enumerate_failed = true;
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
  } else if (dev->address != 0) {
    res = 0;
  } else {
    res = usb_control_transfer(dev, &req, NULL, 0);
    if (res == 0)
      dev->address = new_addr;
  }
  if (res < 0) {
    klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " USB: Failed to set address\n");
    dev->enumerate_failed = true;
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
    dev->enumerate_failed = true;
    return;
  }

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Device Vendor: ");
  klog_hex32(dev->desc.vendor_id);
  klog_puts(" Product: ");
  klog_hex32(dev->desc.product_id);
  klog_puts("\n");
#ifdef USE_RUST_USB
  rust_usb_device_probe(dev, dev->desc.vendor_id, dev->desc.product_id);
#endif

  /* USB hubs (class 0x09) are not enumerated yet: the devices attached to
   * their downstream ports are invisible to the host drivers.  Detect and
   * report them so a machine where the keyboard is on a root port and the
   * mouse is behind an internal hub is diagnosable from /proc/usb. */
  if (dev->desc.device_class == 0x09) {
    dev->claimed_by = USB_CLAIM_HUB;
    dev->is_hub = true;
#ifdef USE_RUST_USB
    /* The Rust hub driver sets the configuration, powers the ports and
     * reports children back through usb_hub_child_discovered. */
    rust_usb_hub_probe(dev);
#else
    klog_puts(KLOG_CLR_YELLOW "[ WARN ]" KLOG_CLR_RESET
              " USB: Hub detected; downstream devices are NOT enumerated\n");
#endif
    return;
  }

  bool keyboard = usb_kbd_probe(dev);
  bool mouse = usb_mouse_probe(dev);
  struct usb_audio_playback_desc audio_playback;
  bool audio_supported = usb_audio_discover(dev, &audio_playback);
  if (audio_supported)
    usb_audio_device_discovered(dev, &audio_playback);
  if (keyboard) {
    dev->claimed_by |= USB_CLAIM_KBD;
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Keyboard driver attached\n");
  }
  if (mouse) {
    dev->claimed_by |= USB_CLAIM_MOUSE;
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " USB: Mouse driver attached\n");
  }
  if (audio_supported)
    klogf("[USB-AUDIO] UAC1 playback descriptor matched: iface=%u alt=%u "
          "ep=0x%02X maxpkt=%u interval=%u sync=%s; configuration unchanged\n",
          audio_playback.interface_number, audio_playback.alternate_setting,
          audio_playback.endpoint_address, audio_playback.max_packet_size,
          audio_playback.interval,
          ((audio_playback.endpoint_attributes >> 2) & 3) == 3
              ? "synchronous" : "adaptive");
#ifdef USE_RUST_USB
  /* Skipped when the C BOT self-test owns mass-storage interfaces. */
  if (!keyboard && !mouse && !audio_supported &&
      !usb_cmdline_has_option("usb_storage_probe=1") &&
      rust_usb_storage_probe(dev))
    dev->claimed_by |= USB_CLAIM_STORAGE;
#endif
  if (!keyboard && !mouse && !audio_supported &&
      !(dev->claimed_by & USB_CLAIM_STORAGE))
    klogf("[USB] no driver claimed vid=0x%04X pid=0x%04X class=0x%02X "
          "(kbd=%u mouse=%u)\n",
          dev->desc.vendor_id, dev->desc.product_id, dev->desc.device_class,
          keyboard ? 1 : 0, mouse ? 1 : 0);
}

/* Run only after every host controller's initial root-port scan has finished.
 * Keeping this out of device-discovery callbacks makes the opt-in transport
 * exercise safe from IRQ and hotplug contexts. */
void usb_audio_boot_smoke(void) {
  for (int i = 0; i < device_count; i++)
    usb_audio_probe_smoke(devices[i]);
}

/* Same contract as usb_audio_boot_smoke(): after every controller's root-port
 * scan, in task context. */
void usb_storage_boot_smoke(void) {
  for (int i = 0; i < device_count; i++)
    usb_storage_probe_smoke(devices[i]);
}

/* First connected device below `parent` in table[0..capacity), or NULL. */
static struct usb_device *usb_topo_next_child(struct usb_device **table,
                                              int capacity,
                                              struct usb_device *parent) {
  spinlock_acquire(&topo_lock);
  struct usb_device *found = NULL;
  for (int i = 0; i < capacity && !found; i++) {
    struct usb_device *d = table[i];
    if (d && d->parent == parent && d->connected)
      found = d;
  }
  spinlock_release(&topo_lock);
  return found;
}

/* Remove dev and every device below it.  Children are removed first, so each
 * device's hook runs after all of its descendants' hooks: deepest first, and
 * the subtree root last.  topo_lock is never held across `hook`. */
static void usb_topo_remove_tree(struct usb_device **table, int capacity,
                                 struct usb_device *dev,
                                 void (*hook)(struct usb_device *dev)) {
  struct usb_device *child;
  while ((child = usb_topo_next_child(table, capacity, dev)) != NULL)
    usb_topo_remove_tree(table, capacity, child, hook);

  spinlock_acquire(&topo_lock);
  bool was_connected = dev->connected;
  dev->connected = false;
  spinlock_release(&topo_lock);
  if (was_connected)
    hook(dev);
}

/* Driver and HCD teardown for one device whose connected flag is already
 * cleared. */
static void usb_topo_device_gone(struct usb_device *dev) {
  struct usb_hcd *hcd = dev->hcd;
  hcd->stats.devices_removed++;
#ifdef USE_RUST_USB
  /* Before the HCD teardown: the hub driver must stop polling and cancel its
   * interrupt pipe first.  Children were already removed (deepest first). */
  if (dev->is_hub)
    rust_usb_hub_removed(dev);
  rust_usb_storage_removed(dev);
#endif
  usb_kbd_disconnect(dev);
  usb_mouse_disconnect(dev);
  if (hcd->device_removed)
    hcd->device_removed(hcd, dev);
  /* The HCD has revoked endpoint DMA by this point. ALSA node removal runs
   * later in the audio worker, outside this IRQ/watchdog context. */
  usb_audio_device_removed(dev);
  if (dev->depth == 0) {
    klog_puts("[USB] Device removed from port ");
    klog_uint64(dev->port + 1);
    klog_puts("\n");
  } else {
    char path[32];
    usb_topo_format_path(path, usb_bus_number(hcd), dev);
    klog_puts("[USB] Device removed from ");
    klog_puts(path);
    klog_puts("\n");
  }
}

static struct usb_device *usb_topo_find_connected_locked(
    struct usb_hcd *hcd, struct usb_device *parent, uint8_t port) {
  for (int i = 0; i < MAX_USB_DEVICES; i++) {
    struct usb_device *d = devices[i];
    if (!d || !d->connected || d->hcd != hcd || d->parent != parent)
      continue;
    if (parent ? d->parent_port == port : d->port == port)
      return d;
  }
  return NULL;
}

void usb_hub_child_removed(struct usb_device *hub, uint8_t port) {
  if (!hub)
    return;
  spinlock_acquire(&topo_lock);
  struct usb_device *dev =
      usb_topo_find_connected_locked(hub->hcd, hub, port);
  spinlock_release(&topo_lock);
  if (dev)
    usb_topo_remove_tree(devices, MAX_USB_DEVICES, dev, usb_topo_device_gone);
}

void usb_device_removed(struct usb_hcd *hcd, uint8_t port) {
  spinlock_acquire(&topo_lock);
  struct usb_device *dev = usb_topo_find_connected_locked(hcd, NULL, port);
  spinlock_release(&topo_lock);
  if (dev)
    usb_topo_remove_tree(devices, MAX_USB_DEVICES, dev, usb_topo_device_gone);
}

/* ---- Topology self-test ----
 * Builds trees on a private table with a mock HCD.  Nothing here touches
 * devices[] or any controller.  Runs only with usb_topology_selftest=1. */
#define ST_CAP 16

static struct usb_device *st_table[ST_CAP];
static struct usb_device st_nodes[ST_CAP];
static int st_used;
static struct usb_device *st_removed[ST_CAP];
static int st_removed_count;
static int st_hcd_removed_calls;
static const char *st_fail;

static void st_mock_device_removed(struct usb_hcd *hcd, struct usb_device *dev) {
  (void)hcd;
  (void)dev;
  st_hcd_removed_calls++;
}

static struct usb_hcd st_hcd = {.name = "selftest",
                                .device_removed = st_mock_device_removed};

#define ST_CHECK(cond, why)                                                    \
  do {                                                                         \
    if (!(cond) && !st_fail)                                                   \
      st_fail = (why);                                                         \
  } while (0)

static void st_reset(void) {
  for (int i = 0; i < ST_CAP; i++) {
    st_table[i] = NULL;
    memset(&st_nodes[i], 0, sizeof(st_nodes[i]));
  }
  st_used = 0;
  st_removed_count = 0;
  st_hcd_removed_calls = 0;
}

/* Add a device below `parent` (or on root port root_port0 when parent is
 * NULL).  Returns NULL when the topology is rejected, as the real attach does. */
static struct usb_device *st_add(struct usb_device *parent, uint8_t parent_port,
                                 uint8_t root_port0, enum usb_speed speed,
                                 bool hub, uint8_t num_ports) {
  if (st_used >= ST_CAP)
    return NULL;
  struct usb_device *dev = &st_nodes[st_used];
  memset(dev, 0, sizeof(*dev));
  if (usb_topo_fill(dev, &st_hcd, parent, parent_port, root_port0, speed) < 0)
    return NULL;
  dev->connected = true;
  dev->is_hub = hub;
  dev->num_ports = num_ports;
  st_table[st_used++] = dev;
  return dev;
}

/* Removal hook for the test: the same ordering as the real teardown, but it
 * records the device and calls only the mock HCD callback. */
static void st_remove_hook(struct usb_device *dev) {
  if (st_removed_count < ST_CAP)
    st_removed[st_removed_count++] = dev;
  if (dev->hcd && dev->hcd->device_removed)
    dev->hcd->device_removed(dev->hcd, dev);
}

static void st_check_routes(void) {
  st_reset();
  /* Path 1-3.2.4: root port 3 (0-based 2), hub port 2, then hub port 4. */
  struct usb_device *hub_a = st_add(NULL, 0, 2, USB_SPEED_HIGH, true, 8);
  struct usb_device *hub_b = st_add(hub_a, 2, 0, USB_SPEED_HIGH, true, 8);
  struct usb_device *leaf = st_add(hub_b, 4, 0, USB_SPEED_HIGH, false, 0);
  ST_CHECK(hub_a && hub_b && leaf, "route: tree construction failed");
  if (!leaf)
    return;
  ST_CHECK(hub_a->route == 0 && hub_a->depth == 0, "route: root device not depth 0 route 0");
  ST_CHECK(hub_b->route == 0x2 && hub_b->depth == 1, "route: tier-1 nibble not 2");
  ST_CHECK(leaf->route == 0x42 && leaf->depth == 2, "route: 1-3.2.4 not route 0x42");
  ST_CHECK(leaf->root_port == 3 && leaf->port == 2, "route: root port not inherited");
  ST_CHECK(leaf->parent == hub_b && leaf->parent_port == 4, "route: parent link wrong");

  char path[32];
  usb_topo_format_path(path, 1, leaf);
  ST_CHECK(strcmp(path, "1-3.2.4") == 0, "route: path string is not 1-3.2.4");

  /* Five tiers are the limit.  Chain root hub -> 5 more hubs (depths 1..5),
   * all on port 1, so route nibbles are 1,1,1,1,1. */
  st_reset();
  struct usb_device *top = st_add(NULL, 0, 0, USB_SPEED_HIGH, true, 4);
  struct usb_device *tier = top;
  for (int depth = 1; depth <= USB_MAX_DEPTH && tier; depth++)
    tier = st_add(tier, 1, 0, USB_SPEED_HIGH, true, 4);
  ST_CHECK(tier && tier->depth == USB_MAX_DEPTH, "tier: depth-5 device rejected");
  ST_CHECK(tier && tier->route == 0x11111, "tier: five-tier route not 0x11111");
  if (tier) {
    int before = st_used;
    ST_CHECK(st_add(tier, 1, 0, USB_SPEED_HIGH, false, 0) == NULL,
             "tier: depth-6 device was accepted");
    ST_CHECK(st_used == before, "tier: rejected device left a table entry");
  }
  /* Port range is checked on a shallow hub, so depth does not mask it. */
  if (top) {
    ST_CHECK(st_add(top, 0, 0, USB_SPEED_HIGH, false, 0) == NULL,
             "port: port 0 was accepted");
    ST_CHECK(st_add(top, 5, 0, USB_SPEED_HIGH, false, 0) == NULL,
             "port: port beyond num_ports was accepted");
  }
}

static void st_check_tt(void) {
  st_reset();
  struct usb_device *hs_hub = st_add(NULL, 0, 0, USB_SPEED_HIGH, true, 4);
  struct usb_device *fs_dev = st_add(hs_hub, 3, 0, USB_SPEED_FULL, false, 0);
  struct usb_device *fs_hub = st_add(hs_hub, 2, 0, USB_SPEED_FULL, true, 4);
  struct usb_device *ls_dev = st_add(fs_hub, 1, 0, USB_SPEED_LOW, false, 0);
  struct usb_device *hs_dev = st_add(hs_hub, 1, 0, USB_SPEED_HIGH, false, 0);
  struct usb_device *root_fs = st_add(NULL, 0, 1, USB_SPEED_FULL, false, 0);
  ST_CHECK(hs_hub && fs_dev && fs_hub && ls_dev && hs_dev && root_fs,
           "tt: tree construction failed");
  if (!(hs_hub && fs_dev && fs_hub && ls_dev && hs_dev && root_fs))
    return;
  ST_CHECK(fs_dev->tt_hub == hs_hub && fs_dev->tt_port == 3,
           "tt: FS device under HS hub has wrong TT");
  ST_CHECK(ls_dev->tt_hub == hs_hub && ls_dev->tt_port == 2,
           "tt: LS device behind FS hub must use the HS hub's TT on port 2");
  ST_CHECK(hs_dev->tt_hub == NULL, "tt: HS device got a TT");
  ST_CHECK(root_fs->tt_hub == NULL, "tt: root-port FS device got a TT");
  ST_CHECK(hs_hub->tt_hub == NULL, "tt: HS hub got a TT");
}

static void st_check_removal(void) {
  st_reset();
  /* A (root, hub) -> B (hub, port 2) -> C (port 4 of B); A -> D (port 1);
   * E is an unrelated root device that must survive. */
  struct usb_device *a = st_add(NULL, 0, 0, USB_SPEED_HIGH, true, 4);
  struct usb_device *b = st_add(a, 2, 0, USB_SPEED_HIGH, true, 4);
  struct usb_device *c = st_add(b, 4, 0, USB_SPEED_HIGH, false, 0);
  struct usb_device *d = st_add(a, 1, 0, USB_SPEED_FULL, false, 0);
  struct usb_device *e = st_add(NULL, 0, 1, USB_SPEED_HIGH, false, 0);
  ST_CHECK(a && b && c && d && e, "removal: tree construction failed");
  if (!(a && b && c && d && e))
    return;
  usb_topo_remove_tree(st_table, ST_CAP, a, st_remove_hook);
  ST_CHECK(st_removed_count == 4, "removal: wrong number of devices removed");
  ST_CHECK(st_hcd_removed_calls == 4, "removal: mock device_removed not called per device");
  if (st_removed_count == 4) {
    ST_CHECK(st_removed[0] == c && st_removed[1] == b &&
                 st_removed[2] == d && st_removed[3] == a,
             "removal: order is not deepest first");
  }
  ST_CHECK(!a->connected && !b->connected && !c->connected && !d->connected,
           "removal: subtree device still connected");
  ST_CHECK(e->connected, "removal: unrelated device was removed");
}

void usb_topology_selftest(void) {
  st_fail = NULL;
  st_check_routes();
  st_check_tt();
  st_check_removal();
  st_reset();
  if (st_fail)
    klogf("USB topology selftest: FAIL %s\n", st_fail);
  else
    klogf("USB topology selftest: PASS\n");
}

static bool usb_topology_selftest_requested(void) {
  extern const char *kernel_boot_cmdline;
  if (!kernel_boot_cmdline)
    return false;
  const char *p = kernel_boot_cmdline;
  const char *option = "usb_topology_selftest=1";
  size_t option_len = strlen(option);
  while ((p = strstr(p, option)) != NULL) {
    if ((p == kernel_boot_cmdline || p[-1] == ' ') &&
        (p[option_len] == '\0' || p[option_len] == ' '))
      return true;
    p += option_len;
  }
  return false;
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

static void usb_core_diag_node(struct usb_diag *d, const struct usb_device *dev,
                               uint8_t bus) {
  char path[32];
  usb_topo_format_path(path, bus, dev);
  const char *driver = "none";
  if (dev->claimed_by & USB_CLAIM_HUB)
    driver = "hub";
  else if (dev->claimed_by & (USB_CLAIM_KBD | USB_CLAIM_MOUSE))
    driver = (dev->claimed_by & (USB_CLAIM_KBD | USB_CLAIM_MOUSE)) ==
                     (USB_CLAIM_KBD | USB_CLAIM_MOUSE)
                 ? "kbd+mouse"
             : (dev->claimed_by & USB_CLAIM_KBD) ? "keyboard"
                                                 : "mouse";

  /* Children are indented two spaces per tier under their parent. */
  usb_diag_printf(d, "  ");
  for (uint8_t k = 0; k < dev->depth; k++)
    usb_diag_printf(d, "  ");
  usb_diag_printf(d,
                  "path=%s depth=%u addr=%u port=%u speed=%s connected=%u "
                  "configured=%u vid=0x%04X pid=0x%04X class=0x%02X "
                  "driver=%s\n",
                  path, dev->depth, dev->address, dev->root_port,
                  usb_speed_name(dev->speed), dev->connected ? 1 : 0,
                  dev->configured ? 1 : 0, dev->desc.vendor_id,
                  dev->desc.product_id, dev->desc.device_class, driver);
  if (dev->is_hub)
    usb_diag_printf(d, "    hub: ports=%u multi_tt=%u\n", dev->num_ports,
                    dev->multi_tt ? 1 : 0);
  if (dev->tt_hub) {
    char tt_path[32];
    usb_topo_format_path(tt_path, bus, dev->tt_hub);
    usb_diag_printf(d, "    tt: hub=%s port=%u\n", tt_path, dev->tt_port);
  }
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

  /* Depth is bounded by USB_MAX_DEPTH, so this recursion is too. */
  if (dev->depth >= USB_MAX_DEPTH)
    return;
  for (int i = 0; i < MAX_USB_DEVICES; i++) {
    const struct usb_device *child = devices[i];
    if (child && child->parent == dev)
      usb_core_diag_node(d, child, bus);
  }
}

void usb_core_diag(struct usb_diag *d) {
  usb_diag_printf(d, "USB core build: %s %s\n", __DATE__, __TIME__);
  usb_diag_printf(d, "USB devices (core, tree by topology path):\n");
  if (!device_count) {
    usb_diag_printf(d, "  none enumerated\n");
    return;
  }
  for (int i = 0; i < MAX_USB_DEVICES; i++) {
    const struct usb_device *dev = devices[i];
    if (!dev || dev->parent)
      continue;
    usb_core_diag_node(d, dev, usb_bus_number(dev->hcd));
  }
#ifdef USE_RUST_USB
  rust_usb_diag(d);
#endif
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

  /* Keep the last 256 matching lines.  The earliest mouse-probe decisions are
   * exactly what a failing real machine needs; 48 lines used to drop them.
   * The arrays are static because
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
    usb_audio_diag(&d);
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
