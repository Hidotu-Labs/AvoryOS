# Mouse events lost between SDDM and Wayland sessions

The evdev mouse and keyboard devices had one shared read cursor. Independent
opens of `/dev/input/eventN` consumed each other's events. When Xorg remains
open during a Wayland login, it can drain events before the compositor sees
them. GPU environment changes do not repair that input queue behavior.

The original kernel reproduced this in a private QEMU guest: two independent
nonblocking opens received one injected USB mouse report; reader A obtained
144 bytes, while reader B returned EAGAIN. The host regression also fails
against the original source immediately after the first reader drains input.

Each evdev open now has an independent cursor, poll/epoll readiness and
SYN_DROPPED reporting on overflow. Duplicated descriptors still share their
open file. New opens start at the current event position. Final close removes
the client under the device lock, which also protects notification lifetime.
The open syscall retains temporary ownership until descriptor installation
so EMFILE and other errors can release an uninstalled evdev client.

Checks:
- `scripts/tests/test_evdev_readers.c`: independent readers, readiness,
  new-open behavior, slow-reader overflow and close lifetime.
- `scripts/tests/test_usb_tablet.c`: real HID/mouse event construction,
  relative input, absolute axes, button transitions and wheel events.
- Guest reproduction logs under `build/mouse-input-repro/`.

The fixed-kernel guest comparison delivered 72 bytes to each independent
reader from the same injected report. AddressSanitizer and UBSan passed the
host client-lifetime regression. Plasma Wayland mouse login, Welcome Center
button clicks, application launcher, and logout were verified through SDDM.
The project GPU environment/profile rollback remains in place; VM-specific
GPU settings used for compositor tests are confined to disposable overlays.

Desktop interaction checks with the fixed kernel:
- Plasma: SDDM login, dismiss Welcome Center, open the application launcher,
  and log out through the mouse-operated confirmation dialog.
- XFCE: clean SDDM login, open the Applications menu and directory menu.
- Weston: SDDM login, launch its terminal, drag the window, and wheel-scroll
  terminal history (showing lines 38–61 after generating lines 1–100).
Screenshots and serial logs are retained in `build/mouse-input-repro/`.

The relative USB mouse guest test also delivered 72 bytes to each reader.
The rebuilt `avoryos-x86_64.iso` contains the kernel fix. These tests use
private overlays; `disk.img` was not modified by this mouse investigation.
