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
2. `make run-vfio KERNEL_CMDLINE="kpi_amdgpu=1 amdgpu.runpm=0 amdgpu.ppfeaturemask=0xfff73fff drm.debug=0x4"` (Display Core enabled),
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
The build installs firmware after replacing `/lib` with a real directory for
the compatibility runtime. `scripts/image-firmware-install.sh` populates
`/lib/firmware` and verifies every installed blob against its staged bytes;
the image build fails if a file is missing or differs. The rootfs overlay also
carries `/usr/lib/firmware`, but that alone is insufficient when `/lib` is no
longer a symlink to `/usr/lib`.

## Known issues / gotchas

- **Native PCI configuration access (2026-10-12).** The kernel's byte/word
  accessors previously used dword read-modify-write transfers, including
  through PCIe ECAM. A command or capability-control write could therefore
  clear neighboring write-one-to-clear status bits. The native accessors now
  preserve the requested transfer width; legacy CF8/CFC transfers also hold
  an IRQ-saving spinlock around the shared address latch and data transfer.
  LinuxKPI uses these native accessors without an AMDGPU source change.
  `python3 scripts/tests/test_pci_config_transactions.py` checks status-bit
  preservation, all three widths, extended ECAM offsets and concurrent legacy
  transfers. Recreating the former dword word-write fails the W1C check.
  This fixes a demonstrated PCI implementation bug, but does not establish
  the cause of the reported host reboot. The temporary PSP BUSY rejection
  patch was removed; firmware response handling remains upstream behavior.
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
  Clean power-off is preferable, but is not a guaranteed cold reset.
  The native reboot/power-off/halt path now dispatches bound imported PCI
  drivers' shutdown callbacks before storage shutdown and ACPI. AMDGPU's
  existing callback suspends its IP blocks and unloads firmware state for
  passthrough shutdown; previously it was never called. A private boot on
  2026-10-11 completed this callback and the following boot initialized AMDGPU
  successfully. Forced QEMU termination bypasses guest shutdown. These checks
  do not prove a fix for the reported host reboot: the previous host journal
  ended without a recorded panic or orderly shutdown.
  The guest-side `pci_reset_function()` is
  still a `-ENOTSUPP` stub (P5 C1
  gap), so the driver's own recovery path cannot reset the device yet.
  `make reset-gpu` (`sudo scripts/vfio-reset-gpu.sh [BDF]`) automates step 2,
  but cannot reset this shared-bus GPU when the host refuses that reset. The driver's
  bring-up sequence assumes a cold device, and a warm one shows up as
  `AUTOLOAD_RLC` answering `TEE_ERROR_BUSY` or the KIQ HQD coming back with
  cleared doorbell state (`dbctl=0`) and a `-110` ring test.
- **Use `make run-c7-prime` for AMDGPU sessions.** Display Core remains
  enabled for the AMD primary renderer and VirtIO output. The former C4/C5
  launchers used `amdgpu.dc=0` for early driver bring-up and are retired.
  `bin/test_kpi_amdgpu` remains available after login for raw DRM diagnostics;
  it discovers the AMD node and skips when the driver is not bound.
- **One QEMU per disk image.**  Close the interactive session before a
  headless run, or point the run at a scratch copy
  (`cp --reflink=auto disk.img build/disk-c6.img`).
- **The default std VGA is a bochs VGA** (`1234:1111`).  `run-vfio` without
  `-vga none` makes the P4 bochs canary run on it and fail the BAR0 pattern
  readback (it expects `-device bochs-display`).  Adding
  `-vga none -device virtio-vga` makes it skip and gives the C1 ROM test a
  virtio-vga device.
- **Display output from the passed GPU (C6).**  Once AvoryOS binds amdgpu with
  DC on (`run-vfio` with `kpi_amdgpu=1`), KMS drives the GPU's physical HDMI/DP connectors and a
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

## PRIME run (radeonsi + accelerated sessions)

```sh
make run-c7-prime
```

This is the supported one-monitor setup: AMD renders the desktop and VirtIO
presents it in the QEMU window. Start `sddm` in the guest. The packaged
`bin/test_kpi_radeonsi` remains available for GBM/EGL and PRIME diagnostics.

PRIME uses a relative USB mouse by default so Minecraft/GLFW camera capture
receives continuous relative motion through the native USB HID and evdev
drivers. The old absolute USB tablet is unsuitable for captured game cameras.
The native VirtIO driver leaves virtual hardware cursors unavailable for a
relative pointer. Xorg then draws its normal software cursor into the scanout,
so GTK can hide its host pointer (`GTK_SHOW_CURSOR=off`) during capture without
hiding the guest cursor. Games can hide the guest cursor normally.
Click inside the QEMU window to capture input; Ctrl+Alt+G toggles the GTK
mouse and keyboard grab. Restart the VM after changing the launcher; this
device selection does not require rebuilding `disk.img`. The native cursor
capability change requires the updated kernel ISO.
Ordinary `run-vfio` retains its tablet default. An explicit
`VFIO_POINTER=usb-tablet` override remains available for tablet-only sessions.

The old `run-c4`, `run-c5`, `run-c6`, `run-c7`, `run-linuxdrm` and C4/C6
watchdog commands have been retired. The historical C7 emulated-connector mode displayed sessions on an
invisible AMD output; use PRIME for the QEMU window. Native DRM, LinuxKPI and
AMDGPU regression test sources remain available.

## Phase 6 expectations

### PRIME offload to the QEMU window (one monitor)

`make run-vfio` and `make run-c7-prime` now select the AMD-primary Xorg
configuration when building the guest disk. AMD owns screen 0; VirtIO is the
output GPU for the QEMU window. Both VFIO PCI slots are fixed: AMD at
`PCI:0:3:0`, VirtIO at `PCI:0:6:0`. Display Core stays enabled because Xorg's
primary modesetting device needs KMS resources, even with no physical AMD
monitor. The PRIME target defaults to 8 vCPUs (`VFIO_SMP=N` overrides this).

The configuration lives in `config/xorg/10-modesetting.conf`. Disk builds
default to this AMD-primary profile, including standalone `make disk.img`
and `make all`, so rebuilding outside a VFIO launch cannot restore card0 as
the renderer. Use `XORG_PROFILE=native` explicitly when building a disk for
a desktop without passthrough; this selects `config/xorg/10-native.conf`.
Switching launch targets reuses the existing
disk and its installed configuration; it does not repopulate the disk to switch
profiles. Set `XORG_PROFILE=amdgpu` or `XORG_PROFILE=native` when explicitly
rebuilding the disk to change that configuration. The native PCI bridge publishes imported driver
bindings in sysfs so libudev can identify AMD. Xorg's AutoBindGPU associates
the virtual output provider. `/usr/bin/avory-prime-display` explicitly connects
RandR Sink Output providers to the Source Output provider, then enables the
VirtIO HDMI output as the sole active, primary output. The VFIO profile's
secondary connector uses Xorg's `HDMI-<port>-<GPU>` naming. The synthetic AMD
HDMI head is disabled; it has no physical monitor supplying vblank events.
AMD page flips and VirtIO's PRIME Synchronization property are disabled so
Xorg uses PRIME damage updates for presentation. SDDM runs this through DisplayCommand
before the greeter; `/etc/X11/xinit/xinitrc.d/45-prime-output.sh` also runs it
for console-started sessions such as `startxfce4`. Provider IDs are discovered
from capabilities rather than hard-coded or selected by their identical
`modesetting` names. The host regression is
`python3 scripts/tests/test_prime_display_startup.py`.

Native VFS fixes make this discovery possible with current libudev: `statx`
reports stable mount IDs, hand-built sysfs directories have distinct inode
numbers, and `O_PATH|O_NOFOLLOW`, empty-path `readlinkat` and directory dot
components preserve Linux path-walking semantics. Before these changes,
libudev's DRM scan returned `-EUNATCH` or rejected the card paths, leaving
Xorg to PCI probing instead of creating the secondary output GPU. An isolated
VirtIO boot now enumerates DRM nodes and Xorg logs `Platform probe` for card0.
The regression is `python3 scripts/tests/test_vfs_device_identity.py`.

DRM event reads preserve nonblocking semantics as well. The imported file
bridge propagates the native descriptor's `O_NONBLOCK` setting into Linux
`file->f_flags` before a read and returns errors such as `EAGAIN` unchanged.
The native VirtIO event reader returns `EAGAIN` when its queue is empty and
honors interrupted blocking reads. This prevents an event drain from parking
the X server on a descriptor it made nonblocking. The regression is
`python3 scripts/tests/test_drm_nonblocking_read.py`.

After boot, start `sddm` normally and choose XFCE, or use `startxfce4`.
Applications then inherit AMD as Xorg's default renderer:

```sh
glxinfo -B
glxgears
```

The legacy `startx.sh` regenerates its own Xorg configuration; use the distro
launchers above for this static configuration.

Xorg's modesetting output sink imports AMD's dma-buf and creates its framebuffer
with legacy ADDFB. That path must prepare the VirtIO resource and contiguous
upload shadow, just as ADDFB2 does. Without it, the framebuffer exists but
VirtIO silently skips its uploads, leaving the QEMU window black. The kernel
PRIME self-test now exercises legacy ADDFB on a newly imported buffer, including
unsupported-format rejection and framebuffer teardown.

On 2026-10-11, the isolated VFIO regression used AMD as Xorg's primary and
VirtIO as the output sink. GLX reported accelerated radeonsi, and `xsetroot`
produced a captured 1280x800 QEMU frame with all 1,024,000 pixels red. The same
test was black before preparing imported buffers in legacy ADDFB. Evidence:
`build/logs/prime-present-fixed.png` and
`build/logs/prime-present-fixed-Xorg.0.log`.

SDDM also exposed a signal-consumption bug after Xorg initialized: glibc's
`__nptl_setxid` sends a thread-directed signal to each Mesa worker and waits for
every acknowledgement before finishing Xorg's credential change. Delivering
one signal previously cleared the pending bit on all siblings, leaving this
barrier stuck and Xorg unable to answer SDDM's cursor and RandR requests.
The signal state now records thread-directed pending bits separately from
process-wide replicas. Delivery consumes only the target's directed bit;
signalfd likewise cannot consume another thread's directed signal. Regression:
`python3 scripts/tests/test_thread_directed_signals.py`.
The full guest-disk snapshot regression then reached SDDM's greeter in QEMU;
Xorg logged radeonsi glamor acceleration and VirtIO PRIME damage tracking.
The captured login screen is `build/logs/prime-sddm-fixed.png`, with Xorg's
log in `build/logs/prime-sddm-fixed-Xorg.0.log`.

The initial greeter capture was insufficient: enabling both AMD's synthetic
head and VirtIO with `xrandr --auto` subsequently produced repeated
`Present-flip: queue flip during flip ... Device or resource busy` messages.
The output-only setup above was checked with the normal QEMU USB/EDU devices
on a private guest-disk copy. AMDGPU initialized successfully, Xorg reported
radeonsi glamor, and the greeter continued updating through successive
1280x800 QEMU captures without shared-pixmap or Present-flip errors. Evidence:
`build/logs/prime-output-only.png` and
`build/logs/prime-output-only-Xorg.0.log`. GPU warm-start probe failures are
excluded from this rendering result.
The follow-up also ran `/etc/init.d/rcS` and sourced `/etc/profile` before
SDDM, matching the normal boot services and login environment. A QEMU test
key changed the visible password field; the captured changed pixels were
inside that field, demonstrating continued presentation after startup.
That capture and Xorg log are `build/logs/prime-normal-services.png` and
`build/logs/prime-normal-services-Xorg.0.log`.

Concurrent desktop GPU clients exposed additional LinuxKPI races. A
private-disk XFCE/Alacritty/glxgears reproduction first stalled in
DRM_SYNCOBJ_WAIT and reported a graphics-ring timeout with soft recovery.
`set_current_state()` had been a no-op: a fence callback arriving before the
native timeout helper armed its wake flag was lost. It now prepares the wait
before upstream checks the fence; waitqueue preparation/cleanup use the same
handoff. The forced boundary regression is
`python3 scripts/tests/test_kpi_prepared_wait.py`, also exercised by the guest
`sched prepared wait` boot probe. The old timeout-only wake logic fails it.

A longer reproduction then caught Mesa's command submission blocked on the
AMDGPU notifier mutex. The LinuxKPI mutex slow path held the waiter-list lock
but assigned ownership without a compare/exchange, allowing a concurrent
fast-path acquisition to be overwritten. Both acquisition paths now use the
same atomic ownership operation, and unlock publishes the next owner with
release ordering. `python3 scripts/tests/test_kpi_mutex_contention.py` runs
96,000 competing acquisitions using the actual mutex implementation. The
original implementation deadlocked in this host comparison; the fixed one
completed with exactly one owner and no stranded waiters. Reproduction logs:
`build/logs/prime-xfce-lost-fence-wake.log` and
`build/logs/prime-xfce-mutex-stall.log`.

The mixed-client reproduction still stalled after those two fixes. The
workqueue worker kept `pending` set through a callback, rejecting a requeue
while that callback was running. This loses deferred interrupt processing
and retry work. Workers now clear pending before invoking the callback and
select queued entries under the queue lock, skipping entries still running
on another worker. Flush waits for both queued and executing work, and every
completion wakes flush waiters. The regression
`python3 scripts/tests/test_kpi_workqueue_requeue.py` runs the actual queue and
worker functions with two concurrent workers. It checks requeueing during
execution, duplicate rejection, single execution, flush, and cancellation;
the original code fails the running-work requeue assertion. The first
private-disk desktop verification kept Alacritty, XFCE About, and glxgears
rendering for over a minute without a graphics-ring timeout, then accepted
Escape to close glxgears. Its serial capture is
`build/logs/prime-workqueue-first-pass.log`. A subsequent sustained run
still froze after roughly 45 seconds with another soft-recovered ring timeout;
these compatibility fixes do not yet establish that the desktop stall is
resolved. That capture is `build/logs/prime-workqueue-long-stall.log`.

Automatic stalled-frame capture then found `gfx_0.0.0` BLOCKED with both
KPI wake flags set. Native `sched_wakeup()` returned immediately on a
RUNNING/READY state before taking the target's queue lock. A waker could
therefore set the pending flag just after the waiter consumed it, observe
RUNNING and return, while that waiter subsequently published BLOCKED under
the lock. The native wake decision now happens under the same queue lock as
blocked/sleeping-state publication. The regression
`python3 scripts/tests/test_scheduler_wake_publication.py` executes the real
native KPI wake and scheduler wake functions, forcing the waker into this
boundary with a held queue lock. It covers both BLOCKED and SLEEPING, with
the target still current and with it switched out. Restoring the old fast
return deterministically leaves the target parked. Capture:
`build/logs/prime-scheduler-lost-wakeup.log`.

With the serialized scheduler wake, a fresh private copy of `disk.img`
completed over five minutes of visible radeonsi glxgears rendering inside
Alacritty on XFCE. The capture at 360 seconds still showed advancing frame
counts, and an injected Alt+F1 opened the Applications menu while rendering
continued. No graphics-ring timeout or GPU reset appeared in the serial
capture. Evidence: `build/logs/prime-scheduler-wake-verified.log`,
`build/logs/prime-scheduler-wake-rendering.png`, and
`build/logs/prime-scheduler-wake-keyboard.png`. The normal boot ISO was rebuilt
and its stripped kernel compared byte-for-byte against the new build.
The About process in that fresh-disk run separately remained in a userspace
futex wait in libglycin's image loader, with its Mesa submission worker idle;
that image-loader startup wait is outside the successful GPU freeze test.

`make run-c7-prime` retains its serial logfile and now also exposes
`build/logs/p6-c7-prime-serial.sock`. During a future live stall,
`python3 scripts/prime-dump.py` requests a kernel thread dump from the host,
without needing a working guest console. The report goes into the same
`build/logs/p6-c7-prime.log`.

Renderer identification alone does not verify visible presentation or XFCE
compositing. The captured Xfwm abort occurred in libepoxy's
`glXBindTexImageEXT` resolver, reporting `GLX_EXT_texture_from_pixmap` as
unavailable. Mesa's DRI3 screen initialization deliberately disables that
extension when the render and display GPU descriptors differ:
`can_EXT_texture_from_pixmap = fd_render_gpu == fd_display_gpu`.
See [Mesa's DRI3 implementation](https://github.com/chaotic-cx/mesa-mirror/blob/main/src/glx/dri3_glx.c).
This policy cannot be changed by correcting a native DRM ioctl alone. No
compositor-mode override is installed to hide that failure.

Native DRM now exports real dma-bufs and can import Linux/AMDGPU dma-bufs,
retaining attachments, mappings, and implicit write fences. Foreign linear
XRGB8888 scanout buffers use a synchronized CPU copy into a VirtIO 2D resource;
this is not a zero-copy presentation path. Tiled modifiers are rejected rather
than interpreted as linear pixels. Other foreign scanout formats are unsupported.
The boot test exercises sharing between two device namespaces, repeated imports,
re-export after the original fd closes, and teardown without requiring AMD hardware.
The radeonsi test also checks AMD rendering into a virtual-display-owned buffer.
End-to-end Xorg offload still requires successful AMDGPU bring-up and Xorg DRI3
with dma-buf import/export support; confirm the renderer and visible frames on
the actual VFIO boot.

Native DRM's GET_UNIQUE returns the actual VirtIO `pci:0000:BB:SS.F`
identity so modesetting's PCI probe can match the display Device; the non-PCI
fallback retains its platform identity. Sysfs publishes native
`card0`/`renderD128` under VirtIO's PCI parent and AMD `card2`/`renderD129`
under AMD's parent. This identity must agree across `/sys/class/drm`,
`/sys/dev/char`, and DRM netlink events.

The native hardware-cursor fixes operate independently of GL rendering and
pixmap import. No new initrd cursor configuration is installed.
Each CRTC owns its legacy cursor framebuffer and retains the GEM buffer until
replacement, hiding or teardown. Closing the userspace handle must not free a
buffer still used by the cursor plane. Cursor ioctls bind to the requested CRTC;
the boot regression checks two independent CRTCs and movement after handle close.
The kernel cursor queue retains the resource, scanout and hotspot
of deferred moves and cancels them when a newer move/hide/show is queued; a
completion worker must not derive a deferred command from mutable visibility
state, which can incorrectly send resource zero and hide the pointer.
`scripts/tests/test_virtio_cursor.c` tests these races against the actual driver
with a controlled transport. An isolated QEMU/Xorg run with hardware cursors
also exercised arrow/crosshair changes, movement, hiding and redisplaying the
pointer. A VNC RichCursor capture verified actual nonempty host cursor masks,
including 176 visible pixels for the initial cursor, rather than relying only
on queued commands. This uses native VirtIO with packaged Xorg; the actual
AMDGPU/VFIO desktop still needs confirmation after booting the updated images.

The Linux file bridge copies `inode->i_size` to the native VFS node when
installing a descriptor. Upstream AMD dma-buf exports previously left that
node's length zero, so Xorg's size discovery could lead to a zero-length mmap
and DRI3 BadAlloc even though radeonsi context creation succeeded. The
`test_kpi_dmabuf` regression failed before this correction and passed afterwards,
including mmap, invalidation, fork sharing and the allocation soak. The
radeonsi test checks size discovery on actual AMD GBM exports as well; visible
the AMD-primary Xorg PRIME output path is now verified by the capture above.

If `DRI_PRIME=1002:164e glxinfo -B` still reports llvmpipe, collect the actual guest logs:

```sh
DRI_PRIME=1002:164e LIBGL_DEBUG=verbose DRI_PRIME_DEBUG=1 glxinfo -B 2>&1
grep -Ei 'glamor|DRI2|DRI3|AIGLX|render|kms|\(EE\)' /var/log/Xorg.0.log
```

The AMD offscreen test proves rendering and shared-buffer writes; it does not
prove Xorg initialized glamor, negotiated DRI3, or passed a render fd to GLX.

Full chunked plan: `docs/linuxkpi-phase6-plan.md` (C0–C9, written
2026-09-13).  Summary:

1. P6a: compile/link the amdgpu subset against the Phase 5 LinuxKPI surface
   (no hardware ownership).
2. P6b/P6c: bind + firmware (SMU/PSP/DMCUB) from `/lib/firmware`.
3. P6d: rings/fences/scheduler; `drm_sched` already carried P4/P5 tests.
4. P6e: display — connectors, EDID over the C4 i2c core, atomic modeset;
   this is when the desktop can move to the real GPU and fastfetch's output
   becomes meaningful beyond PCI enumeration.
5. P6f (C7): Mesa radeonsi 3D through the render node, and the Xorg/Weston/
   KWin sessions select amdgpu when it can drive a display.


### Plasma X11 PRIME startup layout

Plasma 6's KScreen startup module re-enabled the emulated AMD `HDMI-1`
connector after the PRIME helper had selected VirtIO `HDMI-1-1`. The captured
session log showed an RRSetScreenSize change from 1280x800 to 3200x1080,
enabling the AMD head at 1920x1080 and leaving QEMU black. Both KWin and
plasmashell remained alive; no GPU timeout or process crash was recorded.
Evidence: `build/logs/prime-plasma-kscreen-session.log` and
`build/logs/prime-plasma-kscreen-black.log`.

The startplasma-x11 wrapper now detects a connected secondary PRIME HDMI
output, reapplies avory-prime-display, and prepends a runtime KConfig directory
with `[Module-kscreen] autoload[$i]=false` in `kded5rc` (the filename still used
by KF6 KDED). This disables automatic layout replacement only in that session.
The user configuration and native single-GPU session defaults are retained.
KScreen's backend can still answer PowerDevil queries. See upstream
https://github.com/KDE/kded/blob/master/src/kded.cpp for the config lookup.

A fresh private copy of disk.img reproduced the black screen with the original
wrapper and displayed the Plasma wallpaper and panel with the patched wrapper.
The desktop clock advanced and the successful run had no GPU timeout/reset.
`build/logs/prime-plasma-visible.png` captures the corrected virtual output.
The host configure-accounts.sh and disk.img's /usr/bin/startplasma-x11 both
contain the change. This verification establishes display startup; global
shortcut responses and launching Konsole through a shortcut were not confirmed.


### Xorg startup buffer initialization

Native GEM allocations previously published recycled PMM pages without clearing
all storage. A new dumb framebuffer could therefore show old page contents
before Xorg painted it. GEM creation now clears its entire page-rounded mapping
before publishing the handle; VirtIO also clears its owned PRIME staging pages
before attaching them to QEMU. Imported producer buffers are not cleared.

`scripts/tests/test_drm_gem_initialization.py` compiles the actual allocator with
recycled pages prefilled with 0xa5, verifies zeroing at publication and across
mmap padding, and checks that failed allocations remain unpublished. Removing
the clear makes this regression fail. The PRIME display startup checks pass and
`avoryos-x86_64.iso` was rebuilt with the changes (build log:
`build/logs/prime-startup-buffer-iso.log`). The reported symptom was a brief
startup flash, disappearing once the desktop painted; visual confirmation of
that flash's removal remains to be done on a subsequent guest boot.


### LinuxKPI PFN bounds across the Q35 PCI memory hole

The user's Plasma session recorded repeated AMD GTT allocation failures for
2 MiB buffers and radeonsi context-creation failures. Xorg reported
GL_OUT_OF_MEMORY in glReadPixels/PBO allocation, alongside a zero-length
read-only private mmap request. Original evidence is saved in
`build/logs/prime-user-mmap-zero.log` and
`build/logs/prime-user-mmap-Xorg.log`.

LinuxKPI page initialization used the sum of RAM region lengths as its physical
address ceiling. Those are different quantities when the memory map contains
holes. On Q35, managed RAM above 4 GiB could therefore be allocated by PMM but
rejected by pfn_to_page(), causing a false -ENOMEM in the Linux allocator.
PMM now exports the exclusive managed physical RAM end; LinuxKPI uses it for
max_pfn, physmem_end and sparse-section coverage. The page roundtrip boot test
also samples the actual physical bound. Total RAM accounting is unchanged.

`scripts/tests/test_kpi_page_physical_bounds.py` compiles the actual sparse
lookup and initializer with 4 GiB of RAM ending at physical address 5 GiB.
It verifies descriptors above 4 GiB and at the upper boundary, while rejecting
PFNs beyond RAM. Substituting the original RAM-count bound fails this test.
The rebuilt kernel's page roundtrip test passes in QEMU, and a private Plasma
run renders glxgears with accelerated radeonsi without the previous AMD
allocation or glamor out-of-memory errors.

mmap(length=0) still returns EINVAL, as required. Mapping error diagnostics now
include the descriptor pathname and an explicit zero-length reason, so a future
recurrence can identify its exact file rather than only a transient fd number.
The observed warning was not reproduced in the patched rendering run; this
checks the allocation failure fix without asserting that every empty-file mmap
probe is itself a GPU fault.


Final PFN-bound validation completed approximately three minutes of continuous
Plasma/glxgears rendering on a fresh private disk copy. glxinfo reports AMD
radeonsi and Accelerated: yes. The saved session and Xorg logs contain no
amdgpu_bo_alloc failure, GL_OUT_OF_MEMORY or context-creation failure; the serial
log contains no zero-length mmap warning or GPU reset. Evidence:
`build/logs/prime-pfn-fixed-glxinfo.log`, `prime-pfn-fixed-gears.log`,
`prime-pfn-fixed-session.log`, `prime-pfn-fixed-Xorg.log`,
`prime-pfn-fixed-serial.log`, and `prime-pfn-fixed-rendering.png`.
The runner ended the private VM (status 0). The kernel extracted from the main
avoryos-x86_64.iso matches the stripped current kernel byte for byte; disk.img
was not replaced with the private diagnostic image.

### Black root during SDDM PRIME startup

This launcher workaround was subsequently removed in favor of the native AMD
allocation fix below; the original distribution launcher is restored in both
disk.img and the staging rootfs.

SDDM appends `-background none` when launching Xorg, leaving the root unpainted
before the greeter draws. The PRIME Xorg launcher now removes that argument pair
and adds `-br` to paint a black root before the output handoff. It preserves the
distribution's Xorg.wrap, authentication and displayfd arguments; non-PRIME
boots retain their original arguments. Both image build paths install
`config/xorg/Xorg.sh`, and the same launcher was written into the stopped main
disk.img and read back byte for byte. The original launcher is backed up at
`build/logs/prime-black-root-original-Xorg`.

The private-image test initialized amdgpu and Xorg glamor with radeonsi. Captured
startup frames show the console transitioning to black, then the SDDM greeter,
with no random pixels in the sampled frames. Half-second sampling cannot rule
out a shorter flash. Evidence: `build/logs/prime-black-root-frame-*.ppm`,
`prime-black-root-greeter.png`, `prime-black-root-sddm.log`,
`prime-black-root-Xorg.log` and `prime-black-root-repro.log`. The VM shut down
successfully. `scripts/tests/test_xorg_startup_background.py` passes argument
filtering, native passthrough and wrapper fallback checks.

### Native initialization of newly allocated AMD VRAM

Ordinary userspace AMD GEM creation can allocate VRAM without requesting
AMDGPU_GEM_CREATE_VRAM_CLEARED. With an unpainted Xorg root, the new buffer can
expose its previous contents through PRIME. AvoryOS now adds that flag for new
userspace allocations requesting VRAM while the driver's buffer functions are
enabled. The existing amdgpu clear operation initializes the allocation and
places its completion fence in the reservation before publishing the handle.
Existing and imported buffers are untouched; GTT initialization remains in TTM.
This adds a GPU clear to new VRAM allocations, rather than each presentation.
The policy is implemented in AvoryOS-owned
`kernel/linuxkpi/src/amdgpu_memory.c`. The kernel build gives the imported GEM
creation implementation a bridge symbol; the DRM driver's ordinary entry point
resolves to the native policy, which delegates validation and allocation to that
implementation. The imported amdgpu_gem.c is byte-identical to the pinned source
and the earlier VRAM divergence patch has been deleted. Driver imports do not
overwrite the native policy. No userspace launcher or background changes are
needed.

`scripts/tests/amdgpu_vram_zero.c` requests 2 MiB CPU-accessible VRAM buffers
without the userspace clear flag, waits for initialization, checks every byte,
fills each allocation with 0xa5 and frees it. The unpatched kernel fails on the
first allocation (offset 0 contained 0x03); the patched kernel passes all 32
iterations. Evidence is in `build/logs/prime-native-clear-baseline-zero.log`
and `prime-native-clear-zero.log`. Build the probe with the musl compiler,
`-static -O2 -Itoolchain/musl-sysroot/include
-Ibuild/alpine/rootfs/usr/include/libdrm`, then run it on the AMD render node.

A private-image boot using the original distribution Xorg launcher and SDDM's
unchanged `-background none` reached the greeter with radeonsi glamor enabled.
The sampled transition shows black followed by the greeter without random
pixels; half-second sampling cannot exclude shorter artifacts or arbitrary
reuse of already existing application buffers. Evidence:
`build/logs/prime-native-clear-Xorg.log`, `prime-native-clear-sddm.log`,
`prime-native-clear-repro.log`, `prime-native-clear-frame-*.ppm` and
`prime-native-clear-greeter.png`. The VM ended with status 0. The main kernel ISO
was rebuilt; disk.img retains its ordinary launcher, with no background filter.

The final AvoryOS-owned implementation was separately boot-tested after removing
the vendor patch. Its allocation probe again passes all 32 iterations; Xorg
reports radeonsi glamor and the SDDM greeter is visible with the original
launcher. Evidence: `build/logs/prime-owned-memory-zero.log`,
`prime-owned-memory-Xorg.log`, `prime-owned-memory-sddm.log`,
`prime-owned-memory-repro.log` and `prime-owned-memory-greeter.png`.
`scripts/tests/test_amdgpu_memory_policy.py` also verifies upstream error/handle
forwarding, caller flag preservation, the ring-disabled path and compilation
without the AMD policy. Both native and imported entry points resolve in the
linked kernel. The main ISO's extracted kernel matches the current stripped
kernel byte for byte; disk.img's launcher matches the original distribution
script. The private VM ended with status 0.

### TLB acknowledgement timeout during Plasma

The supplied panic has KWin's `munmap` holding `shootdown_lock` and waiting for
CPU 1, which runs `plasmashel:cs0` with IRQs disabled. Other CPUs wait for the
shootdown lock. Its stack scan does not establish an exact CPU 1 lock owner.
An IRQ-masked lock waiter could previously prevent an acknowledgement while
the initiator held an outer lock needed by that waiter.

Native and LinuxKPI spinlock wait loops now poll the local published shootdown
request when IRQs are masked. This uses the existing invalidation handler,
which invalidates before releasing the acknowledgement; it takes no locks and
does not enable IRQs or schedule. A delayed IPI is ignored if there is no
pending request. The initiator selects its CPU and targets after acquiring
the shootdown lock, so lock contention cannot leave it using a CPU identity
saved before migration. The fatal timeout remains enabled.

`python3 scripts/tests/test_tlb_lock_poll.py` compiles the actual lock loops
and handler, mocking only platform support and privileged instructions. It
checks native, save/restore, raw and LinuxKPI lock waits with a two-thread
owner/acknowledgement cycle, PCID and fallback paths, IRQ-state preservation,
flush-before-ack and delayed IPIs. Removing polling from the native waiter
recreates the failure as a negative control. The rebuilt kernel also boots
eight CPUs, runs the existing boot self-tests and shuts down on a private
QEMU disk without using the passed-through GPU (`build/logs/prime-tlb-smoke.log`).
This is not a full Plasma reproduction: the user's running VM retains the
GPU and was left running. The updated kernel is in `avoryos-x86_64.iso` for
the next `make run-c7-prime` boot; this change needs no disk.img edits.

### Generated test artifact cleanup

Obsolete private test disks, test ISOs, frame sequences and temporary host
runners were removed after validation. Earlier evidence paths above are
historical and may no longer exist. Final `prime-owned-memory` logs and its
greeter screenshot, plus `prime-tlb` validation logs, are retained. The live
`p6-c7-prime.log`, main disk and boot ISO are not cleanup targets.

### Rendering corruption audit (2026-10-13)

See [the rendering corruption investigation](radeonsi-corruption-investigation.md)
for confirmed driver-fault and sequence contract fixes, mode-3 bounded resource
tracing, and exact guest validation commands. `make gpu-diagnostics` packages the
new fog-free `test_gpu_corruption` pixel oracle. Host regressions and llvmpipe
controls pass; Minecraft and RadeonSI rendering remain unvalidated on the new
kernel. Build the ISO before the next user-controlled hardware boot.
