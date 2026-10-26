# SDDM hides Wayland sessions: ramfs mount permissions

SDDM's session menu listed IceWM Session, IceWM and Xfce Session, although
`/usr/share/wayland-sessions` contained Plasma, Weston and XFCE entries and
`startplasma-wayland` was executable.

The kernel's `ramfs_mount_at()` created a fresh root vnode for `/dev` and
`/run` without setting its permission mask. The resulting `/dev` had mode
0000. Root could traverse it, but the SDDM account's `stat`, `statx` and
`access(F_OK)` calls for `/dev/dri` failed with ENOENT. An `ls -ld` check
was misleading: the separate symlink-metadata lookup could still display
the child directory's 0755 mode.

SDDM checks `QFileInfo::exists("/dev/dri")` before populating Wayland
sessions. This failed lookup suppressed the whole Wayland list.

`ramfs_mount_at()` now gives new ramfs roots mode 0755. This permits listing
and traversal while keeping writes restricted to the owner. It changes the
new mount root, without altering the covered filesystem's directory.

Validation:

- `python3 scripts/tests/test_ramfs_mount_permissions.py` checks both existing
  mountpoints and the fallback that creates a missing mountpoint.
- In a private QEMU overlay, the SDDM account's stat/statx/access probes
  failed before repair and all succeeded after `chmod 755 /dev /run`.

The permanent fix is in the kernel, so an updated boot ISO is required.

## Login handoff and XFCE compositor

The console also lacked `TIOCSCTTY` (0x540e). SDDM's Wayland child calls
`setsid()` and then takes control of its VT before executing the session.
Returning ENOTTY made that child exit and returned the user to the greeter.
The console now records the controlling terminal for a session leader and
supports detaching it. `test_vt_queries.py` covers attachment, repeated
attachment, rejection of a different terminal/non-leader, and detachment.

After that repair, the saved selection was XFCE Wayland. Its launcher exits
when `labwc` is missing; the package list previously installed `xfce4-session`
without its default Wayland compositor. The message is written to stdout,
which SDDM discards, explaining the empty stderr session log. `labwc` is now
in `scripts/rootfs-packages.txt` and installed with libsfdo, wlroots0.20 and
xcb-util-errors. Plasma Wayland independently reached the complete desktop
through SDDM in the private software-rendering VM.

The existing `disk.img` was backed up to
`build/sddm-session-debug/disk-before-labwc.img` before installing the four
packages and their Axon local database records. The private filesystem
copy passed an ext4 check after reconnecting one pre-existing orphaned
inode to lost+found. Package binary/library hashes matched the staged tree.

`seatd` startup lives directly in the versioned `config/init/rcS`, after
udev coldplug. Rootfs setup and disk population install that script into
`/etc/init.d/rcS`. The account provisioning script does not start seatd.
The existing DRM profile sets `WLR_DRM_DEVICES` to the production-card list
(excluding headless VKMS), and permits software rendering for virtual GPUs.
Explicit wlroots settings remain respected. Those settings, with seatd,
reached XFCE's visible desktop in the private VM.

The image's custom `/etc/profile` had replaced the distro's profile.d
loader. Its generated Wayland login fragment now explicitly sources the
DRM environment hook. This is required for SDDM's login-shell wrapper to
receive the wlroots card selection and software-rendering allowance.
`test_desktop_runtime_dirs.py` checks that the hook loads for Wayland,
and `test_drm_device_selection.py` checks wlroots defaults and overrides.

Final verification: a fresh private overlay of the updated `disk.img` logged
`Starting seat manager ... [ ok ]` at boot. Selecting XFCE Wayland in SDDM
reached the visible desktop without manually starting seatd or setting GPU
variables. Screenshot: `build/sddm-session-debug/xfce-wayland-sddm-final.png`.

## Rollback after mouse regression report

The new WLR_DRM_DEVICES/WLR_RENDERER_ALLOW_SOFTWARE exports and explicit
Wayland profile loading were reverted after mouse input regressed across
Weston, XFCE and Plasma. The earlier desktop screenshots describe the
pre-rollback configuration. Seatd still starts directly in rcS; no seatd
startup remains in configure-accounts.sh. Input-driver code was not changed.
The rollback updates source and the staged rootfs, not the running disk image.
