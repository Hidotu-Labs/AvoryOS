# USB mass storage and hub implementation plan

## First milestone

Two drivers, both written in **Rust**, on xHCI:

- **USB hubs:** USB 2.0 and SuperSpeed hubs, chained up to the spec limit of five tiers, with full-/low-speed devices behind high-speed hubs (Transaction Translator) and hot-plug on downstream ports. HID, audio, and storage devices behind a hub work the same as on a root port.
- **USB mass storage:** **Bulk-Only Transport (BOT)** with the **SCSI transparent command set** (interface `0x08/0x06/0x50`). Cover LUN 0 only, 512- or 4096-byte logical sectors, read/write/flush, and one outstanding command per device. The disk registers as `sdX` in the block layer, partitions are scanned, FAT32 mounts, and the disk can be removed safely, including surprise removal.

**Language split.** Rust owns the class-driver logic: descriptor parsing, protocol state machines, error recovery, and the per-device state. C keeps the USB core (`usb.c`), the host controller (`xhci.c`), the block layer, the VFS, and kernel primitives. Rust reaches C only through a small, explicit shim (Phase 0). It never mirrors the layout of `struct usb_device` or `struct block_device`.

UAS, non-xHCI controllers, and multi-LUN card readers are out of scope (see Deferred).

## Starting point (what exists, what is missing)

| Area | State | Gap |
|---|---|---|
| Rust crate (`kernel/rust`) | `no_std` staticlib, `USE_RUST ?= 1`, C-to-Rust parser calls with C fallbacks (`USE_RUST_STORAGE`, `USE_RUST_NET`) | No Rust-to-C calls, no kernel bindings, no allocator, no threads or locks; the panic handler is a silent `cli; hlt` |
| `usb.h` HCD ops | control, interrupt, isoch | **No bulk transfer API** |
| USB topology (`usb.c`) | A device is identified by `(hcd, root port)` | **No parent/child tree**; hubs (class `0x09`) are detected and logged as unsupported |
| `xhci.c` slot context | Root hub port and speed taken from `PORTSC` | No route string, TT hub/port, Hub flag, port count, or `SET_HUB_DEPTH` |
| `xhci.c` endpoints | Interrupt (`ep type 7`) and isoch; EP0 halt recovery | No bulk endpoint type, no TRB chaining across 64 KiB, no non-EP0 halt recovery |
| `block.c` | `block_register`, partitions, 64 slots | **No `block_unregister`**, no handling for a device that disappears while mounted |
| `sysfs.c` | `removable` hard-coded to `"0"` | Per-device removable/ro flags; USB parent path for udev |
| Filesystems | ext2/3/4, FAT32, FUSE | No exFAT or NTFS |

## Ordered phases

0. **Rust driver infrastructure**
   - **Deliverable:**
     - **C shim** in `drivers/usb/usb_rust_glue.c` with matching `usb_rust_glue.h`, built only with `USE_RUST_USB=1`. It uses opaque handles and plain-integer/buffer functions only:
       - USB: control transfer, interrupt open/wait/resubmit/cancel, bulk open/transfer/clear-halt/close (Phase 4), plus raw device and configuration descriptor bytes
       - kernel: `kmalloc`/`kfree` for DMA-safe buffers, mutex lock/unlock, kernel-thread spawn, `msleep`, monotonic ms clock, `klog`
       - block: register/unregister through a small ops struct whose entries are Rust `extern "C"` trampolines
       - diagnostics: append to a `usb_diag` buffer
     - **Rust side** in `kernel/rust/src/usb/`, laid out as `ffi.rs`, `kernel.rs`, `hub/`, and `storage/`:
       - `ffi.rs`: `extern "C"` declarations for the shim
       - `kernel.rs`: safe wrappers (an RAII `MutexGuard`, a `DmaBuf` that owns memory from `kmalloc`, a `klog!` macro, and `core::fmt::Write` for diag)
       - Exports: `rust_usb_hub_probe/removed`, `rust_usb_storage_probe/removed`, `rust_usb_diag`
     - **No `alloc` crate.** Per-device state lives in fixed static slot tables (for example 16 hubs and 8 storage devices) guarded by shim mutexes. Allocation failure stays an error value, never a panic.
     - **Panic policy:**
       - Add `#![deny(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing, clippy::panic)]` to the `usb` module and run `cargo clippy` in `make`.
       - Change the panic handler to log the panic location through the shim before halting, so a Rust bug can be diagnosed instead of showing as a frozen screen.
     - **Testability:** Protocol code depends on a `trait UsbTransport` (control/bulk/interrupt/clear-halt) and a `trait Clock`, not on `ffi` directly. Host `cargo test` runs it against scripted mock devices. Add `make rust-test`.
     - **Build:** Pass `-DUSE_RUST_USB=1` to `usb.c` and the glue. With `USE_RUST=0` the drivers are absent and `usb.c` keeps today's "hub detected, unsupported" path. There are no C fallback drivers.
     - **Layout safety:** Every `#[repr(C)]` type crossing the boundary has a C `_Static_assert` and a Rust `const` assert on size and offsets.
   - **Exit:** A trivial Rust probe sees every enumerated device and logs VID:PID through the shim. `make rust-test` passes on the host. The kernel builds and boots with `USE_RUST=0` and with `USE_RUST=1`.

1. **Fixtures and descriptor contract**
   - **Deliverable:**
     - Add `make run-usb-storage` and `make run-usb-hub`. Use a separate image with `snapshot=on,file.locking=off` so `make run`'s `disk.img` is never touched.
       - Storage directly attached: `-device usb-storage` on `qemu-xhci`, both a USB2 and a USB3 port.
       - Behind a hub: `-device usb-hub,bus=xhci.0,port=1` with `usb-kbd`, `usb-mouse`, and `usb-storage` on `port=1.x`, and a second hub nested on `1.4` to test tiers.
     - Capture descriptors in `docs/fixtures/qemu-usb-storage-bot.md` and `docs/fixtures/qemu-usb-hub.md`. The Rust config parser keeps the SS Endpoint Companion descriptor (`0x30`) and parses hub descriptors `0x29` (USB2) and `0x2A` (SS).
   - **QEMU limitation:** QEMU's `usb-hub` is a full-speed USB 1.1 hub. It exercises topology, route strings, and the hub state machine, but **not** Transaction Translators, high-speed hubs, or SuperSpeed hubs. Those need physical hubs (Phase 8).
   - **Exit:** Each fixture enumerates as expected, and the descriptors parse in host tests using the captured bytes.

2. **USB core topology (C)**
   - **Deliverable:**
     - Add to `struct usb_device`: `parent`, `parent_port` (1-based hub port), `root_port`, `depth` (0 = root), `route` (20-bit, a 4-bit nibble per tier), `tt_hub` and `tt_port`, plus hub fields `is_hub`, `num_ports`, `multi_tt`.
     - Identify devices by `(hcd, parent, port)` instead of `(hcd, port)`.
     - Add `usb_hub_child_discovered(hub, port, speed)` and `usb_hub_child_removed(hub, port)`, which the Rust hub driver calls through the shim. The root-port entry points become the `parent == NULL` case.
     - Removal is recursive: unplugging a hub disconnects every descendant, deepest first, before the hub itself, so kbd, mouse, audio, and storage get their removal callbacks in order.
     - Enforce the 5-tier limit.
     - Replace the `claimed_by` bit for hubs with a hand-off to `rust_usb_hub_probe`, and give storage its own bit (`0x08`).
     - `/proc/usb` prints the tree, for example `1-2.4.1`.
   - **Exit:** Root-port behaviour is unchanged (kbd, mouse, audio). Unit-style self-checks for route-string construction and recursive removal pass.

3. **xHCI hub support (C)**
   - **Deliverable:**
     - **Address Device for a device behind a hub:**
       - Slot Context Route String = `dev->route`
       - Root Hub Port Number = the root ancestor's port
       - Speed taken from the hub's port status, not `PORTSC`
       - for a LS/FS device behind a HS hub: TT Hub Slot ID and TT Port Number from the nearest HS ancestor hub
       - EP0 max packet set from the speed
     - **When a device is a hub:** a Configure Endpoint or Evaluate Context sets Slot Context `Hub=1`, Number of Ports, `MTT`, and TT Think Time. For SS hubs, the Rust driver sends `SET_HUB_DEPTH` before its interrupt endpoint is used.
     - **On child removal:** disable the child's slot and free its rings without touching the root port. Root-port `PORTSC` handling only touches devices with `depth == 0`.
     - Port reset of downstream ports is done by the hub (class requests). The xHCI layer only addresses the device after the hub driver reports the port enabled.
   - **Exit:** Devices behind the QEMU hub (including the nested one) are addressed and HID input works through them. Unplugging the hub through QMP frees every child slot. Slot counts in `/proc/usb_xhci` return to their baseline.

4. **Bulk transport (C)**
   - **Deliverable:** Add a bulk API to `struct usb_hcd` and `usb.h`, wrapped by the shim:
     ```c
     struct usb_bulk_pipe *usb_bulk_open(struct usb_device *dev, uint8_t endpoint,
                                         uint16_t max_packet, uint8_t max_burst);
     int  usb_bulk_transfer(struct usb_bulk_pipe *pipe, void *buf, uint64_t phys,
                            uint32_t len, uint32_t *actual, uint32_t timeout_ms);
     int  usb_clear_halt(struct usb_bulk_pipe *pipe);   // Reset EP + Set TR Deq + CLEAR_FEATURE(ENDPOINT_HALT)
     void usb_bulk_close(struct usb_bulk_pipe *pipe);
     ```
     In xHCI:
     - Configure both endpoints in one Configure Endpoint command: EP type 2 for OUT and 6 for IN, CErr=3, MaxBurst from the companion descriptor.
     - Build Normal TRB chains that never cross a 64 KiB boundary. Set ISP on IN transfers and IOC on the last TRB, and fill TD Size.
     - Compute the residual from the Transfer Event length, treating Short Packet as success.
     - Return a distinct STALL error. Generalise `xhci_recover_ep0` into a per-endpoint helper.
     - Stop the endpoint and cancel outstanding TDs on timeout, close, and removal (including removal through a parent hub).
     - The calling thread sleeps until completion instead of spinning, because storage waits can last seconds.
   - **DMA:** A per-device physically contiguous bounce buffer of 128 KiB. It is owned by C and handed to Rust as a `DmaBuf`.
   - **Exit:** The `usb_storage_probe=1` self-test passes:
     - transfers larger than 64 KiB that wrap the ring
     - short IN reads
     - STALL, then clear-halt, then a successful retry
     - timeout followed by cancel
     - unplug with a transfer outstanding, on a root port and behind a hub

     Interrupt pipes and isoch streams keep working at the same time.

5. **Hub class driver (Rust, `rust/src/usb/hub/`)**
   - **Deliverable:**
     - **Probe:** On device class `0x09`, read the hub descriptor and record the port count, power-on delay, multi-TT, and whether ports are removable. Send `SET_CONFIGURATION`; for multi-TT hubs select alternate setting 1. Report the hub parameters to C for Phase 3, then power all ports and wait `bPwrOn2PwrGood × 2 ms`.
     - **Event loop:** One kernel worker per hub waits on the status-change interrupt endpoint (bitmap: bit 0 = hub, bit *n* = port *n*). For each changed port it runs `GET_PORT_STATUS`, clears `C_PORT_CONNECTION`, `C_PORT_ENABLE`, `C_PORT_RESET`, `C_PORT_OVER_CURRENT`, `C_PORT_SUSPEND`, and for SS hubs `C_BH_PORT_RESET`, `C_PORT_LINK_STATE`, and `C_PORT_CONFIG_ERROR`.
     - **Port state machine:**
       1. connect: debounce for 100 ms of stable connection
       2. `SET_FEATURE(PORT_RESET)`
       3. wait for `C_PORT_RESET` with a timeout
       4. read speed from the LS/HS bits (implied SuperSpeed on SS hubs)
       5. wait the 10 ms reset recovery
       6. `usb_hub_child_discovered`

       A disconnect calls `usb_hub_child_removed`. Over-current marks the port faulted and re-powers it after a backoff. Each reset step is retried a bounded number of times.
     - **Ordering:** Reset one port at a time per hub. A newly attached child hub starts its own worker, which gives tier-by-tier discovery naturally.
     - **Removal:** Stop the worker, cancel the interrupt pipe, and let C tear down the subtree.
     - Diagnostics per hub: ports, per-port status and change bits, resets, debounce failures, over-current events.
   - **Host tests:** Run against a scripted mock hub: power-on sequencing, debounce rejection of a bouncing connect, reset timeout and retry, connect and disconnect storms, over-current, and SS-specific change bits.
   - **Exit:** On the QEMU hub fixture, kbd, mouse, and storage behind first- and second-tier hubs enumerate at boot and on hot-plug. Unplugging the hub removes all of them cleanly. Physical USB 2 and USB 3 hubs pass in Phase 8.

6. **BOT + SCSI class driver (Rust, `rust/src/usb/storage/`)**
   - **Deliverable:** Split into `bot.rs`, `scsi.rs`, and `device.rs`:
     - **Probe/claim:** Match interface `08/06/50` and take the storage claim bit. C hands Rust the parsed bulk endpoints and their MaxBurst.
     - **Class requests:** GET MAX LUN (`0xFE`, where a STALL means LUN 0) and Bulk-Only Mass Storage Reset (`0xFF`).
     - **BOT engine (`bot.rs`):**
       - CBW and CSW are `#[repr(C, packed)]` types with explicit little-endian encode/decode.
       - Check the CSW signature, tag, and residue.
       - Reset recovery: BOMSR, then clear-halt on IN, then clear-halt on OUT. If the data phase STALLs, clear the halt and still read the CSW. Status `0x02` (phase error) triggers reset recovery.
       - Represent the result as an `enum` covering Passed, Failed (sense needed), PhaseError, Stall, Timeout, and Gone, with the caller matching every case.
     - **SCSI (`scsi.rs`):**
       - INQUIRY
       - TEST UNIT READY with backoff during spin-up
       - REQUEST SENSE after every Failed, decoded into a typed sense key/ASC/ASCQ
       - READ CAPACITY(10), falling back to (16) when it returns `0xFFFFFFFF`
       - READ/WRITE(10), switching to (16) above 2³² sectors
       - SYNCHRONIZE CACHE(10) for flush (ILLEGAL REQUEST counts as success)
       - MODE SENSE(6) for the write-protect bit
       - START STOP UNIT and PREVENT/ALLOW MEDIUM REMOVAL for eject
     - **Device (`device.rs`):**
       - Cap each command at 240 sectors.
       - A shim mutex keeps one CBW in flight.
       - A worker started at probe does INQUIRY, TUR, and capacity before registering the block device, so spin-up never blocks enumeration.
       - A `const` VID:PID quirk table (empty at first) for flags such as `NO_SYNC_CACHE`, `MAX_SECTORS_64`, and `IGNORE_RESIDUE`.
       - After removal, every call returns Gone.
   - **Host tests:** Run against a scripted mock BOT device: CBW encoding, CSW validation (bad signature, tag mismatch, residue), STALL in the data phase, phase-error recovery sequence, sense decoding, the capacity (16) fallback, the 16-byte CDB switch, and splitting at the 240-sector cap.
   - **Exit:** On the QEMU fixture, both directly attached and behind a hub, capacity and sector size are correct. A 256 MiB pattern written and read back matches its checksum on the host. A forced phase error recovers without a replug.

7. **Block layer, removal, and desktop integration (C)**
   - **Deliverable:**
     - **Registration:** Rust registers `sda`, `sdb`, … (major 8) through the shim, with read, write (unless write-protected), and flush trampolines. Then call `block_scan_partitions`.
     - **`block_unregister()`** must:
       - remove partition children first
       - remove `/dev`, sysfs, `/proc/partitions` and diskstats entries
       - send a `remove` uevent
       - free the slot
     - **`gone` flag or refcount:** After removal, I/O fails fast with `-EIO` and the struct lives until the last mount or opener drops it.
     - **sysfs:** Make `removable` and `ro` per-device, and link `device` under the USB path (including the hub chain) so udev sets `ID_BUS=usb`.
     - **udev:** Send add/remove uevents during coldplug and hotplug.
     - **Desktop:** UDisks2 and the Plasma notifier offer mount, unmount, and eject. Eject sends SYNCHRONIZE CACHE then START STOP UNIT. Unmount flushes the page cache.
     - **exFAT/NTFS:** Decide between `exfat-fuse`/`ntfs-3g` over FUSE or a native driver later.
   - **Exit:**
     - `lsblk` shows RM=1.
     - Plug, notifier, mount, copy, eject, and unplug all work from Plasma.
     - Unplugging while writing, or unplugging the hub the disk sits behind, gives I/O errors and a clean unmount, with no panic or use-after-free.
     - After a replug the disk works again.

8. **Acceptance and regression**
   - **Deliverable:**
     - QEMU cases: HS and SS attach, 4 KiB logical sectors, a sparse disk larger than 2 TiB, a read-only drive, media not ready, storage behind nested hubs, and QMP `device_del`/`device_add` of the hub and of the disk during transfers (50 cycles under CPU load).
     - Physical hardware covering what QEMU cannot:
       - a USB 2.0 hub (single-TT) with a FS/LS keyboard or mouse behind it, which exercises the TT
       - a multi-TT USB 2.0 hub
       - a USB 3.x hub, which on its USB2 side is a companion hub on the same cable
       - the machine's own internal hubs (front panel)
       - a USB 2 stick and a USB 3 SSD enclosure, both directly and behind a hub
     - Regression: root-port kbd, mouse, and audio, plus NVMe/AHCI boot. S3/S4 resume either revalidates hubs and disks or removes and re-adds them cleanly.
     - Runs with `make rust-test`, `cargo clippy`, and `USE_RUST=0`.
   - **Exit:** All cases pass with no leaked slots, rings, bounce buffers, Rust slot-table entries, or block slots, as shown in `/proc/usb`, `/proc/usb_xhci`, and `/proc/usb_storage`.

## Risks and decisions to settle early

- **The FFI boundary is where bugs will hide.** Keep the shim narrow: opaque handles, integers, byte slices. Avoid mirroring kernel structs. Any lifetime crossing the boundary (a pipe handle used after the device is gone) must be invalidated on the C side and must report Gone to Rust instead of dangling.
- **Panics halt the machine.** Rust driver panics have no recovery path. Deny-lints plus fallible APIs are the main defence. Keep `unsafe` confined to `ffi.rs` and `kernel.rs`.
- **Removal semantics:** Decide early whether a removed block device stays as a zombie until unmount (safest) or frees its slot immediately. Hub removal makes this common, because one unplug removes several devices.
- **TT and SS hubs cannot be tested in QEMU.** Plan for physical hub testing before calling hubs done, since a wrong TT Hub Slot ID only shows up on real hardware.
- **Enumeration concurrency:** Several hub workers can call into `usb.c` at once. The core needs a topology lock, and xHCI command submission is already serialised by `command_lock`.
- **Throughput:** A synchronous BOT pipeline with a 128 KiB bounce buffer should give USB 2.0 roughly 30–35 MB/s. USB 3 above about 100 MB/s needs UAS or larger transfers with direct DMA.

## Deferred

UAS (`08/06/62`, streams), hubs and bulk on EHCI/OHCI/UHCI, multi-LUN card readers and media-change polling, CB/CBI and UFI floppies, TRIM/UNMAP, native exFAT/NTFS, hub and port selective suspend and runtime autosuspend, USB Type-C/PD, and booting from USB.

## References

- [USB 2.0 specification](https://www.usb.org/document-library/usb-20-specification), chapter 11 (Hub)
- [USB 3.2 specification](https://www.usb.org/document-library/usb-32-revision-11-june-2022), chapter 10 (Hub, SuperSpeed)
- [USB Mass Storage Class – Bulk-Only Transport 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)
- [USB Mass Storage Class – Specification Overview 1.4](https://www.usb.org/sites/default/files/Mass_Storage_Specification_Overview_v1.4_2-19-2010.pdf)
- SCSI Block Commands (SBC-3) and SCSI Primary Commands (SPC-4), T10
- [Intel xHCI specification, revision 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf) §4.3 (device slot and route string), §4.6.6 (Configure Endpoint), §4.8.3 (halted endpoints), §4.11 (TRBs), §6.2.2 (Slot Context: Hub, MTT, TT fields)
