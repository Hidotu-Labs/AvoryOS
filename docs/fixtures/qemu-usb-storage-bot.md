# QEMU USB mass storage (BOT) fixture

This fixture pins the descriptors QEMU 11.1.2 exposes for `usb-storage` when it
is attached to the `qemu-xhci` controller used by AvoryOS. It is launched by
`make run-usb-storage` in the top-level `GNUmakefile`.

## Launch

```
timeout 90 make -o avoryos-x86_64.iso -o edk2-ovmf run-usb-storage
```

- `-o` marks the ISO and OVMF as up to date, so the kernel and ISO are not
  rebuilt. The fixture boots the existing `avoryos-x86_64.iso`.
- The root disk is `disk.img` attached with `snapshot=on,file.locking=off`.
  `disk.img` is never written, and a running `make run` VM is not affected.
- The stick is `build/usb-stick.img`, a 64 MiB FAT32 image made by
  `mkfs.fat -F 32 -n AVORYUSB`. It is attached with `snapshot=on,file.locking=off`
  and is also never written.
- `USB_STORAGE_PORT` selects the xHCI root port (default `1`). Empty means
  auto-assign.

Effective device line (`USB_STORAGE_PORT=1`):

```
-device qemu-xhci,id=xhci \
-drive if=none,id=stick,file=build/usb-stick.img,format=raw,snapshot=on,file.locking=off \
-device usb-storage,bus=xhci.0,port=1,drive=stick
```

Serial was captured with `-serial stdio` and the run was stopped by `timeout`.

## QEMU version

```
$ qemu-system-x86_64 --version
QEMU emulator version 11.1.2
```

## Port mapping observed on this QEMU

- The kernel reports the controller as `ports=8`, with
  `protocol caps: rev=2 ports 5..8 (USB2)`. Root ports 1..4 are therefore the
  SuperSpeed ports.
- `usb-storage` on `port=1`, `port=2` and `port=3` was enumerated by the kernel as
  `SuperSpeed` on those ports, with `Max Packet Size: 9`.
- `port=5` through `port=8` are rejected by QEMU with
  `usb port 5 (bus xhci.0) not found (in use?)`. Auto-assign (`USB_STORAGE_PORT=`)
  also picked root port 1.
- **Not verified:** a `usb-storage` on a USB2 root port (5..8). QEMU would not
  place it there in this setup. The USB2 hub path is covered by the hub fixture
  (`qemu-usb-hub.md`).

## Observed kernel log (`USB_STORAGE_PORT=1`)

```
[XHCI] Found controller: version=0x0x00000000 slots=64 ports=8 scratchpads=0
[XHCI] protocol caps: rev=2 ports 5..8 (USB2)
USB: New device detected on port 1 (SuperSpeed)
USB: Enumerating device...
USB: Max Packet Size: 9
USB: Device Vendor: 0x000046F4 Product: 0x00000001
[USB-MOUSE] probe begin addr=1 port=1 speed=SuperSpeed hcd=xhci vid=0x46F4 pid=0x0001 class=0x00
[USB-MOUSE] config total_len=44 num_interfaces=1 config_value=1 attributes=0xC0 max_power=0
[USB-MOUSE] Interface: class=0x0x00000008 subclass=0x0x00000006 protocol=0x0x00000050
[USB] no driver claimed vid=0x46F4 pid=0x0001 class=0x00 (kbd=0 mouse=0)
```

The kernel has no mass-storage driver yet. The `no driver claimed` line is the
expected result for this phase. The `[USB-MOUSE]` tag is the HID mouse probe,
which was the only driver to parse the storage device's configuration; it
rejected the device as non-HID, as expected.

The same run also enumerated the QEMU keyboard (`0627:0001` on port 6, High-Speed)
and tablet (`0627:0001` on port 7, High-Speed). They are not part of the storage
fixture.

## Device descriptor (SuperSpeed, observed on the USB3 root port)

Source: `hw/usb/dev-storage.c`, `desc_device_super` and `desc`. Byte values are
hand-serialized from those literals with the layout from `hw/usb/desc.c`. The
kernel does not dump descriptors, so these bytes are not read back from the device.

| Field | Value |
| --- | --- |
| bcdUSB | 0x0300 |
| bDeviceClass / SubClass / Protocol | 00 / 00 / 00 |
| bMaxPacketSize0 | 9 (2^9 = 512 for SS) |
| idVendor / idProduct | 0x46F4 / 0x0001 |
| bcdDevice | 0x0000 |
| iManufacturer / iProduct / iSerialNumber | 1 "QEMU" / 2 "QEMU USB HARDDRIVE" / 3 "1" |
| bNumConfigurations | 1 |

Raw (18 bytes): `12 01 00 03 00 00 00 09 F4 46 01 00 00 00 01 02 03 01`

## Configuration descriptor tree (SuperSpeed)

`total_len=44` matches the kernel log.

```
Configuration   09 02 2C 00 01 01 06 C0 00
  iConfiguration 6 = "Super speed config (usb 3.0)"; bmAttributes 0xC0 (self-powered); bMaxPower 0
  Interface     09 04 00 00 02 08 06 50 00
    class 0x08 (mass storage), subclass 0x06 (SCSI transparent), protocol 0x50 (bulk-only), 2 endpoints
    Endpoint IN  07 05 81 02 00 04 00      EP1 IN, bulk, wMaxPacketSize 1024, bInterval 0
      SS Endpoint Companion  06 30 0F 00 00 00   bMaxBurst 15, bmAttributes 0, wBytesPerInterval 0
    Endpoint OUT 07 05 02 02 00 04 00      EP2 OUT, bulk, wMaxPacketSize 1024, bInterval 0
      SS Endpoint Companion  06 30 0F 00 00 00   bMaxBurst 15, bmAttributes 0, wBytesPerInterval 0
```

The SS Endpoint Companion (`0x30`) descriptors are the ones the Rust config parser
must keep (Phase 1 contract).

## Other speeds (source only, not observed)

`dev-storage.c` also defines full-speed and high-speed sets. Neither was enumerated
in these runs, because only the USB3 root ports were used. The high-speed set is
`bcdUSB 0x0200`, `bMaxPacketSize0 64`, `iConfiguration 5`, bulk `wMaxPacketSize 512`,
with no companion descriptor. The full-speed set uses `iConfiguration 4` and the
same device IDs.

## Limitations

- Only the SuperSpeed descriptor set was observed. Full-speed and high-speed
  bytes are source-derived and unverified.
- The stick is blank, so the mass-storage BOT protocol has not been exercised.
  These fixtures only prove enumeration and descriptor shape.
