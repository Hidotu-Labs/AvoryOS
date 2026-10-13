# Native graphics corruption flight recorder

The pixel corruption is still unverified. This instrumentation is intended
to replace guesses with a capture from the affected workload. It changes no
Mesa or AMDGPU source and writes no Minecraft preferences.

## Capture in the affected guest

Boot the rebuilt ISO. As root in AvoryOS, launch:

```sh
(sleep 5; echo 2 > /proc/gpu_debug; sleep 15; echo dump > /proc/gpu_debug) &
```

Switch back to the affected game/demo and keep rendering during the capture.
Do not pause the game if pausing stops the corruption. The five-second delay
lets the program resume before sampling begins. The dump freezes the native
recorder and writes it to the host's `build/logs/p6-c7-prime.log` between
`[GPUDBG] frozen flight recorder begin/end`. State whether the corruption was
visible during this capture. A capture after it stops cannot isolate its cause.

The equivalent guest helper is `scripts/gpu-debug-capture.sh [delay] [seconds]
[mode] [output]`. It saves the frozen proc report to a guest file (default
`/tmp/gpu-debug-capture.log`) before exporting it to serial, so serial loss does
not destroy the capture. It is staged as `/usr/bin/gpu-debug-capture` for future image builds;
the command above works with just the new kernel and needs no disk update.

`echo 1` resets/starts metadata recording without pixel checks. `echo 2`
resets/starts metadata plus **64** full pixel checks. After that budget is used,
metadata continues. `echo freeze` or `echo 0` stops recording but retains the
data, `cat /proc/gpu_debug` reads it, and `echo clear` resets a stopped recorder.
Metadata recording starts enabled on boot. All instrumentation can affect
timing/ordering; pixel mode adds three full buffer reads per sampled copy.
If the artifact disappears while recording, say so and compare with
`echo 0 > /proc/gpu_debug`. A debug-induced change is evidence, not proof of a fix.

## What the records mean

Each event has a sequence, guest millisecond timestamp, thread ID, object
identity, four hexadecimal fields and a decimal result. No pixel contents,
shader contents or arbitrary guest memory are printed. All records are native
hooks; vendor sources are unchanged.

| Event | object | a, b, c, d | result |
|---|---|---|---|
| IOCTL | Linux file | request, user argument address, elapsed ms, reserved | ioctl return |
| CS_IN | AMD DRM file | context ID, BO-list handle, chunk count, flags | 0 |
| CS_OUT | same file | context ID, returned submission handle, reserved, reserved | 0 |
| CACHE | Linux page | physical page, previous cache bits, new bits, IRQs masked | 0 after transition |
| CACHE_CHECK | page/VMA | physical page, expected cache bits, actual direct-map bits, reserved | 0 match, 1 disagreement, -1 no mapping |
| MAP | Linux VMA | virtual address, physical page, effective user PTE flags, requested protection | 0 after map |
| FAULT | Linux VMA | aligned address, file page offset, bridge sequence, CPU fault flags | driver VM fault flags |
| IMPORT | native GEM | dma-buf, mapping object, CPU address, byte size | 0 |
| CPU_BEGIN | dma-buf | CPU address, size, access/lock/fence wait ms, reserved | access/wait return |
| FENCE | dma-buf | fence context, fence sequence, reservation usage, fence flags | 0 |
| CPU_END | dma-buf | CPU address, size, reserved, reserved | exporter end-access return |
| COPY_LAYOUT | source CPU address | resource ID, width/height packed in upper/lower 32 bits, source pitch, backing offset | crop x/y packed in upper/lower 32 bits |
| COPY_CHECK | same source | resource ID, source hash before, staging hash, source hash after | bit 0 source changed; bit 1 stable-source mismatch |
| PRESENT | scanout head | resource ID, scanout index, successful publication count, unknown-selection flag | 0 success, -1 failure |

Fence events are sampled from the first eight reservation fences under the
existing reservation lock. They do not take extra fence references or poll
hardware. Fence context IDs and userspace context IDs are different namespaces.

Failed shared-buffer setup uses `IMPORT` records with a negative result and a
separate layout: detail, byte size, stage number, reserved. The detail is the
source ioctl request for stages 1/2, or the imported FD for stages 3/4/9/10.
Attachment/mapping stages identify the DMA-buf as their object. Such failures
are retained in the anomaly ring even after ordinary ioctl traffic wraps.
The first 16 failures per boot also print directly to serial:

```text
[GPUDBG] PRIME stage=7 detail=0x0 size=3145728 error=-12 tid=108
```

This example means CPU mapping failed with ENOMEM; it is illustrative, not
the observed cause of the user's black screen. Stages are:

| Stage | Operation |
|---|---|
| 1 | Source AMD GEM allocation ioctl |
| 2 | Source PRIME export ioctl |
| 3 | Imported FD lookup |
| 4 | DMA-buf byte-size validation |
| 5 | Exporter attachment/pinning |
| 6 | Exporter scatterlist mapping |
| 7 | Exporter CPU mapping |
| 8 | Unsupported I/O-memory CPU mapping |
| 9 | Native mapping/object allocation |
| 10 | Native per-file GEM handle allocation |

Stages 1/2 can include unrelated AMD allocations/exports: correlate their
thread and timing with the affected application. The native PRIME boot
self-test intentionally imports FD=-1, producing one expected stage-3 EBADF
record before desktop startup. No application recovery, GPU reset, forced
synchronization or preset change is triggered by these records. Disabling
the recorder also disables these automatic error lines.

Cache bits use the native 4 KiB PAT encoding: WB=0, UC=0x18, WC=0x98.
Huge-page PAT is normalized before comparison. Cache inspection reads page
tables under the native VMM lock; it does not change PTEs. That lock release
may drain previously deferred native invalidations when IRQs permit, so this
check is also observational instrumentation that can affect ordering.

The 2,048-entry ring wraps; cumulative counters cover the whole capture.
The last 64 pixel checks are retained separately, including successful checks,
so ioctl traffic cannot erase their hashes. They appear as `sample` rows before
the general event ring. Explicit serial export now paces against the serial
queue and drains its final output. A stalled export is bounded to 30 seconds;
the frozen proc report remains readable. Other concurrent serial producers
can still interfere with the best-effort mirror; use the guest file if needed.
The last 32 cache disagreements, changing-source/copy observations, CPU-access
errors and presentation errors are retained separately, so normal frame events
cannot overwrite their details. Freeze before reading to get consistent offsets.

## Interpretation limits

- `cache_mismatch > 0` identifies a native direct-map/alias disagreement; the
  retained record supplies the physical page and expected/actual types.
- `source_changed > 0` means the source changed during a reservation-held
  snapshot. GPU writes are fenced, but concurrent CPU writers can also do
  this. It does not, by itself, prove a missing GPU fence.
- `stable_copy_mismatch > 0` identifies differing hashes for a stable source
  and its tightly packed staging copy, before publication.
- Zero mismatch counters mean nothing if the matching check count is zero.
  Even with checks, they validate only the sampled copies, not every frame.
- Clean copies do not prove shader inputs or GPU-produced pixels are correct.
  A publication acknowledgement also does not prove QEMU's displayed pixels
  match the source. The recorder cannot decide which pixels a game intended.

`userland/test_gpu_constants.c` is a separate offscreen RadeonSI diagnostic
that queues changing shader constants across submissions and checks every
rendered pixel after a batch. Its `--software` mode is only a harness test.
It is staged as `/usr/bin/test_gpu_constants` for future disk builds. Hardware
results from that diagnostic can distinguish a rendering failure from a copy
failure without changing Mesa or presenting through X11/VirtIO.

To transfer the diagnostic without rebuilding or editing the guest disk, on
the host run:

```sh
make gpu-diagnostics
python3 -m http.server 9007 --bind 127.0.0.1 --directory build/gpu-diagnostics
```

Then inside the QEMU user-network guest, as root to disable the recorder:

```sh
echo 0 > /proc/gpu_debug
curl -f http://10.0.2.2:9007/test_gpu_constants -o /tmp/test_gpu_constants
chmod +x /tmp/test_gpu_constants
/tmp/test_gpu_constants > /tmp/gpu-render.log 2>&1
cat /tmp/gpu-render.log
```

This checks constants before display and allows submission overlap; it does
not establish that all Minecraft shaders or texture operations are correct.
A failure isolates incorrect GPU readback before the display path, but does
not by itself distinguish LinuxKPI from Mesa or hardware.

## Affected-workload capture, 2026-10-13

The user confirmed the flicker continued while this capture ran. The header
reported 65,598 events, 6,835 decoded CS entries and successful exits, 814 CPU
begin/end pairs, 815 presentations, 64 pixel checks, and 5,836 cache checks.
Source-change, stable-copy-mismatch, cache-mismatch, and retained-anomaly
counters were all zero. The retained fault result 256 is VM_FAULT_NOPAGE,
not a fault error. These observations validate the sampled CPU copies and
inspected cache mappings, not GPU shader correctness or QEMU display contents.

Only 393 of the expected 2,048 general event rows survived as complete lines
in both the pasted attachment and the host serial log. The end marker was
missing and no pixel-check rows survived the event-ring wrap. The changes
above address these diagnostic losses; they are not a claimed pixel fix.

## Validation

The actual recorder is exercised by `scripts/tests/test_gpu_flight_recorder.py`:
8,000 concurrent events, wraparound, freeze, 37-byte reads, the 64-check limit,
packed/padded image hashes, and retained anomalies after normal-event wrap.
Existing CPU snapshot exclusion, cache alias and scanout lifetime/publication
tests pass. The rebuilt ISO's eight-CPU KVM fixture arms mode 2 through the real
proc file and dumps through the real guest write path. It completed 400 flips
and 400 damage commits; 64 pixel checks and 1,007 cache checks reported zero
mismatches, and 76 active host frames plus the final 1,024,000 pixels matched.
The fixture has no physical AMD GPU; these are controlled native checks, not
validation of the reported Minecraft/Mesa artifact.
