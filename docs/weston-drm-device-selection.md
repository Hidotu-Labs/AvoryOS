# Weston and drm-identify EACCES diagnostics

`drm-identify` issued DRM_IOCTL_SET_VERSION (0xc0106407), which is
master-only on imported DRM devices. Identity scanning should not mutate
DRM device state or require ownership. The helper now uses read-only
GET_UNIQUE and falls back to the card's sysfs PCI identity, as it already
did for render nodes. Pairing still matches the hardware identity rather
than relying on minor numbers.

Weston's DRM_IOCTL_MODE_ATOMIC (0xc03864bc) denial was reproduced with its
default device heuristics: it selected VKMS and enabled the invisible
Virtual-1 output. Passing the connected native card reached the visible
Weston desktop. Its SDDM desktop entry now invokes avory-weston-session,
which uses drm-pick.sh --display to pass the correct --drm-device=cardN.
Explicit backend/device options are preserved. DRM master checks remain
unchanged.

Validation:
- scripts/tests/test_drm_identity_readonly.py
- scripts/tests/test_drm_device_selection.py
- scripts/tests/test_weston_session.py
- make -C userland drm_identify.elf
- Private QEMU reproduction of the original EACCES and visible Weston
  desktop on the connected display; screenshot under
  build/sddm-session-debug/weston-connected-card.png.
