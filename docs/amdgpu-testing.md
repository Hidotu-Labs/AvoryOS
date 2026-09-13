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

## Firmware staging (Phase 6)

`scripts/linux-firmware-install.sh` copies the names in
`scripts/linux/firmware-manifest.txt` into the image's `/lib/firmware`; the
Phase 5 self-test uses a deterministic 4 KB blob staged the same way.  The
Raphael firmware names (SMU/PSP/DMCUB) belong in that manifest before P6c;
nothing is installed by default yet.

## Known issues / gotchas

- **One QEMU per disk image.**  Close the interactive session before a
  headless run, or point the run at a scratch copy
  (`cp --reflink=auto disk.img build/disk-c6.img`).
- **The default std VGA is a bochs VGA** (`1234:1111`).  `run-vfio` without
  `-vga none` makes the P4 bochs canary run on it and fail the BAR0 pattern
  readback (it expects `-device bochs-display`).  Adding
  `-vga none -device virtio-vga` makes it skip and gives the C1 ROM test a
  virtio-vga device.
- **No display output from the passed GPU yet.**  The guest has no amdgpu, so
  the physical connectors show nothing; the GTK window is the virtual
  desktop.  Real display output starts at P6e (KMS/connectors/EDID).
- **`build/vfio/vbios.rom` is host-extracted**; never commit it (gitignored
  build tree), and re-extract after firmware updates.
- **Do not write to the GPU's MMIO/config from the C7 test**; it only reads
  BAR5 once and the ROM.  Keep it that way so a P6a run can bind amdgpu on
  the same boot.
- **fastfetch's GPU line** is the native `gpu` module now; if it ever prints
  nothing, check that the guest's `/sys/bus/pci/devices/<bdf>/` still exposes
  `vendor`, `device`, `class` and `subsystem_vendor`.

## Phase 6 expectations

1. P6a: compile/link the amdgpu subset against the Phase 5 LinuxKPI surface
   (no hardware ownership).
2. P6b/P6c: bind + firmware (SMU/PSP/DMCUB) from `/lib/firmware`.
3. P6d: rings/fences/scheduler; `drm_sched` already carried P4/P5 tests.
4. P6e: display — connectors, EDID over the C4 i2c core, atomic modeset;
   this is when the desktop can move to the real GPU and fastfetch's output
   becomes meaningful beyond PCI enumeration.
