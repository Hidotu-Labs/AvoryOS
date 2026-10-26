# VLC and Firefox playback stalled at 00:00

The installed disk was inspected read-only, directly from its ext4 partition
at offset 1048576. No installed disk or boot ISO was changed by these tests.

## Installed failure

`/nohup.out` records WirePlumber exiting because its Lua policy module cannot
load `liblua.so.5.5`. That library is absent from the installed filesystem.
The module's bytes match the staged module used in the private reproduction.
The log also records Firefox PulseAudio stream-creation timeouts.

The installed disk still contains the earlier manual PipeWire output in
`/etc/pipewire/pipewire.conf.d/20-avoryos-hda.conf`, named
`AvoryOS HDA Output`, and the ACP-disabling WirePlumber override. Seeing that
output in Plasma does not establish that WirePlumber is running and linking
application streams. The manual output remains after WirePlumber exits.

## Same-kernel comparison

All VM tests use private root and boot images, virtual HDA, and QEMU's WAV
capture backend. They do not open the installed `disk.img`, use host audio,
or attach VFIO hardware.

The VLC test loads the actual staged `libvlc.so.5` and its filesystem, AVI,
raw video/audio, PulseAudio, and dummy video-output plugins. It plays a local
four-second AVI containing raw video and stereo PCM, observing VLC's state
and media time. It exercises the media engine rather than the Qt GUI or GPU
rendering. Firefox itself was not executed in the private fixture.

| Audio runtime with the same kernel | Observed result |
| --- | --- |
| Required Lua 5.5 library present; normal ALSA profiles | Real `AvoryOS Audio Stereo` sink; Pulse playback completes; VLC advances through 1051, 2100, 3231, and 4025 ms, then reaches Ended |
| Installed missing Lua library and installed legacy audio overrides | VLC reports Playing but stays at 0 ms throughout a 20-second observation |

The successful fixture's Lua library came from the cached Arch package
`lua-5.5.1-1-x86_64.pkg.tar.zst`. Its SHA-256 matches the staged repository
metadata: `2cd360ab4590950b40adae136dd45a22a50b0621c67db5691e066751f6f2416f`.
It was supplied only to the private fixture; it is not installed into the
product disk or staged rootfs by this investigation.

The captured VLC tone spans 3.985 seconds, with zero internal silent gaps
over 1 ms and zero discontinuities. The separate two-second Pulse test also
passes the capture check. Existing actual-function HDA and ALSA tests and the
real-alsa-lib conversion test pass.

Logs and captures are in `build/udev-audio-boot`: `serial-native-pulse.log`,
`native-pulse.wav`, `serial-vlc-working.log`, `vlc-working.wav`, and the final
`serial.log` for the installed-state reproduction. The local VLC observation
program is `media-player.c` in that directory. Installed file evidence is in
`build/audio-diagnostics/installed-audio-files.txt`.

## Resolution and limits

The evidence identifies a missing userspace policy runtime that reproduces
the reported stall. It does not justify another ALSA/HDA kernel modification
for this failure. The lasting repair is to install a Lua runtime matching
WirePlumber's ABI, remove the old manual output and ACP-disabling overrides,
and keep dependency validation in rootfs provisioning. The ACP-disabling
override has been removed from the source configuration; the installed disk
still requires that cleanup and the matching dependency.

The prior kernel card-discovery changes remain separate. These tests do not
prove that all video formats, Firefox's renderer, or hardware video decoding
are correct.
