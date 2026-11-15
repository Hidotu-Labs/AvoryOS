# QEMU USB Audio UAC1 acceptance fixture

This fixture pins QEMU 11.1.2's default `usb-audio` descriptor set (the `multi`
property is off). It is directly attached to `qemu-xhci` as a full-speed USB
device. QEMU's monitor reports `Device 0.0, Port 1, Speed 12 Mb/s, Product QEMU
USB Audio Interface`; this confirms the fixture's actual attachment speed and
topology. The descriptor bytes below were captured from the configured QEMU
device and cross-checked against `hw/usb/dev-audio.c` in the QEMU 11.1.2 source
and its USB descriptor serializer (`hw/usb/desc.c`). The source literals are
also retained as a convenient way to review the descriptor contract without a
QEMU build tree.

## Device descriptor

```text
12 01 00 01 00 00 00 40 f4 46 02 00 00 00 01 02 00 01
```

USB 1.00; EP0 max packet 64; VID:PID `46f4:0002`; one configuration.

## Configuration descriptor tree

The following is the complete 113-byte configuration descriptor, including
both AudioControl and AudioStreaming interfaces and all class-specific
descriptors. Descriptors are in bus order; multi-byte fields are little-endian.

```text
09 02 71 00 02 01 04 c0 32
09 04 00 00 00 01 01 00 05
09 24 01 00 01 2b 00 01 01
0c 24 02 01 01 01 00 02 03 00 00 06
0d 24 06 02 01 02 01 00 02 00 02 00 07
09 24 03 03 01 03 00 02 08
09 04 01 00 00 01 02 00 09
09 04 01 01 01 01 02 00 0a
07 24 01 01 00 01 00
0b 24 02 01 02 02 10 01 80 bb 00
09 05 01 0d c0 00 01 00 00
07 25 01 00 00 00 00
```

| Interface/alternate | Meaning |
| --- | --- |
| 0 / 0 | AudioControl; UAC1 header includes streaming interface 1. |
| 1 / 0 | AudioStreaming disabled alternate; no endpoint. |
| 1 / 1 | AudioStreaming Type I, stereo, two-byte subframes, 16-bit samples, 48 kHz; one isochronous OUT endpoint. |

The AudioControl graph is Input Terminal ID 1 (`0x0101`, 2 channels, channel
configuration `0x0003`) → Feature Unit ID 2 → Output Terminal ID 3 (`0x0301`,
speaker). The AS general descriptor links to terminal 1 and has PCM format tag
1. The endpoint is OUT 1, `bmAttributes=0x0d`: isochronous, synchronous
synchronization, data endpoint; `wMaxPacketSize=192`; `bInterval=1`; no
feedback endpoint. The seven-byte class-specific endpoint descriptor has no
sampling-frequency or pitch controls. At full speed, 192 bytes per 1 ms frame
is exactly 48 stereo frames × 2 channels × 2 bytes.

This satisfies the first milestone: full-speed directly attached UAC1 playback,
S16_LE stereo at 48 kHz, synchronous isochronous OUT. The parser also accepts
adaptive synchronization when the remaining supported format and endpoint
requirements match. `usb-audio,multi=on` changes the format and is not this
acceptance fixture. Asynchronous synchronization, feedback, capture and UAC2 are
unsupported.

## Parser and rejection contract

Parse by descriptor `bLength` and `bDescriptorType`, retaining interface
alternate settings rather than assuming one descriptor per interface. Associate
class-specific interface descriptors with their immediately preceding
interface, and class-specific endpoint descriptors with their endpoint. Check
the AudioControl collection and terminal links before accepting an AS alternate.
Accept this topology only when its UAC1 PCM Type I format is stereo, 16-bit,
48 kHz and its single OUT endpoint is full-speed isochronous with synchronous or
adaptive synchronization and enough packet capacity. This QEMU fixture uses
synchronous synchronization. Reject malformed lengths, broken links,
extra/async feedback endpoints, unsupported rates/formats, or inconsistent
endpoint counts. A rejection must leave unrelated USB interface drivers and
devices untouched.

The `run-usb-audio` make target attaches the matching QEMU device to `qemu-xhci`
and boots the normal AvoryOS ISO under TCG. It uses `disk.img` through a
temporary qcow2 overlay; override `USB_AUDIO_DISK=/path/to/valid-disk.img` when
the default image is absent or is only a partition shell. The null audio
backend is quiet by default; set `USB_AUDIO_BACKEND=pa` or `pipewire` to route
playback to a host audio server. The boot-only `usb_audio_probe=1` exercise
sends silence packets; audible playback uses the ALSA/PipeWire PCM path, which
has been verified with the QEMU fixture. For example:

```sh
make run-usb-audio USB_AUDIO_DISK=/path/to/valid-disk.img
make run-usb-audio USB_AUDIO_DISK=/path/to/valid-disk.img USB_AUDIO_BACKEND=pa
```
