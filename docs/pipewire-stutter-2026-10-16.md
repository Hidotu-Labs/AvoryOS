# PipeWire playback gaps and load tolerance

The initial twenty-second tone recorded through PipeWire's PulseAudio bridge
contained 256 silent gaps totaling about three seconds. Most were one or two
2048-byte HDA descriptors long. VLC additionally logged repeated late-buffer
flushes. The earlier playback tests checked process completion, not sample
continuity, so they missed this failure.

The HDA queue now appends arriving samples to unused future descriptors after
the last queued sample. It never writes the descriptor currently being played.
Previously, early writes left silent look-ahead descriptors that could only be
refilled after they had already played. Delay queries now account for the live
position within the current descriptor instead of reporting only completed
periods. ALSA respects start_threshold and explicit START: PipeWire uses a very
large threshold specifically to prime the queue before starting hardware.

A separate timerfd fault could strand an imminent expiry after an earlier
scheduler interrupt consumed the LAPIC's one-shot timer. Every timer tick now
retains the earliest pending timerfd deadline; contended scans retry shortly.
Periodic expirations retain their original phase and count missed intervals.
The actual-function test fails the early-IRQ assertion against the old code.

For load tolerance, HDA uses sixteen 2048-byte descriptors instead of four.
This extends the DMA ring from about 43 ms to 171 ms at 48 kHz stereo S16 while
retaining approximately 10.7-ms completion interrupts. A WirePlumber rule queues
8192 extra frames (171 ms at 48 kHz), plus the processing quantum. This trades
additional audio latency for tolerance of graphics and memory-allocation stalls.
The image builder installs config/wireplumber/20-avoryos-hda.conf and tracks it
as a rootfs build dependency. The existing disk can carry the same rule in its
existing 10-avoryos-vm.conf, without allocating new filesystem blocks.

Verification:

```
python3 scripts/tests/test_hda_playback.py
python3 scripts/tests/test_alsa_playback.py
python3 scripts/tests/test_alsa_lib_playback.py
python3 scripts/tests/test_timerfd_deadlines.py
AVORY_AUDIO_SECONDS=20 AVORY_AUDIO_CAPTURE=1 AVORY_AUDIO_PULSE=1 AVORY_AUDIO_STRESS=1 python3 scripts/tests/test_udev_audio_boot.py
python3 scripts/tests/check_audio_capture.py build/udev-audio-boot/hda-output.wav
```

The driver fixtures use actual functions with ASan/UBSan. HDA checks sample
ordering across long ring wrapping and an interrupt delayed by nine periods
(about 96 ms), without touching the currently playing slot. ALSA checks explicit
start and automatic thresholds in addition to real-library rate conversions.

Private KVM captures use QEMU's WAV backend, without host audio or VFIO. The
20-second Pulse bridge tone is exactly 20 seconds and has zero silent gaps over
1 ms and zero sample discontinuities under eight allocation workers. Each
worker repeatedly touches and frees 4 MiB. A full disk clone playing the same
WAV through VLC/pulse with the same workers also has zero steady-playback gaps
and discontinuities. VLC's captured tone spans about 19.88 seconds; the analysis
allows 150 ms of onset/drain variation and excludes the first/last 200 ms from
the continuity check. It does not establish bit-exact playback of every frame.
Logs and captures are under build/audio-quality.

This does not guarantee playback through arbitrarily long IRQ-disabled regions.
The HDA position register alone cannot identify complete missed DMA revolutions;
service delays exceeding the enlarged ring still need further driver latency
work. Audible output on the user's particular host backend and passed-through
GPU load remains a separate check from the private recorded-sample tests.

ABI/behavior references: upstream [PipeWire ALSA backend](https://github.com/PipeWire/pipewire/blob/master/spa/plugins/alsa/alsa-pcm.c)
uses an explicit start after setting start_threshold to LONG_MAX.

Installed into disk.img and the normal boot ISO after QEMU stopped. The disk
change uses only the existing WirePlumber file block and inode size/checksum;
filesystem allocation metadata is unchanged. Backups and install-report.json
are in build/audio-quality (disk-before.img and iso-before.iso).
