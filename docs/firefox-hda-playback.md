# Firefox ALSA/HDA playback corrections

Date: 2026-10-15. Host tests and boot ISO build pass; audible guest playback
has not yet been verified. The running VM was left running.

The report is silent YouTube playback, roughly 1.5x/softened audio on other
sites, and silence after 10–30 seconds. Source inspection identified concrete
driver faults compatible with those symptoms; it does not prove which fault
occurred during the reported Firefox session.

## Corrections

- ALSA refinement previously replaced an impossible access/format mask with
  supported choices and clamped disjoint rate intervals into success. Final
  HW_PARAMS then forced stereo/48 kHz regardless of the request. It now returns
  EINVAL for incompatible constraints, preserves open/empty interval semantics,
  and validates HW_PARAMS before touching the live hardware. A 32 kHz source
  rendered unchanged at 48 kHz would play at 1.5x; clients must use software
  conversion when their source format differs from the hardware format.
- HDA queue/getter/reset helpers unconditionally enabled interrupts, including
  when invoked from the HDA completion interrupt. They now use an IRQ-saving
  stream spinlock shared by all CPUs. The DMA/ring accounting, format changes,
  reset, and OSS snapshots are serialized, and the original IRQ state is
  restored. OSS argument access and blocking-write user copies occur outside
  the stream lock; blocking writes stage at most 4 KiB at a time.
- Descriptor service invoked the ALSA callback on starvation even when called
  by `hda_queue_pcm` from that same callback. This could recurse before ALSA
  advanced its consumed pointer, duplicate submissions, or exhaust the stack.
  Only the ISR now invokes callbacks, after releasing the stream lock.
- The ALSA mmap pump is serialized across completion interrupts and queries
  on other CPUs, so they cannot submit the same frame range concurrently.
- HDA copies align to the actual PCM frame size, including 32-bit stereo,
  rather than always rounding to four bytes.

The lock order is ALSA pump -> HDA stream. The ISR releases the HDA lock before
calling ALSA. Blocking writers release the stream lock before registering or
sleeping on a wait queue; they register a fallback deadline with local IRQs
masked and recheck free space after registration.

The ALSA implementation still exposes one global hardware stream. Independent
clients do not get per-open PCM buffers or software mixing from these changes;
concurrent HW_PARAMS/PREPARE/reset operations remain a separate limitation.
This also does not add codecs, fix browser decoding/network problems, implement
full ALSA pause/drain semantics, or infer missing entire DMA revolutions from
LPIB when interrupts are delayed longer than the hardware ring duration.

## Verification

Run from the repository root:

```sh
python3 scripts/tests/test_hda_playback.py
python3 scripts/tests/test_alsa_playback.py
make -j8 avoryos-x86_64.iso
```

Both harnesses compile actual driver functions with AddressSanitizer and
UndefinedBehaviorSanitizer and deterministic hardware/scheduler fixtures.

- HDA: preserved IRQ flags from an ISR, no recursive underrun callback, one
  callback per completion IRQ outside the stream lock, four concurrent
  producers with byte-for-byte sample accounting, user-copy failure cleanup,
  blocking writer wakeup, full-frame alignment, and 10,000 descriptor
  completions (over 100 seconds at 48 kHz/S16/stereo) without order/delay loss.
- ALSA: incompatible rates, formats, access modes, channels, open endpoints,
  and empty constraints fail; valid 48 kHz stereo and a broad rate interval
  negotiate successfully. More than 100 seconds of mmap ring wrapping and four
  competing pumps submit each frame once, with correct delay/availability.
- The old HDA source fails the IRQ-preservation assertion. Substituting the
  old ALSA ioctl body fails the exact-32-kHz rejection assertion.
- ISO build succeeds, retaining the existing RWX LOAD-segment linker warning.

Reboot the VM into the rebuilt ISO. Play a YouTube video and a previously
affected other site for at least two minutes each, then pause/resume and seek.
Repeat while loading another application. Compare audio speed with video and
check whether sound survives beyond the previous 10–30-second cutoff. If it
fails, capture fresh `[ALSA] HW_PARAMS` lines, browser audio errors, and whether
another audio client was active.

## Follow-up: residual speed and silent YouTube

The user reports that sample softness is resolved but playback remains slightly
fast, and YouTube remains silent. The other affected site is
<https://offihito.vercel.app/>. Its first MP3 reports 44.1 kHz stereo in ffprobe;
its player uses a normal HTML audio element routed through Web Audio.

A new integration harness loads the host's real `libasound.so.2`, opens its
`plug -> hw` chain, and forwards device ioctls to the actual kernel ALSA code.
Only device syscalls and hardware delivery are mocked. This exposed a defect
the earlier internal-driver tests missed: HW_REFINE/HW_PARAMS prematurely
selected a 1024-frame period and 4096-frame buffer, replacing the 1200/4800
hardware geometry needed by a 44.1-to-48-kHz converter. The client retained its
1102-frame/4410-frame geometry, so the conversion chain dropped samples.

For 441,000 input frames (10 seconds at 44.1 kHz), the previous code delivered
409,600 hardware frames at 48 kHz: 8.533 seconds. The corrected code delivers
480,000 hardware frames: exactly 10 seconds. This is a host integration
measurement, not an audible Firefox result.

HW_REFINE now propagates and intersects sample/frame widths, period and buffer
bytes/times/sizes, and period counts without choosing defaults. HW_PARAMS
preserves valid exact constraints, including periods below 64 or above 16,384
frames, and rejects inconsistent geometry instead of silently resizing it.

Additional validation:

```sh
python3 scripts/tests/test_alsa_lib_playback.py
```

The real-library test covers 44.1 kHz stereo S16, 48 kHz stereo float, 48 kHz
mono float, and 48 kHz stereo S16, plus different buffer latencies. All produce
480,000 non-silent hardware frames for 10 seconds of input. The previous
geometry fails this regression. This test requires a host ALSA library and its
configured rate converter; the existing driver fixtures remain sanitizer-tested.

The float/mono tests exercise conversion paths relevant to browser playback,
but do not establish why the reported YouTube session was silent. Repeating
both sites in the rebuilt guest is still required; per-open stream isolation
and software mixing remain outside this change.
