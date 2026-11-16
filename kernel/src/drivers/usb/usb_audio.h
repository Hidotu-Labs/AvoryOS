#ifndef DRIVERS_USB_USB_AUDIO_H
#define DRIVERS_USB_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

struct usb_device;
struct usb_diag;

/* A UAC1 playback alternate setting that matches the first USB audio
 * milestone.  The descriptor values are retained so the stream driver can
 * configure exactly the alternate that was inspected. */
struct usb_audio_playback_desc {
  uint8_t configuration_value;
  uint8_t interface_number;
  uint8_t alternate_setting;
  uint8_t terminal_link;
  uint8_t endpoint_address;
  uint8_t endpoint_attributes;
  uint8_t interval;
  uint16_t max_packet_size;
};

/* Parse one complete configuration descriptor. Returns true only for a
 * well-formed UAC1 configuration containing the supported full-speed PCM
 * playback alternate (48 kHz, stereo, S16_LE, synchronous/adaptive OUT). */
bool usb_audio_parse_configuration(const uint8_t *bytes, uint16_t length,
                                   struct usb_audio_playback_desc *selected);

/* Fetch and inspect configuration zero. Discovery is passive: it does not
 * select the configuration or change an alternate setting. */
bool usb_audio_discover(struct usb_device *dev,
                        struct usb_audio_playback_desc *selected);

void usb_audio_selftest(void);

/* Opt-in boot-context xHCI transport exercise.  usb.c walks enumerated
 * devices after host-controller initialization and calls this hook; it only
 * acts on the pinned QEMU fixture when usb_audio_probe=1 is on the command
 * line. */
void usb_audio_probe_smoke(struct usb_device *dev);
void usb_audio_boot_smoke(void);

/* IRQ-side discovery/removal only records state. The worker performs ALSA
 * node registration and synchronous endpoint teardown in process context. */
void usb_audio_device_discovered(
    struct usb_device *dev,
    const struct usb_audio_playback_desc *selected);
void usb_audio_device_removed(struct usb_device *dev);
bool usb_audio_device_reusable(struct usb_device *dev);
void usb_audio_start_worker(void);
void usb_audio_poll(void);
void usb_audio_diag(struct usb_diag *d);

#endif
