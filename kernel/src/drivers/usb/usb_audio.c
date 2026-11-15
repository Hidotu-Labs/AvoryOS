#include "usb_audio.h"
#include "usb.h"
#include "../audio/alsa_emu.h"
#include "../../console/klog.h"
#include "../../apic/lapic_timer.h"
#include "../../io/io.h"
#include "../../lock/spinlock.h"
#include "../../lib/string.h"
#include "../../mm/dma_alloc.h"
#include "../../mm/heap.h"
#include "../../sched/sched.h"
#include "../../sched/wait.h"
#include "../../smp/cpu.h"
#include "xhci.h"

#define USB_AUDIO_CONFIG_MAX 512
#define USB_AUDIO_PROBE_BATCH 16
#define USB_AUDIO_PROBE_BATCHES 20
#define USB_AUDIO_PROBE_QUEUED 128
#define USB_AUDIO_MAX_DEVICES 16
#define USB_AUDIO_DMA_SLOTS 40
#define USB_AUDIO_DMA_BYTES 8192
#define USB_AUDIO_MAX_INFLIGHT 32
#define USB_AUDIO_FRAMES_PER_PACKET 48
#define USB_AUDIO_FRAME_BYTES 4
#define USB_AUDIO_SOFT_FRAMES 32768
#define USB_DT_CS_INTERFACE 0x24
#define USB_DT_CS_ENDPOINT 0x25

static uint16_t audio_le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t audio_le24(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static void usb_audio_probe_poll(void) {
  io_wait();
  /* The boot probe runs before normal periodic USB polling is fully active.
   * Drain xHCI's event ring while waiting for transfer completions. */
  xhci_msix_watchdog();
}

struct audio_alt_state {
  bool is_streaming;
  uint8_t interface_number;
  uint8_t alternate;
  uint8_t declared_endpoints;
  uint8_t endpoint_count;
  bool has_general;
  bool has_format;
  uint8_t terminal_link;
  bool pcm_tag;
  bool stereo_s16_48k;
  bool has_endpoint;
  uint8_t endpoint_address;
  uint8_t endpoint_attributes;
  uint8_t interval;
  uint16_t max_packet;
};

struct usb_audio_state {
  spinlock_t lock;
  wait_queue_t wait_queue;
  struct usb_device *dev;
  struct usb_audio_playback_desc desc;
  int card_index;
  bool registered;
  bool attach_pending;
  bool detach_pending;
  bool opened;
  bool running;
  bool stopping;
  bool transitioning;
  bool drain_tail;
  struct usb_isoch_stream *stream;
  dma_buffer_t *dma_handle;
  uint8_t *dma_buffer;
  uint64_t dma_phys;
  struct usb_isoch_packet *packets;
  bool packet_active[USB_AUDIO_DMA_SLOTS];
  uint8_t packet_real_frames[USB_AUDIO_DMA_SLOTS];
  uint8_t *software_queue;
  uint32_t queue_read_frame;
  uint32_t queue_write_frame;
  uint32_t queued_frames;
  uint32_t inflight;
  uint64_t hardware_frames;
  uint64_t submitted_packets;
  uint64_t completed_packets;
  uint64_t transfer_errors;
  uint64_t underruns;
  uint64_t short_packets;
  uint8_t last_completion_code;
  uint16_t last_actual_length;
  uint8_t last_error_code;
  uint16_t last_error_actual_length;
};

static struct usb_audio_state usb_audio_states[USB_AUDIO_MAX_DEVICES];
static spinlock_t usb_audio_states_lock = SPINLOCK_INIT;
static bool usb_audio_states_initialized;
static bool usb_audio_worker_started;
static struct thread *usb_audio_worker_thread;

static void usb_audio_wake_worker(void) {
  struct thread *worker =
      __atomic_load_n(&usb_audio_worker_thread, __ATOMIC_ACQUIRE);
  if (worker && sched_get_current() != worker)
    sched_wakeup(worker);
}

static bool audio_alt_matches(const struct audio_alt_state *alt) {
  if (!alt->is_streaming || !alt->alternate || !alt->has_general ||
      !alt->has_format || !alt->pcm_tag ||
      !alt->stereo_s16_48k || !alt->has_endpoint ||
      alt->declared_endpoints != 1 || alt->endpoint_count != 1)
    return false;

  /* Full-speed isochronous OUT, synchronous or adaptive, one 1 ms service
   * interval, with one 192-byte S16 stereo frame payload.  Reject high-bandwidth
   * transaction bits and feedback/implicit-feedback usage. */
  uint8_t transfer_type = alt->endpoint_attributes & 0x03;
  uint8_t sync_type = (alt->endpoint_attributes >> 2) & 0x03;
  uint8_t usage_type = (alt->endpoint_attributes >> 4) & 0x03;
  return (alt->endpoint_address & 0x70) == 0 &&
         (alt->endpoint_address & 0x0f) != 0 &&
         !(alt->endpoint_address & 0x80) && transfer_type == 1 &&
         (sync_type == 2 || sync_type == 3) && usage_type == 0 &&
         alt->interval == 1 && alt->max_packet == 192;
}

bool usb_audio_parse_configuration(const uint8_t *bytes, uint16_t length,
                                   struct usb_audio_playback_desc *selected) {
  if (!bytes || !selected || length < 9 || bytes[0] < 9 || bytes[1] != 2)
    return false;

  uint16_t total = audio_le16(bytes + 2);
  if (total < bytes[0] || total != length)
    return false;

  bool in_ac = false;
  bool saw_ac_header = false;
  bool ac_header_valid = false;
  uint16_t ac_class_bytes = 0;
  uint16_t ac_declared_bytes = 0;
  bool ac_stream_terminal = false;
  uint8_t terminal_ids[16];
  uint8_t terminal_types[16];
  uint8_t terminal_count = 0;
  uint8_t collection_interfaces[16];
  uint8_t collection_count = 0;
  struct audio_alt_state alt;
  memset(&alt, 0, sizeof(alt));
  uint16_t offset = bytes[0];
  struct usb_audio_playback_desc found;
  bool have_found = false;

  while (offset < total) {
    if (total - offset < 2)
      return false;
    uint8_t dlen = bytes[offset];
    uint8_t dtype = bytes[offset + 1];
    if (dlen < 2 || dlen > total - offset)
      return false;

    if (dtype == 4) { /* Interface descriptor */
      if (dlen < 9)
        return false;
      if (in_ac) {
        if (ac_header_valid && ac_class_bytes != ac_declared_bytes)
          ac_header_valid = false;
        in_ac = false;
      }
      if (audio_alt_matches(&alt)) {
        found.configuration_value = bytes[5];
        found.interface_number = alt.interface_number;
        found.alternate_setting = alt.alternate;
        found.terminal_link = alt.terminal_link;
        found.endpoint_address = alt.endpoint_address;
        found.endpoint_attributes = alt.endpoint_attributes;
        found.interval = alt.interval;
        found.max_packet_size = alt.max_packet;
        have_found = true;
      }

      uint8_t klass = bytes[offset + 5];
      uint8_t subclass = bytes[offset + 6];
      uint8_t alternate = bytes[offset + 3];
      memset(&alt, 0, sizeof(alt));
      if (klass == 1 && subclass == 1) { /* AudioControl */
        in_ac = true;
        saw_ac_header = true;
        ac_header_valid = false;
        ac_class_bytes = 0;
        ac_declared_bytes = 0;
        ac_stream_terminal = false;
        terminal_count = 0;
      } else if (klass == 1 && subclass == 2) { /* AudioStreaming */
        alt.is_streaming = true;
        alt.interface_number = bytes[offset + 2];
        alt.alternate = alternate;
        alt.declared_endpoints = bytes[offset + 4];
      }
    } else if (dtype == USB_DT_CS_INTERFACE) {
      if (in_ac) {
        ac_class_bytes += dlen;
        if (dlen < 3)
          return false;
        uint8_t subtype = bytes[offset + 2];
        if (subtype == 1) { /* UAC1 AC header */
          if (dlen < 8 || audio_le16(bytes + offset + 3) != 0x0100)
            return false;
          uint8_t in_collection = bytes[offset + 7];
          if (dlen != (uint8_t)(8 + in_collection) ||
              in_collection > sizeof(collection_interfaces))
            return false;
          ac_declared_bytes = audio_le16(bytes + offset + 5);
          ac_header_valid = ac_declared_bytes >= dlen;
          collection_count = in_collection;
          for (uint8_t i = 0; i < collection_count; i++)
            collection_interfaces[i] = bytes[offset + 8 + i];
        } else if (subtype == 2 && dlen >= 12) { /* Input Terminal */
          uint8_t id = bytes[offset + 3];
          uint16_t type = audio_le16(bytes + offset + 4);
          if (terminal_count < sizeof(terminal_ids)) {
            terminal_ids[terminal_count] = id;
            terminal_types[terminal_count++] = type == 0x0101 ? 1 : 0;
          }
        }
      } else if (alt.is_streaming && alt.alternate) {
        if (dlen < 3)
          return false;
        uint8_t subtype = bytes[offset + 2];
        if (subtype == 1) { /* AS General */
          if (dlen < 7 || alt.has_general)
            return false;
          alt.has_general = true;
          alt.terminal_link = bytes[offset + 3];
          alt.pcm_tag = audio_le16(bytes + offset + 5) == 1;
        } else if (subtype == 2) { /* Format Type I */
          if (dlen < 8 || alt.has_format)
            return false;
          alt.has_format = true;
          uint8_t channels = bytes[offset + 4];
          uint8_t subframe = bytes[offset + 5];
          uint8_t bit_resolution = bytes[offset + 6];
          uint8_t frequency_type = bytes[offset + 7];
          bool exactly_48k = false;
          if (frequency_type == 1 && dlen == 11)
            exactly_48k = audio_le24(bytes + offset + 8) == 48000;
          alt.stereo_s16_48k = bytes[offset + 3] == 1 && channels == 2 &&
                               subframe == 2 && bit_resolution == 16 &&
                               exactly_48k;
        }
      }
    } else if (dtype == 5 && alt.is_streaming && alt.alternate) {
      if (dlen < 7)
        return false;
      alt.endpoint_count++;
      if (alt.endpoint_count == 1) {
        alt.has_endpoint = true;
        alt.endpoint_address = bytes[offset + 2];
        alt.endpoint_attributes = bytes[offset + 3];
        alt.max_packet = audio_le16(bytes + offset + 4);
        alt.interval = bytes[offset + 6];
      }
    } else if (dtype == USB_DT_CS_ENDPOINT && alt.is_streaming &&
               alt.alternate && dlen < 3) {
      return false;
    }
    offset += dlen;
  }

  if (in_ac && ac_header_valid && ac_class_bytes != ac_declared_bytes)
    ac_header_valid = false;
  if (audio_alt_matches(&alt)) {
    found.configuration_value = bytes[5];
    found.interface_number = alt.interface_number;
    found.alternate_setting = alt.alternate;
    found.terminal_link = alt.terminal_link;
    found.endpoint_address = alt.endpoint_address;
    found.endpoint_attributes = alt.endpoint_attributes;
    found.interval = alt.interval;
    found.max_packet_size = alt.max_packet;
    have_found = true;
  }

  /* Verify that the AS terminal link names the UAC1 USB-streaming input
   * terminal from the same AudioControl interface.  The class-specific AC
   * descriptor totals are also checked above, so broken class subtrees fail
   * closed rather than being mistaken for a playback interface. */
  if (!saw_ac_header || !ac_header_valid)
    have_found = false;
  if (have_found)
    for (uint8_t i = 0; i < terminal_count; i++)
      if (terminal_ids[i] == found.terminal_link && terminal_types[i])
        ac_stream_terminal = true;
  bool in_collection = false;
  if (have_found)
    for (uint8_t i = 0; i < collection_count; i++)
      if (collection_interfaces[i] == found.interface_number)
        in_collection = true;
  if (!ac_stream_terminal || !in_collection)
    have_found = false;

  if (have_found)
    *selected = found;
  return have_found;
}

bool usb_audio_discover(struct usb_device *dev,
                        struct usb_audio_playback_desc *selected) {
  if (!dev || !selected || dev->speed != USB_SPEED_FULL || !dev->connected)
    return false;
  uint8_t config[USB_AUDIO_CONFIG_MAX];
  struct usb_control_request req = {
      .request_type = 0x80,
      .request = USB_REQ_GET_DESCRIPTOR,
      .value = (USB_DESC_CONFIGURATION << 8),
      .index = 0,
      .length = 9,
  };
  if (usb_control_transfer(dev, &req, config, 9) < 0 || config[0] < 9 ||
      config[1] != USB_DESC_CONFIGURATION)
    return false;
  uint16_t total = audio_le16(config + 2);
  if (total < config[0] || total > sizeof(config))
    return false;
  req.length = total;
  if (usb_control_transfer(dev, &req, config, total) < 0)
    return false;
  return usb_audio_parse_configuration(config, total, selected);
}

void usb_audio_selftest(void) {
  /* QEMU usb-audio UAC1 configuration, captured at full speed. */
  static const uint8_t valid[] = {
      0x09,0x02,0x71,0x00,0x02,0x01,0x04,0xc0,0x32,
      0x09,0x04,0x00,0x00,0x00,0x01,0x01,0x00,0x05,
      0x09,0x24,0x01,0x00,0x01,0x2b,0x00,0x01,0x01,
      0x0c,0x24,0x02,0x01,0x01,0x01,0x00,0x02,0x03,0x00,0x00,0x06,
      0x0d,0x24,0x06,0x02,0x01,0x02,0x01,0x00,0x02,0x00,0x02,0x00,0x07,
      0x09,0x24,0x03,0x03,0x01,0x03,0x00,0x02,0x08,
      0x09,0x04,0x01,0x00,0x00,0x01,0x02,0x00,0x09,
      0x09,0x04,0x01,0x01,0x01,0x01,0x02,0x00,0x0a,
      0x07,0x24,0x01,0x01,0x00,0x01,0x00,
      0x0b,0x24,0x02,0x01,0x02,0x02,0x10,0x01,0x80,0xbb,0x00,
      0x09,0x05,0x01,0x0d,0xc0,0x00,0x01,0x00,0x00,
      0x07,0x25,0x01,0x00,0x00,0x00,0x00,
  };
  struct usb_audio_playback_desc selection;
  bool accepted = usb_audio_parse_configuration(valid, sizeof(valid),
                                                &selection);
  bool ok = accepted && selection.interface_number == 1 &&
            selection.alternate_setting == 1 &&
            selection.endpoint_address == 1 &&
            selection.max_packet_size == 192;

  uint8_t adaptive[sizeof(valid)];
  memcpy(adaptive, valid, sizeof(valid));
  adaptive[100] = 0x09; /* Adaptive synchronization is also supported. */
  ok = ok && usb_audio_parse_configuration(adaptive, sizeof(adaptive),
                                            &selection);

  uint8_t malformed[sizeof(valid)];
  memcpy(malformed, valid, sizeof(valid));
  malformed[9 + 9 + 1] = 0; /* invalid descriptor type inside AudioControl */
  ok = ok && !usb_audio_parse_configuration(malformed, sizeof(malformed),
                                             &selection);
  memcpy(malformed, valid, sizeof(valid));
  malformed[94] = 0x44; /* 44.1 kHz is outside the supported format. */
  malformed[95] = 0xac;
  ok = ok && !usb_audio_parse_configuration(malformed, sizeof(malformed),
                                             &selection);
  memcpy(malformed, valid, sizeof(valid));
  malformed[103] = 2; /* Full-speed interval must be one frame. */
  ok = ok && !usb_audio_parse_configuration(malformed, sizeof(malformed),
                                             &selection);
  memcpy(malformed, valid, sizeof(valid));
  malformed[sizeof(valid) - 14] = 0x81; /* IN endpoint is not playback OUT. */
  ok = ok && !usb_audio_parse_configuration(malformed, sizeof(malformed),
                                             &selection);
  klog_puts(ok ? "[USB-AUDIO-TEST] descriptor parser PASS\n"
               : "[USB-AUDIO-TEST] descriptor parser FAIL\n");
}

static bool usb_audio_probe_requested(void) {
  extern const char *kernel_boot_cmdline;
  if (!kernel_boot_cmdline)
    return false;
  const char *p = kernel_boot_cmdline;
  const char *option = "usb_audio_probe=1";
  size_t option_len = strlen(option);
  while ((p = strstr(p, option)) != NULL) {
    if ((p == kernel_boot_cmdline || p[-1] == ' ') &&
        (p[option_len] == '\0' || p[option_len] == ' '))
      return true;
    p += option_len;
  }
  return false;
}

static int usb_audio_set_interface(struct usb_device *dev, uint8_t iface,
                                   uint8_t alternate) {
  struct usb_control_request req = {
      .request_type = 0x01, /* OUT | standard | interface */
      .request = 0x0b,      /* SET_INTERFACE */
      .value = alternate,
      .index = iface,
      .length = 0,
  };
  return usb_control_transfer(dev, &req, NULL, 0);
}

static void usb_audio_smoke_restore(struct usb_device *dev,
                                    const struct usb_audio_playback_desc *desc) {
  (void)usb_audio_set_interface(dev, desc->interface_number, 0);
  struct usb_control_request req = {
      .request_type = 0x00,
      .request = USB_REQ_SET_CONFIGURATION,
      .value = 0,
      .index = 0,
      .length = 0,
  };
  (void)usb_control_transfer(dev, &req, NULL, 0);
  dev->configured = false;
  dev->configuration_value = 0;
}

void usb_audio_probe_smoke(struct usb_device *dev) {
  if (!usb_audio_probe_requested() || !dev || !dev->connected ||
      dev->speed != USB_SPEED_FULL || dev->desc.vendor_id != 0x46f4 ||
      dev->desc.product_id != 0x0002)
    return;

  struct usb_audio_playback_desc desc;
  if (!usb_audio_discover(dev, &desc)) {
    klog_puts("[USB-AUDIO-PROBE] pinned QEMU device did not match descriptor contract\n");
    return;
  }
  if (!dev->hcd || !dev->hcd->isoch_open || !dev->hcd->isoch_submit ||
      !dev->hcd->isoch_cancel) {
    klog_puts("[USB-AUDIO-PROBE] HCD lacks isochronous transport\n");
    return;
  }

  klog_puts("[USB-AUDIO-PROBE] starting boot-context transport smoke\n");
  struct usb_control_request req = {
      .request_type = 0x00,
      .request = USB_REQ_SET_CONFIGURATION,
      .value = desc.configuration_value,
      .index = 0,
      .length = 0,
  };
  if (usb_control_transfer(dev, &req, NULL, 0) < 0) {
    klog_puts("[USB-AUDIO-PROBE] SET_CONFIGURATION failed\n");
    return;
  }
  dev->configured = true;
  dev->configuration_value = desc.configuration_value;
  if (usb_audio_set_interface(dev, desc.interface_number,
                              desc.alternate_setting) < 0) {
    klog_puts("[USB-AUDIO-PROBE] SET_INTERFACE failed\n");
    usb_audio_smoke_restore(dev, &desc);
    return;
  }

  uint64_t buffer_phys = 0;
  uint8_t *buffer = dma_alloc_page_flags(DMA_FLAG_32BIT, &buffer_phys);
  if (!buffer || !buffer_phys) {
    klog_puts("[USB-AUDIO-PROBE] DMA buffer allocation failed\n");
    if (buffer)
      dma_free_page(buffer);
    usb_audio_smoke_restore(dev, &desc);
    return;
  }
  memset(buffer, 0, 4096);
  struct usb_isoch_packet *packets =
      kmalloc(sizeof(*packets) * USB_AUDIO_PROBE_QUEUED);
  if (!packets) {
    klog_puts("[USB-AUDIO-PROBE] packet metadata allocation failed\n");
    dma_free_page(buffer);
    usb_audio_smoke_restore(dev, &desc);
    return;
  }
  memset(packets, 0, sizeof(*packets) * USB_AUDIO_PROBE_QUEUED);
  struct usb_isoch_stream *stream =
      usb_isoch_open(dev, desc.endpoint_address, desc.max_packet_size,
                     desc.interval);
  bool ok = stream != NULL;
  bool safe_to_free = true;
  bool stream_open = stream != NULL;
  if (ok) {
    packets[0].buffer = buffer;
    packets[0].buffer_phys = buffer_phys;
    packets[0].length = desc.max_packet_size;
    if (usb_isoch_submit(stream, &packets[0]) < 0) {
      klog_puts("[USB-AUDIO-PROBE] initial submit failed\n");
      ok = false;
    } else {
      uint64_t deadline = lapic_timer_get_ms() + 100;
      while (!usb_isoch_completed(&packets[0]) &&
             lapic_timer_get_ms() < deadline)
        usb_audio_probe_poll();
      ok = usb_isoch_completed(&packets[0]) &&
           packets[0].completion_code == 1 &&
           packets[0].actual_length == desc.max_packet_size;
      if (!ok)
        klogf("[USB-AUDIO-PROBE] initial packet timeout=%u cc=%u actual=%u\n",
              usb_isoch_completed(&packets[0]) ? 0 : 1,
              packets[0].completion_code, packets[0].actual_length);
    }
  }

  if (ok) {
      for (uint32_t batch = 0; batch < USB_AUDIO_PROBE_BATCHES && ok;
           batch++) {
        memset(packets, 0, sizeof(*packets) * USB_AUDIO_PROBE_BATCH);
        uint32_t submitted = 0;
        for (uint32_t i = 0; i < USB_AUDIO_PROBE_BATCH; i++) {
          packets[i].buffer = buffer + i * desc.max_packet_size;
          packets[i].buffer_phys = buffer_phys + i * desc.max_packet_size;
          packets[i].length = desc.max_packet_size;
          if (usb_isoch_submit(stream, &packets[i]) < 0) {
            klogf("[USB-AUDIO-PROBE] batch=%u submit-index=%u failed\n",
                  batch, i);
            ok = false;
            break;
          }
          submitted++;
        }
        uint64_t deadline = lapic_timer_get_ms() + 100;
        bool all_completed = false;
        while (ok && submitted && !all_completed &&
               lapic_timer_get_ms() < deadline) {
          all_completed = true;
          for (uint32_t i = 0; i < submitted; i++)
            if (!usb_isoch_completed(&packets[i])) {
              all_completed = false;
              break;
            }
          if (!all_completed)
            usb_audio_probe_poll();
        }
        if (!all_completed) {
          ok = false;
          klogf("[USB-AUDIO-PROBE] batch=%u timeout submitted=%u completed=",
                batch, submitted);
          uint32_t completed = 0;
          for (uint32_t i = 0; i < submitted; i++)
            if (usb_isoch_completed(&packets[i]))
              completed++;
          klogf("%u\n", completed);
        }
        for (uint32_t i = 0; i < submitted; i++)
          if (!usb_isoch_completed(&packets[i]) ||
              packets[i].completion_code != 1 ||
              packets[i].actual_length != desc.max_packet_size) {
            ok = false;
            klogf("[USB-AUDIO-PROBE] batch=%u index=%u completed=%u cc=%u actual=%u\n",
                  batch, i, usb_isoch_completed(&packets[i]) ? 1 : 0,
                  packets[i].completion_code, packets[i].actual_length);
          }
      }

      /* Queue a long tail and stop immediately. The completion/cancel paths
       * must settle every caller-owned descriptor before this metadata goes
       * away. */
      memset(packets, 0, sizeof(*packets) * USB_AUDIO_PROBE_QUEUED);
      uint32_t queued = 0;
      if (!ok)
        klog_puts("[USB-AUDIO-PROBE] skip queued tail after batch failure\n");
      if (ok) {
        for (uint32_t i = 0; i < USB_AUDIO_PROBE_QUEUED; i++) {
          /* The zeroed silence payload is immutable, so pending OUT TDs can
           * safely point to the same caller-owned DMA range. */
          packets[i].buffer = buffer;
          packets[i].buffer_phys = buffer_phys;
          packets[i].length = desc.max_packet_size;
          if (usb_isoch_submit(stream, &packets[i]) < 0) {
            klogf("[USB-AUDIO-PROBE] queued submit-index=%u failed\n", i);
            ok = false;
            break;
          }
          queued++;
        }
      }
      uint32_t pending_before_cancel = 0;
      for (uint32_t i = 0; i < queued; i++)
        if (!usb_isoch_completed(&packets[i]))
          pending_before_cancel++;
      if (pending_before_cancel == 0)
        ok = false;
      klogf("[USB-AUDIO-PROBE] queued-cancel owners=%u pending-before-stop=%u\n",
            queued, pending_before_cancel);
  }

  /* Cancel the first endpoint only after its ring has wrapped and the queued
   * tail is in flight. */
  if (stream_open) {
    if (!usb_isoch_cancel(stream)) {
      safe_to_free = false;
      ok = false;
      klog_puts("[USB-AUDIO-PROBE] first stream cancel failed; retaining DMA owners\n");
    } else {
      stream = NULL;
      stream_open = false;
      for (uint32_t i = 0; i < USB_AUDIO_PROBE_QUEUED; i++)
        if (packets[i].buffer && !usb_isoch_completed(&packets[i]))
          ok = false;
    }
  }

  /* Reopen after a completed cancel and prove the endpoint can transfer again. */
  if (ok && safe_to_free) {
    stream = usb_isoch_open(dev, desc.endpoint_address, desc.max_packet_size,
                            desc.interval);
    stream_open = stream != NULL;
    if (!stream_open) {
      klog_puts("[USB-AUDIO-PROBE] reopen failed\n");
      ok = false;
    } else {
      memset(packets, 0, sizeof(*packets));
      packets[0].buffer = buffer;
      packets[0].buffer_phys = buffer_phys;
      packets[0].length = desc.max_packet_size;
      if (usb_isoch_submit(stream, &packets[0]) < 0) {
        klog_puts("[USB-AUDIO-PROBE] post-reopen submit failed\n");
        ok = false;
      } else {
        uint64_t deadline = lapic_timer_get_ms() + 100;
        while (!usb_isoch_completed(&packets[0]) &&
               lapic_timer_get_ms() < deadline)
          usb_audio_probe_poll();
        if (!usb_isoch_completed(&packets[0]) ||
            packets[0].completion_code != 1 ||
            packets[0].actual_length != desc.max_packet_size) {
          klogf("[USB-AUDIO-PROBE] post-reopen timeout=%u cc=%u actual=%u\n",
                usb_isoch_completed(&packets[0]) ? 0 : 1,
                packets[0].completion_code, packets[0].actual_length);
          ok = false;
        }
      }
      if (!usb_isoch_cancel(stream)) {
        safe_to_free = false;
        ok = false;
        klog_puts("[USB-AUDIO-PROBE] post-reopen cancel failed; retaining DMA owners\n");
      } else {
        stream = NULL;
        stream_open = false;
      }
    }
  }

  if (safe_to_free) {
    dma_free_page(buffer);
    kfree(packets);
    usb_audio_smoke_restore(dev, &desc);
  } else {
    klog_puts("[USB-AUDIO-PROBE] device state left active because DMA may still be owned\n");
  }
  klog_puts(ok ? "[USB-AUDIO-PROBE] PASS submit/reap/320-packet-ring-wrap/reopen/queued-cancel\n"
               : "[USB-AUDIO-PROBE] FAIL submit/reap/320-packet-ring-wrap/reopen/queued-cancel\n");
}

static struct usb_audio_state *usb_audio_state_for(struct usb_device *dev) {
  struct usb_audio_state *found = NULL;
  spinlock_acquire(&usb_audio_states_lock);
  for (uint32_t i = 0; i < USB_AUDIO_MAX_DEVICES; i++) {
    struct usb_audio_state *state = &usb_audio_states[i];
    spinlock_acquire(&state->lock);
    bool matches = state->dev == dev;
    spinlock_release(&state->lock);
    if (matches) {
      found = state;
      break;
    }
  }
  spinlock_release(&usb_audio_states_lock);
  return found;
}

void usb_audio_device_discovered(
    struct usb_device *dev,
    const struct usb_audio_playback_desc *selected) {
  if (!dev || !selected || !dev->hcd || !dev->hcd->isoch_open ||
      !dev->hcd->isoch_submit || !dev->hcd->isoch_cancel)
    return;
  spinlock_acquire(&usb_audio_states_lock);
  if (!usb_audio_states_initialized) {
    for (uint32_t i = 0; i < USB_AUDIO_MAX_DEVICES; i++) {
      spinlock_init(&usb_audio_states[i].lock);
      wait_queue_init(&usb_audio_states[i].wait_queue);
      usb_audio_states[i].card_index = -1;
    }
    usb_audio_states_initialized = true;
  }
  struct usb_audio_state *state = NULL;
  for (uint32_t i = 0; i < USB_AUDIO_MAX_DEVICES; i++) {
    spinlock_acquire(&usb_audio_states[i].lock);
    bool same = usb_audio_states[i].dev == dev;
    bool empty = usb_audio_states[i].dev == NULL;
    spinlock_release(&usb_audio_states[i].lock);
    if (same) {
      state = &usb_audio_states[i];
      break;
    }
    if (!state && empty)
      state = &usb_audio_states[i];
  }
  if (state) {
    spinlock_acquire(&state->lock);
    if (!state->dev) {
      state->card_index = -1;
      state->dev = dev;
      state->registered = false;
      state->opened = false;
      state->running = false;
      state->stopping = false;
      state->transitioning = false;
      state->detach_pending = false;
      state->dma_buffer = NULL;
      state->dma_phys = 0;
      state->packets = NULL;
      state->software_queue = NULL;
      state->inflight = 0;
      state->queued_frames = 0;
      state->hardware_frames = 0;
      state->submitted_packets = 0;
      state->completed_packets = 0;
      state->transfer_errors = 0;
      state->underruns = 0;
      state->short_packets = 0;
      state->last_completion_code = 0;
      state->last_actual_length = 0;
      state->last_error_code = 0;
      state->last_error_actual_length = 0;
    }
    state->desc = *selected;
    if (!state->registered)
      state->attach_pending = true;
    spinlock_release(&state->lock);
  } else {
    klog_puts("[USB-AUDIO] device limit reached; playback card not registered\n");
  }
  spinlock_release(&usb_audio_states_lock);
  if (state)
    usb_audio_wake_worker();
}

void usb_audio_device_removed(struct usb_device *dev) {
  struct usb_audio_state *state = usb_audio_state_for(dev);
  if (!state)
    return;
  spinlock_acquire(&state->lock);
  if (state->dev == dev) {
    state->running = false;
    state->detach_pending = true;
    state->attach_pending = false;
  }
  spinlock_release(&state->lock);
  usb_audio_wake_worker();
}

static bool usb_audio_transition_begin(struct usb_audio_state *state) {
  spinlock_acquire(&state->lock);
  bool available = !state->transitioning;
  if (available)
    state->transitioning = true;
  spinlock_release(&state->lock);
  return available;
}

static void usb_audio_transition_end(struct usb_audio_state *state) {
  spinlock_acquire(&state->lock);
  state->transitioning = false;
  spinlock_release(&state->lock);
}

static int usb_audio_backend_open(void *ctx, uint32_t rate, uint8_t channels,
                                 uint8_t bits) {
  struct usb_audio_state *state = ctx;
  if (!state || rate != 48000 || channels != 2 || bits != 16 ||
      !usb_audio_transition_begin(state))
    return -1;
  spinlock_acquire(&state->lock);
  struct usb_device *dev = state->dev;
  struct usb_audio_playback_desc desc = state->desc;
  bool already_open = state->opened;
  spinlock_release(&state->lock);
  if (already_open || !dev || !dev->connected || !dev->hcd ||
      !dev->hcd->isoch_open || !dev->hcd->isoch_submit ||
      !dev->hcd->isoch_cancel) {
    usb_audio_transition_end(state);
    return -1;
  }

  dma_buffer_t *dma_handle = dma_alloc(USB_AUDIO_DMA_BYTES, DMA_FLAG_32BIT);
  uint64_t dma_phys = dma_handle ? dma_handle->phys : 0;
  uint8_t *dma_buffer = dma_handle ? (uint8_t *)dma_handle->virt : NULL;
  struct usb_isoch_packet *packets =
      kmalloc(sizeof(*packets) * USB_AUDIO_DMA_SLOTS);
  uint8_t *software_queue = kmalloc(
      (size_t)USB_AUDIO_SOFT_FRAMES * USB_AUDIO_FRAME_BYTES);
  if (!dma_handle || !dma_buffer || !dma_phys || !packets || !software_queue) {
    if (dma_handle)
      dma_free(dma_handle);
    if (packets)
      kfree(packets);
    if (software_queue)
      kfree(software_queue);
    usb_audio_transition_end(state);
    return -1;
  }
  memset(dma_buffer, 0, USB_AUDIO_DMA_BYTES);
  memset(packets, 0, sizeof(*packets) * USB_AUDIO_DMA_SLOTS);

  struct usb_control_request req = {
      .request_type = 0x00,
      .request = USB_REQ_SET_CONFIGURATION,
      .value = desc.configuration_value,
      .index = 0,
      .length = 0,
  };
  bool configured = usb_control_transfer(dev, &req, NULL, 0) == 0;
  if (configured) {
    dev->configured = true;
    dev->configuration_value = desc.configuration_value;
    configured = dev->connected &&
        usb_audio_set_interface(dev, desc.interface_number,
                                desc.alternate_setting) == 0;
  }
  struct usb_isoch_stream *stream = NULL;
  if (configured && dev->connected)
    stream = usb_isoch_open(dev, desc.endpoint_address, desc.max_packet_size,
                            desc.interval);
  if (!stream) {
    if (dev->connected)
      usb_audio_smoke_restore(dev, &desc);
    dma_free(dma_handle);
    kfree(packets);
    kfree(software_queue);
    usb_audio_transition_end(state);
    return -1;
  }

  spinlock_acquire(&state->lock);
  state->dma_handle = dma_handle;
  state->dma_buffer = dma_buffer;
  state->dma_phys = dma_phys;
  state->packets = packets;
  state->software_queue = software_queue;
  state->stream = stream;
  state->opened = true;
  state->running = false;
  state->stopping = false;
  state->drain_tail = false;
  state->queue_read_frame = 0;
  state->queue_write_frame = 0;
  state->queued_frames = 0;
  state->inflight = 0;
  state->hardware_frames = 0;
  memset(state->packet_active, 0, sizeof(state->packet_active));
  memset(state->packet_real_frames, 0, sizeof(state->packet_real_frames));
  state->submitted_packets = 0;
  state->completed_packets = 0;
  state->transfer_errors = 0;
  state->underruns = 0;
  state->short_packets = 0;
  state->last_completion_code = 0;
  state->last_actual_length = 0;
  state->last_error_code = 0;
  state->last_error_actual_length = 0;
  state->transitioning = false;
  spinlock_release(&state->lock);
  klogf("[USB-AUDIO] opened UAC1 card=%d 48000Hz stereo S16_LE ep=0x%02X\n",
        state->card_index, desc.endpoint_address);
  return 0;
}

static uint32_t usb_audio_backend_write(void *ctx, const void *samples,
                                        uint32_t frames) {
  struct usb_audio_state *state = ctx;
  if (!state || !samples || !frames)
    return 0;
  spinlock_acquire(&state->lock);
  if (!state->opened || !state->dev || !state->dev->connected ||
      !state->software_queue) {
    spinlock_release(&state->lock);
    return 0;
  }
  uint32_t capacity = USB_AUDIO_SOFT_FRAMES;
  uint32_t accepted = frames;
  if (accepted > capacity - state->queued_frames)
    accepted = capacity - state->queued_frames;
  uint32_t write_frame = state->queue_write_frame;
  uint32_t first = accepted;
  if (first > capacity - write_frame)
    first = capacity - write_frame;
  memcpy(state->software_queue + write_frame * USB_AUDIO_FRAME_BYTES,
         samples, first * USB_AUDIO_FRAME_BYTES);
  if (accepted > first)
    memcpy(state->software_queue,
           (const uint8_t *)samples + first * USB_AUDIO_FRAME_BYTES,
           (accepted - first) * USB_AUDIO_FRAME_BYTES);
  state->queue_write_frame = (write_frame + accepted) % capacity;
  state->queued_frames += accepted;
  spinlock_release(&state->lock);
  return accepted;
}

static uint64_t usb_audio_backend_pointer(void *ctx) {
  struct usb_audio_state *state = ctx;
  return state ? __atomic_load_n(&state->hardware_frames, __ATOMIC_ACQUIRE) : 0;
}

static bool usb_audio_backend_start(void *ctx) {
  struct usb_audio_state *state = ctx;
  if (!state || !usb_audio_transition_begin(state))
    return false;
  spinlock_acquire(&state->lock);
  bool can_start = state->opened && state->dev && state->dev->connected;
  struct usb_device *dev = state->dev;
  struct usb_audio_playback_desc desc = state->desc;
  bool need_open = state->stream == NULL;
  spinlock_release(&state->lock);
  struct usb_isoch_stream *stream = NULL;
  if (can_start && need_open)
    stream = usb_isoch_open(dev, desc.endpoint_address, desc.max_packet_size,
                            desc.interval);
  if (need_open && !stream)
    can_start = false;
  spinlock_acquire(&state->lock);
  if (can_start) {
    if (stream)
      state->stream = stream;
    state->running = true;
    state->stopping = false;
  }
  state->transitioning = false;
  spinlock_release(&state->lock);
  if (can_start)
    klogf("[USB-AUDIO] stream started card=%d\n", state->card_index);
  if (can_start)
    usb_audio_wake_worker();
  return can_start;
}

static bool usb_audio_backend_stop(void *ctx) {
  struct usb_audio_state *state = ctx;
  if (!state || !usb_audio_transition_begin(state))
    return false;
  spinlock_acquire(&state->lock);
  state->running = false;
  state->stopping = true;
  struct usb_isoch_stream *stream = state->stream;
  spinlock_release(&state->lock);
  bool safe = !stream || usb_isoch_cancel(stream);
  spinlock_acquire(&state->lock);
  if (safe) {
    state->stream = NULL;
    state->inflight = 0;
    state->queued_frames = 0;
    state->queue_read_frame = state->queue_write_frame;
    state->drain_tail = false;
    memset(state->packet_active, 0, sizeof(state->packet_active));
    memset(state->packet_real_frames, 0, sizeof(state->packet_real_frames));
    state->stopping = false;
  }
  state->transitioning = false;
  spinlock_release(&state->lock);
  if (!safe)
    klogf("[USB-AUDIO] stop could not revoke DMA card=%d; buffers retained\n",
          state->card_index);
  else
    klogf("[USB-AUDIO] stream stopped card=%d completed=%llu submitted=%llu "
          "errors=%llu underruns=%llu queued=%u hw=%llu "
          "last_cc=%u actual=%u/%u last_error=%u actual=%u short=%llu\n",
          state->card_index, (unsigned long long)state->completed_packets,
          (unsigned long long)state->submitted_packets,
          (unsigned long long)state->transfer_errors,
          (unsigned long long)state->underruns, state->queued_frames,
          (unsigned long long)state->hardware_frames,
          state->last_completion_code, state->last_actual_length,
          state->desc.max_packet_size, state->last_error_code,
          state->last_error_actual_length,
          (unsigned long long)state->short_packets);
  usb_audio_wake_worker();
  return safe;
}

static bool usb_audio_backend_drain(void *ctx) {
  struct usb_audio_state *state = ctx;
  if (!state)
    return false;
  spinlock_acquire(&state->lock);
  bool can_drain = state->opened && state->running && !state->stopping &&
                   state->stream != NULL;
  if (can_drain)
    state->drain_tail = true;
  spinlock_release(&state->lock);
  return can_drain;
}

static void usb_audio_backend_close(void *ctx) {
  struct usb_audio_state *state = ctx;
  if (!state || !usb_audio_transition_begin(state))
    return;
  spinlock_acquire(&state->lock);
  state->running = false;
  struct usb_isoch_stream *stream = state->stream;
  struct usb_device *dev = state->dev;
  struct usb_audio_playback_desc desc = state->desc;
  spinlock_release(&state->lock);
  if (stream && !usb_isoch_cancel(stream)) {
    usb_audio_transition_end(state);
    klogf("[USB-AUDIO] close retained DMA after cancel failure card=%d\n",
          state->card_index);
    return;
  }
  if (dev && dev->connected)
    usb_audio_smoke_restore(dev, &desc);

  spinlock_acquire(&state->lock);
  dma_buffer_t *dma_handle = state->dma_handle;
  struct usb_isoch_packet *packets = state->packets;
  uint8_t *software_queue = state->software_queue;
  state->stream = NULL;
  state->opened = false;
  state->stopping = false;
  state->dma_handle = NULL;
  state->dma_buffer = NULL;
  state->dma_phys = 0;
  state->packets = NULL;
  state->software_queue = NULL;
  state->inflight = 0;
  state->queued_frames = 0;
  state->transitioning = false;
  spinlock_release(&state->lock);
  (void)stream; /* A successful cancel retires the endpoint handle. */
  if (dma_handle)
    dma_free(dma_handle);
  if (packets)
    kfree(packets);
  if (software_queue)
    kfree(software_queue);
}

static void *usb_audio_backend_wait_queue(void *ctx) {
  struct usb_audio_state *state = ctx;
  return state ? &state->wait_queue : NULL;
}

static const struct alsa_pcm_backend_ops usb_audio_backend_ops = {
    .open = usb_audio_backend_open,
    .write = usb_audio_backend_write,
    .pointer = usb_audio_backend_pointer,
    .start = usb_audio_backend_start,
    .drain = usb_audio_backend_drain,
    .stop = usb_audio_backend_stop,
    .close = usb_audio_backend_close,
    .wait_queue = usb_audio_backend_wait_queue,
};

static void usb_audio_worker_reconcile(void) {
  for (uint32_t i = 0; i < USB_AUDIO_MAX_DEVICES; i++) {
    struct usb_audio_state *state = &usb_audio_states[i];
    spinlock_acquire(&state->lock);
    struct usb_device *dev = state->dev;
    bool attached = state->registered;
    bool attach = state->attach_pending;
    bool detach = state->detach_pending;
    bool transition = state->transitioning;
    int card_index = state->card_index;
    if (transition) {
      spinlock_release(&state->lock);
      continue;
    }
    if (attach && !attached && dev && dev->connected)
      state->transitioning = true;
    else
      state->transitioning = false;
    spinlock_release(&state->lock);

    if (detach && attached) {
      bool removed = alsa_emu_unregister_pcm_card(card_index);
      spinlock_acquire(&state->lock);
      state->transitioning = false;
      if (removed) {
        state->registered = false;
        state->card_index = -1;
        state->detach_pending = false;
        state->attach_pending = false;
        state->dev = NULL;
      }
      spinlock_release(&state->lock);
      if (removed)
        klogf("[USB-AUDIO] playback card=%d removed\n", card_index);
      else
        klogf("[USB-AUDIO] card removal deferred; backend stop still owns DMA card=%d\n",
              card_index);
      continue;
    }
    if (detach && !attached) {
      spinlock_acquire(&state->lock);
      state->detach_pending = false;
      state->attach_pending = false;
      state->dev = NULL;
      state->transitioning = false;
      spinlock_release(&state->lock);
      continue;
    }
    if (attach && !attached && dev && dev->connected) {
      char name[32];
      snprintf(name, sizeof(name), "USB Audio %04X:%04X p%u",
               dev->desc.vendor_id, dev->desc.product_id, dev->port + 1);
      int index = alsa_emu_register_pcm_card(name, state,
                                              &usb_audio_backend_ops);
      spinlock_acquire(&state->lock);
      state->transitioning = false;
      if (index >= 0 && dev->connected) {
        state->registered = true;
        state->card_index = index;
        state->attach_pending = false;
        klogf("[USB-AUDIO] playback card registered card=%d %s\n", index,
              name);
      } else if (index >= 0) {
        /* A removal raced registration. Keep the card visible only long
         * enough for the next worker pass to unregister it safely. */
        state->registered = true;
        state->card_index = index;
        state->detach_pending = true;
        state->attach_pending = false;
      }
      spinlock_release(&state->lock);
    }
  }
}

void usb_audio_poll(void) {
  for (uint32_t n = 0; n < USB_AUDIO_MAX_DEVICES; n++) {
    struct usb_audio_state *state = &usb_audio_states[n];
    bool completed_any = false;
    int notify_card = -1;
    spinlock_acquire(&state->lock);
    if (!state->opened || !state->running || state->stopping ||
        state->transitioning || !state->stream || !state->dev ||
        !state->dev->connected || !state->packets || !state->dma_buffer ||
        !state->software_queue) {
      spinlock_release(&state->lock);
      continue;
    }
    for (uint32_t i = 0; i < USB_AUDIO_DMA_SLOTS; i++) {
      if (!state->packet_active[i] ||
          !usb_isoch_completed(&state->packets[i]))
        continue;
      state->last_completion_code = state->packets[i].completion_code;
      state->last_actual_length = state->packets[i].actual_length;
      if (state->packets[i].completion_code == 1 &&
          state->packets[i].actual_length == state->desc.max_packet_size) {
        state->completed_packets++;
      } else {
        state->transfer_errors++;
        state->last_error_code = state->packets[i].completion_code;
        state->last_error_actual_length = state->packets[i].actual_length;
        if (state->packets[i].completion_code == 13)
          state->short_packets++;
      }
      state->hardware_frames += state->packet_real_frames[i];
      state->packet_active[i] = false;
      state->packet_real_frames[i] = 0;
      if (state->inflight)
        state->inflight--;
      completed_any = true;
    }
    if (completed_any && state->registered)
      notify_card = state->card_index;
    /* Pump newly published mmap frames before refilling the xHCI queue so we
     * don't emit a silence/underrun packet while userspace frames are ready. */
    if (notify_card >= 0) {
      spinlock_release(&state->lock);
      alsa_emu_pcm_notify(notify_card);
      spinlock_acquire(&state->lock);
      if (!state->opened || !state->running || state->stopping ||
          state->transitioning || !state->stream || !state->dev ||
          !state->dev->connected || !state->packets || !state->dma_buffer ||
          !state->software_queue) {
        spinlock_release(&state->lock);
        continue;
      }
    }
    while (state->inflight < USB_AUDIO_MAX_INFLIGHT) {
      uint32_t slot = USB_AUDIO_DMA_SLOTS;
      for (uint32_t i = 0; i < USB_AUDIO_DMA_SLOTS; i++)
        if (!state->packet_active[i]) {
          slot = i;
          break;
        }
      if (slot == USB_AUDIO_DMA_SLOTS)
        break;

      uint8_t *packet_buffer = state->dma_buffer + slot * 192;
      bool has_audio = state->queued_frames >= USB_AUDIO_FRAMES_PER_PACKET;
      bool tail_audio = !has_audio && state->drain_tail &&
                        state->queued_frames != 0;
      uint32_t real_frames = has_audio ? USB_AUDIO_FRAMES_PER_PACKET
                                       : tail_audio ? state->queued_frames : 0;
      uint32_t read_frame = state->queue_read_frame;
      if (real_frames) {
        uint32_t first = real_frames;
        if (first > USB_AUDIO_SOFT_FRAMES - read_frame)
          first = USB_AUDIO_SOFT_FRAMES - read_frame;
        memcpy(packet_buffer,
               state->software_queue + read_frame * USB_AUDIO_FRAME_BYTES,
               first * USB_AUDIO_FRAME_BYTES);
        if (first < real_frames)
          memcpy(packet_buffer + first * USB_AUDIO_FRAME_BYTES,
                 state->software_queue,
                 (real_frames - first) *
                     USB_AUDIO_FRAME_BYTES);
        if (real_frames < USB_AUDIO_FRAMES_PER_PACKET)
          memset(packet_buffer + real_frames * USB_AUDIO_FRAME_BYTES, 0,
                 (USB_AUDIO_FRAMES_PER_PACKET - real_frames) *
                     USB_AUDIO_FRAME_BYTES);
      } else {
        memset(packet_buffer, 0, 192);
        state->underruns++;
      }
      struct usb_isoch_packet *packet = &state->packets[slot];
      packet->buffer = packet_buffer;
      packet->buffer_phys = state->dma_phys + slot * 192;
      packet->length = state->desc.max_packet_size;
      if (usb_isoch_submit(state->stream, packet) < 0)
        break;
      state->packet_active[slot] = true;
      state->packet_real_frames[slot] = real_frames;
      state->inflight++;
      state->submitted_packets++;
      if (real_frames) {
        state->queue_read_frame =
            (read_frame + real_frames) % USB_AUDIO_SOFT_FRAMES;
        state->queued_frames -= real_frames;
        if (tail_audio)
          state->drain_tail = false;
      }
    }
    spinlock_release(&state->lock);
  }
}

static void usb_audio_worker(void) {
  struct thread *self = sched_get_current();
  for (;;) {
    usb_audio_worker_reconcile();
    usb_audio_poll();
    bool busy = false;
    for (uint32_t i = 0; i < USB_AUDIO_MAX_DEVICES; i++) {
      struct usb_audio_state *state = &usb_audio_states[i];
      spinlock_acquire(&state->lock);
      busy |= state->running || state->attach_pending ||
              state->detach_pending;
      spinlock_release(&state->lock);
    }
    /* Keep packet service close to 1 ms while a stream runs. With no stream
     * or hotplug work, sleep for one second; discovery/removal/start wake the
     * worker directly through sched_wakeup(). */
    self->wakeup_ticks = lapic_timer_get_ticks() + (busy ? 1 : 1000);
    self->state = THREAD_SLEEPING;
    sched_yield();
    self->wakeup_ticks = 0;
  }
}

void usb_audio_start_worker(void) {
  if (usb_audio_worker_started)
    return;
  spinlock_acquire(&usb_audio_states_lock);
  if (!usb_audio_states_initialized) {
    for (uint32_t i = 0; i < USB_AUDIO_MAX_DEVICES; i++) {
      spinlock_init(&usb_audio_states[i].lock);
      wait_queue_init(&usb_audio_states[i].wait_queue);
      usb_audio_states[i].card_index = -1;
    }
    usb_audio_states_initialized = true;
  }
  spinlock_release(&usb_audio_states_lock);
  struct thread *worker =
      sched_create_kernel_thread(usb_audio_worker, cpu_get_current(), false);
  if (!worker) {
    klog_puts("[USB-AUDIO] failed to start process-context worker\n");
    return;
  }
  strcpy(worker->comm, "usb-audio");
  sched_set_priority(worker, 0, -5);
  usb_audio_worker_thread = worker;
  usb_audio_worker_started = true;
  sched_enqueue_thread(worker, cpu_get_current());
  klog_puts("[USB-AUDIO] process-context worker started\n");
}

void usb_audio_diag(struct usb_diag *d) {
  if (!d)
    return;
  usb_diag_printf(d, "USB audio playback devices:\n");
  for (uint32_t i = 0; i < USB_AUDIO_MAX_DEVICES; i++) {
    struct usb_audio_state *state = &usb_audio_states[i];
    spinlock_acquire(&state->lock);
    if (!state->dev && !state->registered) {
      spinlock_release(&state->lock);
      continue;
    }
    usb_diag_printf(d,
                    "  card=%d vid=%04X pid=%04X connected=%u registered=%u "
                    "open=%u running=%u queue=%u/%u inflight=%u "
                    "packets=%llu/%llu errors=%llu underruns=%llu\n",
                    state->card_index,
                    state->dev ? state->dev->desc.vendor_id : 0,
                    state->dev ? state->dev->desc.product_id : 0,
                    state->dev && state->dev->connected ? 1 : 0,
                    state->registered ? 1 : 0, state->opened ? 1 : 0,
                    state->running ? 1 : 0, state->queued_frames,
                    USB_AUDIO_SOFT_FRAMES, state->inflight,
                    (unsigned long long)state->completed_packets,
                    (unsigned long long)state->submitted_packets,
                    (unsigned long long)state->transfer_errors,
                    (unsigned long long)state->underruns);
    spinlock_release(&state->lock);
  }
}
