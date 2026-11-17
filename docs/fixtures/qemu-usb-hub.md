# QEMU USB hub topology fixture

This fixture pins the topology and descriptors of QEMU 11.1.2's `usb-hub` as
attached to `qemu-xhci`. It is launched by `make run-usb-hub`.

## Launch

```
timeout 90 make -o avoryos-x86_64.iso -o edk2-ovmf run-usb-hub
```

Topology built by the target:

```
xhci.0
└── usb-hub  port=1                       (root port, hub A, 8 ports)
    ├── usb-kbd      port=1.1
    ├── usb-mouse    port=1.2
    ├── usb-storage  port=1.3  (drive=stick, build/usb-stick.img)
    └── usb-hub      port=1.4              (hub B, nested, 8 ports)
        └── usb-kbd  port=1.4.1
```

Disk and stick are attached with `snapshot=on,file.locking=off`, as in
`qemu-usb-storage-bot.md`. `disk.img` and `build/usb-stick.img` are not written.

QEMU's `usb-hub` is a full-speed USB 1.1 hub. This fixture exercises topology,
route strings and the hub state machine. It does **not** exercise Transaction
Translators, high-speed hubs or SuperSpeed hubs. Those need physical hubs (Phase 8).

## QEMU version

```
$ qemu-system-x86_64 --version
QEMU emulator version 11.1.2
```

## Observed kernel log

```
[XHCI] Found controller: version=0x0x00000000 slots=64 ports=8 scratchpads=0
[XHCI] protocol caps: rev=2 ports 5..8 (USB2)
USB: New device detected on port 5 (Full-Speed)
USB: Enumerating device...
USB: Max Packet Size: 8
USB: Device Vendor: 0x00000409 Product: 0x000055AA
[WARN] USB: Hub detected; downstream devices are NOT enumerated
```

- The hub enumerated on root port 5, which the kernel reports as a USB2 root
  port at Full-Speed. This matches the USB2 protocol range (`ports 5..8`).
- The kernel logged the hub warning and did not enumerate any downstream device.
  The keyboard, mouse, storage stick, nested hub and nested keyboard therefore
  did not appear in the log. This is the expected state before the Phase 3 hub
  driver exists.
- The boot continued normally to the userland init, so the hub warning is not fatal.

Note: the same QEMU `port=1` value placed the storage fixture on root port 1
(SuperSpeed), and the hub on root port 5. QEMU's mapping of `port=` names to root
ports for USB2 and USB3 is not otherwise documented here. Treat root port 5 as
observed for this hub, not as a general rule.

## Hub A descriptors (root-attached hub, observed as Full-Speed)

Source: `hw/usb/dev-hub.c` in QEMU 11.1.2 (`desc_device_hub`, `desc_iface_hub`,
`desc_hub`, `qemu_hub_hub_descriptor`). The byte values are hand-serialized from
those literals with the layout from `hw/usb/desc.c`. The kernel does not dump
descriptors, so these bytes are not read back from the device.

Device (18 bytes):

| Field | Value |
| --- | --- |
| bcdUSB | 0x0110 (USB 1.1) |
| bDeviceClass / SubClass / Protocol | 09 / 00 / 00 (hub) |
| bMaxPacketSize0 | 8 |
| idVendor / idProduct | 0x0409 / 0x55AA |
| bcdDevice | 0x0101 |
| iManufacturer / iProduct / iSerialNumber | 1 "QEMU" / 2 "QEMU USB Hub" / 3 "314159" |
| bNumConfigurations | 1 |

Raw: `12 01 10 01 09 00 00 08 09 04 AA 55 01 01 01 02 03 01`

Configuration tree (25 bytes total, matches the source layout):

```
Configuration  09 02 19 00 01 01 00 E0 00
  bmAttributes 0xE0 (self-powered, remote wakeup), bMaxPower 0, no iConfiguration
  Interface    09 04 00 00 01 09 00 00 00      class 0x09 (hub), 1 endpoint
    Endpoint   07 05 81 03 02 00 FF           EP1 IN, interrupt, wMaxPacketSize 2, bInterval 0xFF
```

Hub class descriptor (`0x29`, 10 bytes for 8 ports, as returned by GET_DESCRIPTOR):

```
29 hub descriptor:  0A 29 08 09 00 01 00 00 00 FF
  bDescLength 10, bDescriptorType 0x29, bNbrPorts 8,
  wHubCharacteristics 0x0009 (individual power switching, per-port over-current),
  bPwrOn2PwrGood 2 ms (0x01), bHubContrCurrent 0 mA,
  DeviceRemovable 00 00 (2 bytes, 9 bits for 8 ports), PortPwrCtrlMask FF
```

The template in source has wHubCharacteristics LSB `0x0a`. The `GetHubDescriptor`
handler patches that byte to `0x09` when the hub's port power is on. If port power
is off, the byte stays `0x0a` and the descriptor becomes `0A 29 08 0A 00 01 00 00 00 FF`.
Which of the two QEMU returns in this run was not confirmed.

Nested hub B (on port 1.4) uses the same device and descriptor literals. Its
descriptors were not observed, because the kernel stopped at the hub warning.

## Limitations

- Full-speed USB 1.1 only. No Transaction Translator, no high-speed or
  SuperSpeed hub coverage. Those are Phase 8 physical-hardware tests.
- Downstream devices were not enumerated by the current kernel, so the topology,
  route strings and the nested hub were not observed in this run. Only the root
  hub detection and its descriptors are verified in the log.
- Descriptor bytes are source-derived, not read from the device.
