# amdgpu testing notes (Phase 5 → 6)

How to run, capture and compare the Raphael iGPU under VFIO on this host, and
what is already known about the device.  Written at the Phase 5 closeout
(2026-09-13) so a Phase 6 agent can reproduce every step.

## Host facts

| Fact | Value |
|---|---|
| GPU | `0000:0e:00.0` = `1002:164e` rev c6 (Raphael iGPU, gfx1036/DCN 3.1.x) |
| Host driver | `vfio-pci` (checked with `lspci -nnk -s 0000:0e:00.0`) |
| IOMMU group | 24; `/dev/vfio/24` is `root:users` (the login user is in `users`) |
| GPU audio function | `0000:0e:00.1` stays on the host; only `0e:00.0` is passed |
| VBIOS source | firmware VFCT table — **no ROM BAR** (`/sys/.../rom` does not exist) |

## VBIOS extraction (host, root)

```sh
sudo scripts/vfio-vbios.sh 0000:0e:00.0
```

- With a real option ROM, the script enables and reads
  `/sys/bus/pci/devices/<BDF>/rom`.
- On this APU there is no ROM node, so it parses `/sys/firmware/acpi/tables/
  VFCT` (AMD ACPI table; layout from Linux 6.6 `atomfirmware.h`): it walks
  `uefi_acpi_vfct.vbiosimageoffset` and matches `vfct_image_header` entries on
  PCI bus/device/function + vendor `1002`, then verifies the `55 AA` option
  ROM signature.
- The image is padded to the next power of two with `0xFF` (this one:
  44,544 → 65,536 bytes) so the QEMU ROM BAR size, the guest's
  `pci_map_rom()` size and the CRC32 comparison all agree.
- Output: `build/vfio/vbios.rom`; `make run-vfio` auto-detects it and adds
  `romfile=` alongside `rombar=1`.

After a run, compare the guest's logged CRC with the host file:

```sh
scripts/vfio-check-rom.sh build/logs/p5-vfio.log
# guest: crc32=0x20ef861b size=65536
# host : crc32=0x20ef861b size=65536
# MATCH
```

## Running

Headless capture (recommended for evidence):

```sh
make run-vfio KERNEL_CMDLINE=kpi_vfio_test=1 \
     SERIAL=file:build/logs/p5-vfio.log VFIO_EXTRA='-device edu'
```

Interactive with the virtual desktop (`virtio-vga` at 1280x800) while the real
GPU stays on the VFIO device:

```sh
make run-vfio KERNEL_CMDLINE=kpi_vfio_test=1 VFIO_EXTRA='-device edu' \
     QEMUFLAGS='-vga none -device virtio-vga,xres=1280,yres=800'
```

Knobs:

| Variable | Meaning |
|---|---|
| `VFIO_BDF` / `VFIO_MEM` / `VFIO_ROM` | passthrough device, guest RAM, ROM file |
| `VFIO_EXTRA` | extra QEMU `-device` args (e.g. `-device edu`) |
| `SERIAL` | `stdio` (default) or `file:...` |
| `DISPLAY_OPT` | `-display none` for headless, default opens a window |
| `KERNEL_CMDLINE` | limine `cmdline:` baked into the ISO; module params parsed by `linuxkpi_param_parse()` |
| `QEMUFLAGS` | appended last; override for virtio-vga / display choices |

`kpi_vfio_test=1` gates `test_phase5_vfio.c`, which binds the passed GPU:
identity/BARs, BAR5 map + one read, bus-master set/clear, ROM + VBIOS CRC32,
one MSI/MSI-X vector + `request_irq`, then teardown.  Without the parameter
the suite logs `[SKIP]`, so a Phase 6 boot can let amdgpu own the device.

The default boot (no flags) enumerates the GPU but no driver binds it; the
desktop renders on `virtio-vga` and Mesa uses llvmpipe.  fastfetch's native
`gpu` module already lists `AMD Raphael` from PCI IDs.

### C6 physical-monitor run

*Not exercised on this host (2026-09-14): the main monitor is on the NVIDIA
card and cannot be replugged, and the passed iGPU's motherboard ports have
no spare monitor / dummy plug / capture dongle; C6 closed on the
emulated-sink path.  This is the recipe for when a sink is available.*

1. Connect a monitor/cable to one of the passed GPU's HDMI/DP outputs (the
   motherboard outputs on this APU).  The host needs another display path for
   the duration of the run (see the C6 note below).
2. `make run-c6` (DC on, `drm.debug=0x4`, serial to `build/logs/p6-c6.log`),
   log in, then run:
   ```sh
   bin/test_kpi_amdgpu
   ```
3. Evidence in the log: `dcn physical <connector>` plus the monitor's real
   Modeline (its EDID read over DDC), then `atomic enable commit`, both
   page-flip events, `WAIT_VBLANK`, cursor commit/off and the atomic disable
   on the physical connector - with no `[FAIL]` and no WARN.  Hot-plugging
   the monitor after boot is the HPD IRQ check (watch for the connector
   hotplug event).

## Firmware staging (Phase 6)

`scripts/linux-firmware-install.sh` copies the names in
`scripts/linux/firmware-manifest.txt` into the image's `/lib/firmware`; the
Phase 5 self-test uses a deterministic 4 KB blob staged the same way.  The
Raphael firmware names (SMU/PSP/DMCUB) belong in that manifest before P6c;
nothing is installed by default yet.

## Known issues / gotchas

- **Warm GPU state between runs.**  QEMU does not reset the passed-through
  iGPU when a VM exits.  A run that got as far as loading PSP SOS and was
  then killed leaves the PSP running; the next `make run-vfio` can fail
  early with `PSP create ring failed!` / probe `-22` because the bootloader
  handshake no longer answers.  The device exposes only the `bus` reset
  method (`/sys/.../reset_method` = `bus`; no FLR/PM), so try in order:
  1. just retry the boot -- a failed PSP handshake followed by the driver's
     `psp_ring_destroy()` teardown has unstuck it before (fail -> success
     across two consecutive runs);
  2. function reset **while the device is still bound** (unbinding first can
     put it into D3cold, after which config space is gone and every reset
     method returns `-ENOTTY`):
     ```sh
     cat /sys/bus/pci/devices/0000:0e:00.0/reset_method   # reports "bus"
     sudo sh -c 'echo 1 > /sys/bus/pci/devices/0000:0e:00.0/reset'
     ```
     On this box the write fails with `Inappropriate ioctl for device`: the
     `reset_method=bus` probe passes, but `0e:00.0` shares bus `0x0e` with
     five other functions (HD audio, PSP/CCP, two xHCI USB controllers, more
     audio), and the kernel refuses a parent-bus reset of a shared bus.
     **Do not force a secondary-bus reset of `00:08.1` with `setpci`** - it
     would reset those USB controllers as well.  On this machine a host
     reboot is therefore the only safe way to cold-start the GPU; use
     `make reset-gpu` to at least re-bind vfio-pci (it reports the actual
     reset capability and never leaves the function orphaned).
  3. reboot the host.
  Once amdgpu is bound, prefer a clean guest shutdown over killing QEMU.
  **Confirmed 2026-09-13:** a clean guest `poweroff` (ACPI S5) leaves the
  passed GPU cold for the next boot - C6 runs after a guest poweroff came
  up without the warm `-22`, so a host reboot is only needed if a run was
  killed rather than powered off.  The guest-side `pci_reset_function()` is
  still a `-ENOTSUPP` stub (P5 C1
  gap), so the driver's own recovery path cannot reset the device yet.
  `make reset-gpu` (`sudo scripts/vfio-reset-gpu.sh [BDF]`) automates step 2
  and is now the recommended way to start a C4+ evidence run: the driver's
  bring-up sequence assumes a cold device, and a warm one shows up as
  `AUTOLOAD_RLC` answering `TEE_ERROR_BUSY` or the KIQ HQD coming back with
  cleared doorbell state (`dbctl=0`) and a `-110` ring test.
- **C4 headless runs use `amdgpu.dc=0`.**  It makes
  `amdgpu_device_asic_has_dc_support()` false, so the `dm` ip block is never
  added: no DMUB hardware init / DCN register bring-up (still slow and not
  needed before C6), just PSP/SMU/GMC/IH and the SDMA/GFX rings.  Use the
  dedicated target -- it bakes the right cmdline, goes headless and captures
  `build/logs/p6-c4.log`:
  ```sh
  make run-c4
  ```
  (`KERNEL_CMDLINE` is baked into the ISO; the top Makefile now stamps its
  value, so changing it always rebuilds the ISO instead of silently reusing
  the previous command line.)
- **C5 hardware runs use `make run-c5`.**  Same bring-up command line as
  `run-c4`, but the GTK window is kept so `bin/test_kpi_amdgpu` can be run
  interactively after login; the serial log lands in `build/logs/p6-c5.log`.
  Start from a cold GPU (`make reset-gpu` when it can help, otherwise a host
  reboot), boot, then:
  ```sh
  bin/test_kpi_amdgpu
  ```
  It discovers the amdgpu node by DRM version name and skips cleanly when the
  driver is not bound.
- **One QEMU per disk image.**  Close the interactive session before a
  headless run, or point the run at a scratch copy
  (`cp --reflink=auto disk.img build/disk-c6.img`).
- **The default std VGA is a bochs VGA** (`1234:1111`).  `run-vfio` without
  `-vga none` makes the P4 bochs canary run on it and fail the BAR0 pattern
  readback (it expects `-device bochs-display`).  Adding
  `-vga none -device virtio-vga` makes it skip and gives the C1 ROM test a
  virtio-vga device.
- **Display output from the passed GPU (C6).**  Once AvoryOS binds amdgpu with
  DC on (`run-c6`), KMS drives the GPU's physical HDMI/DP connectors and a
  monitor attached to them shows the guest.  While the VM holds the device
  (`vfio-pci`), the host does **not** drive it, so the host must not depend on
  the passed GPU for its own display: keep a second monitor / another GPU /
  SSH+serial for host control.  The GTK window remains the virtual desktop.
  With no sink attached the DCN suite falls back to a generated EDID override
  and logs `dcn forced ... (EDID override)`; with a sink it logs
  `dcn physical ...` and uses the monitor's own EDID over DDC.
- **`build/vfio/vbios.rom` is host-extracted**; never commit it (gitignored
  build tree), and re-extract after firmware updates.
- **Do not write to the GPU's MMIO/config from the C7 test**; it only reads
  BAR5 once and the ROM.  Keep it that way so a P6a run can bind amdgpu on
  the same boot.
- **fastfetch's GPU line** is the native `gpu` module now; if it ever prints
  nothing, check that the guest's `/sys/bus/pci/devices/<bdf>/` still exposes
  `vendor`, `device`, `class` and `subsystem_vendor`.

## Phase 6 expectations

Full chunked plan: `docs/linuxkpi-phase6-plan.md` (C0–C9, written
2026-09-13).  Summary:

1. P6a: compile/link the amdgpu subset against the Phase 5 LinuxKPI surface
   (no hardware ownership).
2. P6b/P6c: bind + firmware (SMU/PSP/DMCUB) from `/lib/firmware`.
3. P6d: rings/fences/scheduler; `drm_sched` already carried P4/P5 tests.
4. P6e: display — connectors, EDID over the C4 i2c core, atomic modeset;
   this is when the desktop can move to the real GPU and fastfetch's output
   becomes meaningful beyond PCI enumeration.
