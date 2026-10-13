# Mocktail after a forced shutdown

The supplied fault is in Roblox's exception-reporting path: code reads the
vtable of a zero-initialized stream and then dereferences `NULL - 24`. The
stack and the `RBXCRASH: UnhandledException` text show a secondary failure
while reporting an exception. The original exception and the particular
damaged file are not present in the supplied log. The preceding NETLINK
`setsockopt(..., 270, 11, ...) = -ENOPROTOOPT` is not evidence of its cause.

Closing QEMU or cutting power can interrupt database/cache writes. The old
Avory launcher only tried removing stale endpoints and arbitrary `*.lock`
files. Its `pgrep -f mocktail` check could match the launcher itself, skipping
that cleanup; it did not track whether the preceding client exited cleanly.

The launcher now supervises AppRun through `scripts/mocktail-session.sh`:

- An atomic directory in `/tmp` excludes concurrent launches. Avory mounts
  `/tmp` as tmpfs. The wrapper also checks executable paths for a client
  started by the old launcher. It does not rely on the kernel's stub `flock`.
- `.avory-session-clean` under `MOCKTAIL_DATA_ROOT` exists only after AppRun
  exits successfully. A crash, killed launcher, or reboot during the run
  leaves the next launch eligible for recovery. The first launch after
  installation also recovers because older runs have no clean marker.
- Recovery renames `android/data/cache`, the appData `rbx-storage` asset
  directory, and appData `rbx-storage.db` with its journal/WAL/SHM sidecars
  to unique sibling names ending in `.avory-recovery.XXXXXX`. Directory
  renames stay within the same parent because Avory's ext2/ext4 backend
  rejects cross-parent directory moves. A unique directory under
  `MOCKTAIL_DATA_ROOT/avory-recovery` contains symlinks indexing the backups.
  These caches can be rebuilt; backups are retained. It creates a fresh
  Android cache directory and removes the stale external-launch endpoint.
- The separate `$MOCKTAIL_CACHE_ROOT/android` runtime cache is also backed
  up and recreated. The launcher explicitly prepares XDG config/state/data
  parents and the cache root before starting AppRun. Root sessions with
  `HOME=/` default to `/.cache/mocktail` rather than `//.cache/mocktail`.
- LocalStorage, secure storage (`rbx-storage-sc`), storage identity,
  authentication, configuration, payloads, and logs are preserved. It does
  not recursively delete lock files from persistent data.
- Arguments, stdin, and exit status pass through. TERM/HUP/INT are forwarded
  to the child. A nonzero exit keeps the next run eligible for recovery.

To request recovery explicitly, launch `MOCKTAIL_RECOVER=1 mocktail` in the
guest. The helper ships inside each runtime as `avory-session.sh`, installed
by `scripts/setup-mocktail.sh`. Both the primary and versioned launchers use
it. Existing guest launchers need to be updated before this behavior applies.

Validation: `python3 scripts/tests/test_mocktail_session.py` exercises a
forced kill/relaunch, clean restarts, exit status and argument forwarding,
signal handling, concurrent launches, explicit recovery, and preservation of
userdata. The tests reject cross-parent directory renames to model the guest
filesystem and inject rename failures to verify that originals survive and
the app does not start after failed recovery. Shell syntax is checked with
`sh -n` and `bash -n`.

This is recovery for interrupted cache writes, not confirmation of the
original exception's cause. A forced-stop/boot/relaunch with the updated
launcher and actual Roblox payload still needs a guest test. If it fails,
retain the recovered caches and the Roblox logs from the failed run to
identify the original exception. It cannot repair filesystem metadata or
payload binaries damaged by power loss.

## Joining with the current audio runtime

The user reproduced an indefinitely frozen Join with the normal audio
backend, then successfully entered a game using
`SDL_AUDIO_DRIVER=dummy mocktail`. The captured SDL/PipeWire audio setup
mutex wait makes audio a confirmed contributor to this reproduction; the
exact underlying deadlock remains unresolved.

The generated launcher therefore defaults to SDL3's `dummy` audio backend
as a temporary workaround. Games have no sound with this backend. An
explicit `SDL_AUDIO_DRIVER` takes priority, and the legacy
`SDL_AUDIODRIVER` is translated if no SDL3 selection is present.
The user also confirmed joining with `SDL_AUDIO_DRIVER=pulseaudio mocktail`,
but reported no game sound. Direct ALSA playback with
`SDL_AUDIO_DRIVER=alsa SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE=hw:0,0 mocktail`
has not been tested. Audible game playback remains unresolved.

The setup script and staged rootfs launchers carry this default. Existing
guest installations need the updated launcher, or can continue using
`SDL_AUDIO_DRIVER=dummy mocktail` without rebuilding. Updating a boot ISO
alone does not replace the launcher stored on the guest disk.
