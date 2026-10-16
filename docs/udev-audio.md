# Real udev and PipeWire device discovery

2026-10-15

The initial socket-routing fix let root's Plasma session connect to the boot
PipeWire instance. It still had no real output because the disk had libudev
but no udev daemon or standard sound rules. The old kernel also treated sysfs
uevent writes as edits to the attribute contents, and gave the sound card
itself the same device number as its control node.

The image now includes the upstream systemd udev daemon, udevadm, its shared
runtime, helpers, hardware database and rules. BusyBox remains PID 1. rcS starts
udevd, triggers subsystem/device coldplug and waits for the queue before audio.
The early-init script and kernel no longer manufacture the udev database.

Kernel support now preserves uevent attribute contents while emitting events
for writes, including synthetic UUIDs and sequence numbers. Sound control/PCM
nodes are children of card0 under /sys/devices/virtual/sound. Netlink supports
unique port IDs and delivery of processed udev messages with sender credentials;
binding no longer injects a fixed list of fake devices. rt_sigqueueinfo uses the
existing queued-signal machinery so udev can dispatch work to its workers.

## Verification

```
python3 scripts/tests/test_sysfs_uevent.py
python3 scripts/tests/test_rt_sigqueueinfo.py
python3 scripts/tests/test_pipewire_startup.py
python3 scripts/tests/test_inotify_boot.py
python3 scripts/tests/test_udev_audio_boot.py
```

The private KVM test uses the built kernel and staged Arch runtime without
opening disk.img, assigning VFIO or connecting to host audio. It verifies full
coldplug and settle success, SOUND_INITIALIZED created by the standard rules,
AvoryOS Audio Stereo as the output, and two-second PCM playback completion.
The first two tests compile actual kernel functions with ASan/UBSan.

The initial disk-clone check verified device discovery and playback but missed
failed boot statuses. Subsequent user logs exposed a missing hostname executable
and a udev settle timeout. rcS now invokes BusyBox hostname directly and reports
daemon startup, enumeration and queue settling separately.

Inotify previously registered watches without producing or delivering events.
The kernel now queues namespace events (create, delete, rename and watch removal),
with record-sized reads, poll/epoll wakeups, overflow reporting and retained watch
references. This makes deletion of udev’s queue file wake a waiting udevadm settle.
Access, modify, attribute and open/close event production remains unimplemented.
The dedicated inotify guest checks ext2/tmpfs events, rename cookies, asynchronous
delete wakeups, small buffers, overflow and one-shot watches.

The updated integration test executes the actual rcS and rejects failed startup
statuses. It checks hostname, forces a real queue-deletion wait, verifies the ALSA
sink, and completes two-second playback. A separate private clone of the real disk
also exercises the compact installed rcS with the same assertions. Logs and the installation manifest are
under build/udev-install. The disk and ISO were installed after QEMU stopped;
backups are build/udev-install/disk-before-udev.img and
build/udev-install/iso-before-udev.iso.

The existing filesystem has inode/extent and allocation-bitmap problems.
Allocation summary counters were reconciled for installing the new files;
pre-existing inode/data-tree diagnostics are unchanged. No broad filesystem
repair was performed. Audio tests used QEMU's silent backend and verify data
flow/completion, not audible output on the user's speakers. Existing unrelated
LinuxKPI boot tests sometimes report failures; the dedicated udev/audio checks
passed. Native drivers still need to emit events for any future hotplug support.

The hostname/inotify follow-up is installed, with backups and test logs under
`build/udev-inotify-fix`. It changes only the existing rcS bytes in the disk
(no allocation or inode metadata changes) and replaces the ISO kernel.

## SDDM rename regression

The first namespace-notification implementation pinned the source and destination
for tmpfs cross-directory moves after invalidating their cache entries. Looking
up an absent destination inserted a new negative dentry that survived the move.
Qt publishes SDDM's socket by binding inside a private /tmp directory, applying
permissions and renaming it into /tmp. Its subsequent chown returned ENOENT, so
SDDM stopped before spawning the greeter.

The tmpfs path now pins targets before invalidation and invalidates both names
after the backend rename, matching the ext2 path. The private inotify regression
now reproduces Qt's bind/chmod/cross-directory rename/chown/rmdir/connect sequence
and deliberately looks up the absent destination first. The old kernel fails
lookup, ownership and connect; test logs are under build/sddm-udev-debug.

The corrected kernel passed the socket regression and the private disk-clone
SDDM run: "Greeter session started successfully" and "Message received from
greeter: Connect" were both observed. Udev settling and PipeWire playback also
passed again. The corrected normal boot ISO was installed while QEMU was stopped;
the previous ISO is build/sddm-udev-debug/iso-before-sddm-fix.iso. This follow-up
did not modify disk.img. The SDDM test used VirtIO graphics, without AMD VFIO.
