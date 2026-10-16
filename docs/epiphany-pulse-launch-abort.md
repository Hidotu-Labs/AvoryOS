# Epiphany launch abort with WebKitGTK 2.54

The supplied October 16 log shows Epiphany deliberately raising SIGABRT while
WebKit prepares its subprocess sandbox. It is separate from the missing
`sched_setattr` warning emitted by the PressureMonitor thread. That interface
is now implemented separately; see [scheduler attributes](sched-attr.md).

The installed `/usr/lib/libwebkitgtk-6.0.so.4.19.4` was read from the disk image
without modifying it. Its executable LOAD segment has virtual addresses 0x1000
above file offsets. The caller at file offset 0x16fadcf therefore corresponds to
virtual address 0x16fbdcf. The preceding call invokes the
assertion helper with line 60 of `CStringView.h`. Its incoming branch at
0x16f7ae2 checks for a NUL terminator after stripping a `unix:` prefix.

This matches upstream WebKitGTK 2.54.1's
[bindPulse implementation](https://github.com/WebKit/WebKit/blob/webkitgtk-2.54.1/Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp):
`CStringView::fromUTF8` receives a subspan of `span()`, which excludes the
terminator. Any nonempty pathname in `PULSE_SERVER=unix:…` hits the assertion.
AvoryOS's root Plasma launcher exported exactly that form.

The launcher now exports `PULSE_RUNTIME_PATH=/run/pipewire/pulse`. Libpulse
selects the same `native` socket using this supported runtime-directory setting.
The desktop's XDG runtime directory stays separate. A re-entered launcher clears
the old exact default PULSE_SERVER value while preserving custom server choices;
non-root routing is unchanged. Custom unix: PULSE_SERVER settings can still
trigger the upstream WebKit bug.

Validation:

```
bash -n scripts/configure-accounts.sh
python3 scripts/tests/test_pipewire_startup.py
python3 scripts/tests/test_pulse_runtime_routing.py
```

The latter test executes the actual launcher fragment and makes real libpulse
connect and send authentication to a private UNIX listener. It uses no host audio
service or device. The former also covers old-environment migration, custom
server preservation, non-root behavior, and bridge readiness.

For an already running root desktop, launch from its terminal:

```
env -u PULSE_SERVER PULSE_RUNTIME_PATH=/run/pipewire/pulse epiphany
```

The staged rootfs launcher and `disk.img` launcher are updated. The disk patch
was installed after QEMU stopped and its filesystem reported clean with no
journal recovery pending. Only the root audio-routing block was replaced in
the existing launcher; its read-back content matches the patch exactly.
A full image backup is `build/epiphany-abort/disk-before-launcher.img`.
The image build already tracks `scripts/configure-accounts.sh`.
No kernel rebuild is needed for this launcher change. A complete GUI launch
has not been verified here; further subprocess or namespace failures need
their own log. The separate scheduler syscall change is in the rebuilt ISO.
