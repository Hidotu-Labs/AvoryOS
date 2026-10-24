# Plasma Wayland: Xwayland socket setup

The `build/logs/nouveau-c4.log` session matching the pasted
`xembedsniproxy` SIGABRT was launched with `dbus-run-session
startplasma-wayland`. Before the abort it reports:

```
XDG_RUNTIME_DIR "/tmp/runtime-0" can be written by others (mode 041777)
kwin_xwl: /tmp/.X11-unix does not exist. Please check your installation
kwin_xwl: Failed to establish X11 socket
```

KWin cannot establish its Xwayland socket, leaving X11 clients such as
`xembedsniproxy`, `kaccess`, and `gmenudbusmenuproxy` unable to initialize
Qt's XCB platform. The last failed access to a `glibc-hwcaps` library path
is a loader search probe; it does not establish that the library is missing.
The DRM AUTH_MAGIC EACCES from `ksplashqml` is a separate diagnostic and
does not justify bypassing DRM authentication.

`scripts/configure-accounts.sh` now adds an idempotent block to
`/sbin/avory-early-init` to create `/tmp/.X11-unix` and `/tmp/.ICE-unix`
with mode `1777`. The boot rcS invokes this hook before services start.
Creating these paths only in the disk image would not work: the kernel
mounts a fresh tmpfs over `/tmp` on every boot.

`scripts/configure-image-root.sh` now sets `XDG_RUNTIME_DIR` to mode `0700`,
matching the login program and X11 launcher. Previously, sourcing the
image's shell profile changed the login program's private directory to
`1777`, causing the D-Bus warning.

Validation:

```
python3 scripts/tests/test_desktop_runtime_dirs.py
sh -n scripts/configure-accounts.sh
bash -n scripts/configure-image-root.sh
```

The regression tests execute the actual provisioning and runtime fragments,
check fresh creation and permission repair, and ensure reprovisioning keeps
existing early-init hooks without duplicating the socket setup.
A guest boot has not yet verified the full Plasma session. Other diagnostics
in the log, including GPU capability failures and the missing FUSE device,
remain separate issues.

Rebuild the image with `make disk.img` to persist the fix. For an existing
guest, exit the failed session and run these as root before trying again:

```
mkdir -p /tmp/.X11-unix /tmp/.ICE-unix
chmod 1777 /tmp/.X11-unix /tmp/.ICE-unix
export XDG_RUNTIME_DIR="/tmp/runtime-$(id -u)"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"
dbus-run-session -- startplasma-wayland
```

These commands repair the current boot only; the old shell profile can
change the runtime permissions again until the image is updated.

## Desktop remains on the terminal: saved output layout (2026-10-24)

The later report reaches `kwin_wayland`, Xwayland, and `plasmashell` without
switching the visible display away from the terminal. The affected disk's
`/.config/kwinoutputconfig.json` disables `HDMI-A-1` and enables only
`Virtual-1`, the headless VKMS output. Plasma is running on that invisible
output; the D-Bus/systemd, FUSE, and DRM authentication warnings do not
explain this particular symptom.

A private partition copy reproduced the terminal-only result with the
current kernel and RTX 4060 Ti passthrough. Enabling the existing `HDMI-A-1`
entry and disabling `Virtual-1`, without changing the kernel, reached the
Plasma splash and then the complete desktop (wallpaper, panel, and widgets).
Evidence is saved under `build/plasma-repro/`, including
`baseline-nvidia.png` and `repaired-nvidia.png`.

The repair keeps the saved modes and other display settings. The old layout
is backed up as
`/.config/kwinoutputconfig.json.before-display-repair-20261024` in `disk.img`.
Only the repaired configuration and its backup are installed into the main
disk; the private VM's other disk changes are excluded. No kernel or ISO
rebuild is required for this configuration repair.

For future images, `scripts/configure-accounts.sh` removes the cached
`kwinoutputconfig.json` while provisioning the root (`/` and `/root`), Avory,
and `/etc/skel` homes. `scripts/configure-image-root.sh` also removes those
layouts from the finished filesystem after the rootfs overlay. Later guest
logins retain their saved layout. Clearing this cache alone was insufficient:
an unrestricted fresh startup could still select the headless VKMS output.
Both scripts are already full disk-build dependencies in `GNUmakefile`.
The provisioning fragment and actual image finalizer were checked twice
against a small ext4 fixture: all four stale layouts disappeared, while
their `kwinrc` settings survived.

The disk also needs `avory-drm.sh` in
`/etc/xdg/plasma-workspace/env/`. The original copy in `/etc/profile.d/`
was not sourced by this Plasma launch path. `configure-accounts.sh` and the
final disk population recipe now install it into Plasma's environment
directory. Plasma loads it before starting KWin; `drm-pick.sh` selects the
connected production display instead of VKMS and preserves an explicit
`KWIN_DRM_DEVICES` override. Both the software-only and NVIDIA passthrough
guests reached the complete desktop with this hook and no saved output
layout. A private autostart probe also opened `glxgears` during the NVIDIA
run. Screenshots are saved as `build/plasma-repro/fresh-env-software.png`
and `build/plasma-repro/fresh-env-nvidia.png`.

For an older affected guest, exit the session, load the installed display
selector, and back up its layout before restarting Plasma. For root's normal
`HOME=/` session:

```sh
mv /.config/kwinoutputconfig.json /.config/kwinoutputconfig.json.disabled-layout
. /etc/profile.d/avory-drm.sh
dbus-run-session -- startplasma-wayland
```

The installed session hook makes the selector automatic for future launches.
Removing the saved layout resets it, whereas the earlier disk repair changed
only the two outputs' enabled state and kept the existing visible-output mode.

The older unfiltered software-only diagnostic encountered a Mesa Vulkan
device-selection layer crash, and the NVIDIA baseline hit a kernel fault
during the test's forced shutdown. The new validation covers desktop startup;
this investigation does not establish that shutdown is fixed.

## Nouveau and AMDGPU rendering in Wayland

The initial startup hook restricted `KWIN_DRM_DEVICES` to the display card.
`drm-pick.sh --wayland` now returns the connected display, selected render
node, render driver, and an ordered list of production DRM cards from one
identity inventory. The hardware renderer's card comes first, followed by
the visible display and other Nouveau/AMDGPU/native cards. VKMS and VGEM
are excluded unless explicitly selected as the display.

The shared session hook exports that list and `KWIN_RENDER_NODES`, while
preserving explicit overrides (including an explicitly empty render-node
list). For Nouveau it defaults `NOUVEAU_USE_ZINK=1`, enabling OpenGL through
NVK/Zink. AMDGPU uses its Mesa driver without the Nouveau setting. A native
display with no hardware GPU keeps the software fallback.

The NVIDIA passthrough test started with no saved layout. KWin's own
`supportInformation` reported OpenGL ES 3.2 via EGL with renderer
`zink Vulkan 1.4(NVIDIA GeForce RTX 4060 Ti (NVK AD106) (MESA_NVK))`.
`glxgears -info` reported the same hardware renderer and about 59 FPS.
The desktop and gears are captured in `build/wayland-gpu-repro/latest.png`;
KWin's report is in `build/wayland-gpu-repro/kwin-info.txt`.

`scripts/tests/test_drm_device_selection.py` covers Nouveau, AMDGPU,
headless hardware renderers with native display sinks, both drivers,
reordered card/render minors, explicit overrides, and software fallback.
AMDGPU was checked with these topology fixtures; this run did not pass the
host's AMD display GPU through to a guest for hardware verification.


## DRM timeline notifications

DRM syncobj eventfd notifications use native eventfds instead of the
imported `CONFIG_EVENTFD`-disabled stubs. A pending notification retains the
native vnode, so closing or reusing the original descriptor cannot redirect
the signal or free its counter. Fence callbacks increment that counter and
wake native poll/epoll waiters. Kernel counter overflow saturates and reports
`POLLERR` rather than wrapping to zero.

The NVIDIA guest passed 35 API checks covering invalid descriptors,
submitted and signaled timeline points, already-completed points, descriptor
reuse, poll wakeup, and overflow.

## CRTC timing and virtual-display performance

Native legacy KMS retains the configured CRTC mode and viewport for
`GETCRTC`, and computes software vblank frequency from the pixel clock,
horizontal/vertical totals, interlace, doublescan, and vscan. The UAPI
`vrefresh` convenience value may be zero or stale. Atomic `MODE_ID` and
`ACTIVE` now apply the same timing; these properties previously updated
only their stored values. Test-only commits leave the live mode unchanged,
and missing or incorrectly sized mode blobs are rejected.

The rebuilt guest passed 91 mode/timing checks, including measured vblank
rates from 30 through 165 Hz, atomic commits in both property orders,
activation/deactivation, invalid blobs, and test-only commits. At 165 Hz,
visible glxgears improved from approximately 125–136 FPS to 409–439 FPS.

`make run-nouveau` defaults `NOUVEAU_VIRTIO_EDID=off`, selecting the 60 Hz
virtual display-info mode. This reduces compositor work on the CPU-copy
scanout path while client presentation remains uncapped. Use
`make run-nouveau NOUVEAU_VIRTIO_EDID=on` to restore EDID mode enumeration.
The launcher regression checks both settings. Other VFIO targets keep their
existing default.

With the rebuilt kernel and saved desktop configuration retained in a
private copy of the restored disk, the visible GTK run measured
4,573–5,352 FPS across fourteen five-second samples (about 4,921 FPS
on average, 70 seconds total) using:

```sh
vblank_mode=0 MESA_VK_WSI_PRESENT_MODE=immediate glxgears
```

`glxinfo -B` reported NVIDIA GeForce RTX 4060 Ti through NVK/Zink.
The 35 notification checks and 91 mode checks passed in the same guest.
Logs and private probe sources are under `build/nvidia-fps/`.
