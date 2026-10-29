# Sustained CPU spike after closing a YouTube tab

The reported failure is approximately 90% CPU across eight virtual CPUs,
remaining high for more than a minute after closing YouTube. The screenshot
shows ample free memory and no swap use. It does not establish which kernel
path accounts for the CPU time. The affected live VM was closed before a
scheduler snapshot could be collected.

## Isolated measurements

The current kernel was tested in disposable KVM guests with eight CPUs, 4 GiB
RAM, RTL8139 networking, and a separate filesystem copied from an offline
base. Graphical cases used KWin Wayland, VirGL and virtual HDA audio.

| Workload | Final 30-second CPU average before terminating Firefox |
| --- | ---: |
| Eight script-opened network-heavy pages, each closed after five seconds | 0.19% |
| 1080p60 VP9/Opus via MediaSource, then destroy the document | 2.10% |
| 1080p60 AV1/Opus via MediaSource, then destroy the document | 0.72% |

These percentages use deltas of `/proc/stat` user, system and idle counters,
aggregated over all eight CPUs. The video documents were replaced after at
least 30 seconds of decoded playback. The final averaging window occurs more
than 40 seconds after document destruction, while Firefox remains running.
The test rejects sustained final-window CPU above 25%, leaving room for
background browser and desktop work while detecting the reported saturation.

Artifacts: `build/firefox-page-close-boot/serial.log`,
`build/firefox-close-video-boot/monitor.log`, and
`build/firefox-close-av1-boot/monitor.log`, with video metrics alongside them.

A real YouTube watch-page diagnostic was also attempted. Initial attempts
could not obtain a display surface even after 120 seconds before the scheduled closure, so they
**do not count as YouTube playback or tab-closure tests**. The diagnostic
explicitly fails if it does not reach the closure step, and requires visual
inspection of screenshots to establish what actually loaded. Local media
playback cannot establish that the real-site bug is fixed.

## Regression probes

- `scripts/tests/test_firefox_page_close_boot.py`: repeated document/window
  destruction with eight concurrent recurring network fetches per child.
- `scripts/tests/test_firefox_video_boot.py --audio --virgl --wayland --mse
  --cpus 8 --close-page --clip build/firefox-video/1080p60.webm`: VP9 teardown.
  For AV1 use `--codec av1` and an AV1/Opus WebM fixture.
- `AVORY_TEST_DESKTOP_FS` must identify an offline raw filesystem seed. The
  shared preparation helper refuses a source filesystem open by QEMU.

Two network worker sleep-publication races were subsequently fixed; see
`network-worker-sleep-2026-10-28.md`. They are independently demonstrated bugs,
not a verified explanation of the reported sustained CPU spike.

## Next reproduction

While the actual VM is slow, run:

```sh
head -c 3900 /proc/sched_debug > /dev/kmsg
```

The kernel does not register `/dev/ttyS0`; that earlier suggested output target
was incorrect. `/dev/kmsg` is the registered kernel log sink.

This sends the runnable-thread snapshot to the host serial log (normally
`build/logs/desktop-serial.log`). Repeating it a few seconds later helps
separate a persistent runnable loop from an incidental sample. Correlate it
with CPU counters and the affected thread's `/proc/TID/syscall`; do not infer
networking from the CPU meter alone.

The later [Plasma 6 / RTL8139 exact-sequence investigation](firefox-rtl8139-reopen-2026-10-28.md)
records the interrupt-completion and allocator-query fixes, three verified
YouTube reopen/playback cycles, CPU recovery and the replacement ISO.
