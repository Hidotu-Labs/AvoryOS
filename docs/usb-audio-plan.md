# USB audio implementation plan

## First milestone

Support full-speed, directly attached UAC1 playback: S16_LE, stereo, 48 kHz, isochronous OUT using synchronous or adaptive synchronization, with no host feedback adjustment. The pinned QEMU `usb-audio` fixture specifically uses synchronous synchronization. Async playback and feedback endpoints are out of scope.

## Ordered phases

1. **Fixture and descriptor contract**
   - **Deliverable:** Save the chosen fixture's configuration/interface/endpoint descriptors and document the exact UAC1 topology and supported format. Extend parsing design to cover alternate settings and class-specific AudioControl/AudioStreaming descriptors.
   - **Exit:** Fixture is confirmed full-speed, directly attached, and offers the milestone format and sync mode; unsupported descriptors can be rejected without disturbing existing USB devices.
2. **Isochronous xHCI path**
   - **Deliverable:** Add an isochronous HCD stream API and xHCI endpoint/context configuration, interval scheduling, packet completion reporting, DMA buffer ownership, and explicit stop/cancel. Define ownership of endpoint rings and stream buffers from creation through completion and disconnect; ensure cancellation drains/stops hardware before storage is reused.
   - **Exit:** A focused xHCI exercise submits and retires scheduled packets without corrupting existing interrupt pipes; stop, transfer error, and unplug all release ring/buffer ownership exactly once.
3. **USB PCM and per-card registry**
   - **Deliverable:** Add a USB PCM playback backend for the existing sound interface. Replace assumptions tied to global HDA state with a per-card/device registry, so USB and HDA nodes have independent PCM state, lifetimes, and removal. Keep the existing `/dev/snd` and sysfs conventions for ALSA/PipeWire discovery.
   - **Exit:** ALSA clients can open the USB playback PCM, negotiate only the milestone format, stream audio, and close it; the existing HDA card remains independently available.
4. **Hotplug and audio-server integration**
   - **Deliverable:** Connect USB arrival/removal to sound-node registration and removal; verify udev coldplug and PipeWire/WirePlumber discover, select, and drop the USB sink. Add diagnostics for negotiated format, stream state, interval, queue depth, underruns, and xHCI transfer errors.
   - **Exit:** Repeated plug/unplug and stream start/stop produce no stale nodes, stuck streams, or leaked resources; user-space sees the sink and recovers after removal.
5. **Acceptance and regression**
   - **Deliverable:** Run descriptor cases (valid, malformed, unsupported, multiple alternates), QEMU playback and hotplug under CPU load, and a physical full-speed device check. Retain build coverage with USB audio enabled/disabled.
   - **Exit:** The fixture plays continuously at the specified format with bounded queueing and no recurring underruns or DMA/lifetime faults; USB keyboard/mouse and HDA playback regressions pass.

## Phase 1–2 status

- **Phase 1 — complete.** The QEMU 11.1.2 `usb-audio` fixture is full-speed and directly attached to `qemu-xhci`, with synchronous synchronization. Its captured device and 113-byte configuration descriptors, UAC1 topology, and format contract are recorded in the [fixture](fixtures/qemu-usb-audio-uac1.md). The parser accepts either synchronous or adaptive synchronization for otherwise supported endpoints. `make run-usb-audio` provides a headless TCG launch; use `USB_AUDIO_DISK=/path/to/valid-disk.img` if the default disk lacks an ext filesystem.
- **Phase 2 — implemented and exercised.** With `usb_audio_probe=1`, the probe logs PASS for ring wrap, reopen, and cancellation with 128 packets queued. QMP unplug/replug was exercised both while idle and with packets outstanding. USB HID still attaches, and HDA enumerates alongside the fixture.
- **Phase 3 — implemented and QEMU verified.** USB appears as a separate card1 while HDA remains available. Udev sets `SOUND_INITIALIZED`; libasound S16_LE stereo at 48 kHz drained 96,013 frames, including the partial tail.
- **Phase 4 — implemented and QEMU verified.** Targeted PipeWire USB playback captured a 480 Hz tone; direct USB PCM playback also confirmed a 960 Hz tone in the emulated USB WAV. Unplug during a PipeWire stream removed the USB sink and `/dev` node; replug rediscovered it while HDA also played.
- **Phase 5 pending:** Physical full-speed hardware and remaining acceptance/regression checks are not yet verified.

## Deferred

UAC2, capture, asynchronous playback and host feedback adjustment, other rates/formats/channel maps, mixer controls, hubs/non-xHCI controllers, and suspend/resume follow after the first milestone.

## References

- [USB Audio Device Class 1.0 specification](https://www.usb.org/sites/default/files/audio10.pdf)
- [Intel xHCI specification, revision 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf)
