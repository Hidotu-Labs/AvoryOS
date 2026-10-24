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

## Second abort trigger: disallowed sandbox path (2026-10-17)

A later run aborted the same way for a different reason:

```
** (epiphany:985): CRITICAL **: Attempted to add disallowed path to sandbox: //.cache/epiphany
```

Root cause, confirmed against the installed image:

- Epiphany 51.1 (`libephymisc.so`, upstream `ephy_cache_dir()`) builds the
  cache directory with `g_build_filename(g_get_user_cache_dir(), "epiphany")`.
- That session had no `XDG_CACHE_HOME`. `/etc/xdg/xfce4/xinitrc` fills the
  unset variable with a naive join, `XDG_CACHE_HOME=$HOME/.cache`. Root's
  home is `/` by design (`/etc/passwd`, `/etc/profile`), so the join produces
  `//.cache` and the browser's cache dir becomes `//.cache/epiphany`.
- WebKit's `webkit_web_context_add_path_to_sandbox()` checks the raw path:
  `pathIsBlocked()` splits it without canonicalising, and the empty first
  element of `//.cache/...` matches the `""` blocked prefix (the entry that
  rejects UNC-style `//` roots). It logs `g_critical`.
- `libephymisc.so` imports `g_log_set_always_fatal`, and Epiphany installs
  `ephy_debug_set_fatal_criticals()` unless `G_DEBUG` is set, so the critical
  aborts the process via glib's `_g_log_abort` → `tgkill(SIGABRT)`.

The `start-xfce4-wayland` wrapper written by `scripts/setup-arch.sh` had the
same naive join for `XDG_CONFIG_HOME` and `XDG_DATA_HOME`; the observed
critical was the cache path because that is the one the packaged `xinitrc`
derives. The private Epiphany probe harness cannot hit any of this because it
exports `XDG_CACHE_HOME=/tmp/epiphany-clean-cache` before launching the
browser.

Fix — every session entry point now derives the XDG defaults through
`"${HOME%/}"`, so `HOME=/` cannot produce `//`-prefixed paths:

- `scripts/setup-arch.sh` rewrites the `start-xfce4-wayland` wrapper
  unconditionally (incremental rootfs trees pick the fix up) and seds the
  packaged `/etc/xdg/xfce4/xinitrc` fallbacks.
- `AetherDE/scripts/sax11.sh` pins
  `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME` and `XDG_STATE_HOME`
  before starting a session.
- `GNUmakefile` writes the same block into `/etc/profile` and
  `/etc/bash.bashrc`, so login shells never hand unset XDG variables to
  session launchers.
- `userland/avory-installer.c` writes the block into the generated
  `/root/.xinitrc` and the created user's `.xinitrc`.

The recorded session agrees with this chain: `/var/lib/sddm/state.conf` names
`/usr/share/xsessions/xfce.desktop` for root. The Plasma entry point is safe
independently: `plasmax11.desktop` runs the `/usr/bin/startplasma-x11` wrapper
from `scripts/configure-accounts.sh`, which already switches to canonical
`/.config`, `/.cache`, `/.local/share` and `/.local/state` when `HOME` is `/`;
and SDDM's `Xsession` sources `/etc/profile` before any session starts. GLib
joins `/` canonically (`g_build_filename("/", ".cache")` is `/.cache`), so only
a shell-level naive join such as the xinitrc fallback can produce the `//`
prefix that WebKit rejects.

`G_DEBUG` remains a documented escape hatch: any value (even empty) makes
Epiphany skip `g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL)`. It is not
exported globally because it would also mask unrelated criticals that are
worth aborting on.

Validation:

```
python3 scripts/tests/test_session_xdg_env.py
```

The test executes the real fragments: the extracted `start-xfce4-wayland`
wrapper runs with `HOME=/` (asserting canonical values and no `//`), the real
sed expressions are applied to a fixture of the upstream `xinitrc` fallbacks,
the Plasma launcher's root branch from `scripts/configure-accounts.sh` runs
for an empty `HOME`, `HOME=/` and `HOME=/root`, and the `/etc/profile`
pinning block is executed with `HOME=/`. It also keeps a model of the
unpatched fallback so the `//.config`/`//.cache` derivation stays
reproducible.

The kernel-side artefacts in the same log are responses to the abort, not
causes:

- `[ USER FAULT ] ... signal 6 (SIGABRT)` is the kernel reporting the
  self-delivered abort, with coherent VMA and register data.
- `[JBD2] owner tid=1014 died holding the transaction, aborting
  (depth=2 blocks=1 phase=write-mapping)` is `kernel/src/fs/ext3/ext3.c`
  discarding the dead task's in-flight journal write set (`aborted-owner-died`)
  so no torn metadata reaches the disk. The interrupted file write is lost;
  the mount is not left wedged.

The earlier console workaround for an already running session still works:

```
env -u PULSE_SERVER PULSE_RUNTIME_PATH=/run/pipewire/pulse XDG_CACHE_HOME=/.cache XDG_CONFIG_HOME=/.config epiphany
```

The staged rootfs was refreshed with `make setup-arch`. The installed
`disk.img` was patched in place while QEMU was stopped and its filesystem
reported clean with no journal recovery pending: `/etc/profile`,
`/etc/bash.bashrc`, `/.bashrc`, `/etc/xdg/xfce4/xinitrc`,
`/usr/bin/start-xfce4-wayland`, `/bin/sax11.sh` and the `/AetherDE` source
copy. Read-back matches the
patched sources byte for byte; the pre-patch files are kept in
`build/epiphany-abort/disk-pre-xdg-fix/`. Fresh images pick the fix up from
the sources. The direct-boot `initrd/startw.sh`/`initrd/startx.sh` scripts and
their disabled `wayland.service`/`x11.service` files were retired
afterwards, so fresh images no longer contain them. The live `disk.img` was
later rebuilt from the sources and verified offline to match (the
pre-rebuild image is kept at `build/epiphany-abort/disk-pre-reformat.img`).
The patched image was
booted once with the fixed kernel as a
smoke test: the filesystem mounts and the generated session bashrc runs (the
`Root@AvoryOS:/#` prompt comes from it). The guest console only carries
kernel output, so in-guest XDG values are verified by the host test above.

No kernel rebuild is needed for this session change; the plain
`avoryos-x86_64.iso` does not embed the rootfs, and the kernel frames in the
log are the SIGABRT report and the ext3 dead-owner journal discard described
above.
