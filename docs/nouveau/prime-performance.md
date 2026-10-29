# Nouveau PRIME performance investigation

The supported topology remains one passed-through RTX 4060 Ti and the host's
AMD iGPU. VirtIO presents the guest desktop in the QEMU window; no second
discrete GPU or physical NVIDIA monitor is required for this path.

## Measurements, October 22, 2026

The user confirmed Zink/NVK hardware rendering and eight guest vCPUs. Disabling
vsync did not eliminate the low application throughput. The initial IRQ-off
command-wait correction alone did not resolve the reported slowdown.

A private copy of the installed disk reproduced approximately 1,000 glxgears
FPS on bare Xorg at a 300x300 window size. Across its 40-second sample, VirtIO
frames increased from 2 to 39,470 and uploaded pixels from 2,048,000 to
40,417,280,000. Every upload therefore covered the entire 1280x800 desktop,
approximately 4 GB/s of pixel payload. The native commit callback discarded
its damage rectangles. The subsequent Plasma sample reached approximately
3,800-4,260 FPS after warming up, so bare-Xorg presentation overhead cannot
be assumed to explain every composited application slowdown.

This baseline run hung waiting for timed application termination and was
stopped after its bounded timeout. Its rendering samples are useful evidence,
not a successful end-to-end qualification. Host preflight remained accessible
with no blockers afterward. Logs are retained under
`build/nouveau-c7/prime-command-wait/perf-baseline.log`.

## Damage-aware candidate

The scanout engine now retains accumulated damage independently for each of
its two staging images. Before reusing an inactive image, it copies and
uploads the union of all changes that image missed, not just the current
frame's rectangle. It preserves cropped viewport coordinates, padded source
pitches, framebuffer backing offsets, and tightly packed host staging.

New framebuffer identities/layouts, new surfaces, missing damage, skipped
commits, and failed publication conservatively invalidate history or request
a full update. Source reservation protection still covers the pixel copy and
ends before transport waits. Host DMA backing remains independent of producer
GEM lifetime. Publication still waits for transfer/select/flush acknowledgement;
this is damage-aware staging, not zero-copy or a DRI3 protocol upgrade.

The host regression checks 800 full-frame pixel results after small updates,
including alternating images, changed source identities and offsets, resizing, explicit
invalidation, full fallback, and late transfer completion. Its test workload
reduces uploaded pixels by more than 50%. A mutation that drops accumulated
history fails exact pixels. That reduction is a regression-fixture result,
not a measured application FPS improvement.

The candidate is built privately under `build/nouveau-c7/prime-damage/`.
Software readiness, GPU pixel/stress checks, clean shutdown, and controlled
performance comparison must be recorded for the exact candidate before
activation. The earlier command-wait candidate failed LinuxKPI hardware boot
self-tests despite passing its GPU shader/Xorg oracle; that result must not
be relabeled as fully passing.

## Executed damage candidate

Kernel SHA-256:
`656642b28f898745cc0c8771ec108054440ad3d8820cd5a499cefa8c01a34f9c`.
Both eight-vCPU software readiness and the hardware C7 shader/Xorg oracle pass
for this exact kernel. The latter includes 256 Xorg updates, 600 vkcube frames,
both full-frame pixel captures and clean Nouveau removal/S5.

The private installed-desktop run completed both rendering phases and cleanly
removed Nouveau after explicitly closing its session and Xorg. Discarding the
first two five-second readings in each phase gives:

| Phase | Installed kernel | Damage candidate | Difference |
| --- | ---: | ---: | ---: |
| Bare Xorg | 981.59 FPS | 1,069.01 FPS | +8.91% |
| Plasma | 3,916.67 FPS | 4,472.59 FPS | +14.19% |

These are one pair of private headless runs, not repeated statistical proof or
a same-session comparison to the user's reported 2,000 FPS. The installed
baseline panicked during poweroff with active desktop clients, in
`nve0_bo_move_copy` at `nouveau_boa0b5.c:49`; its rendering samples do not qualify
its shutdown. The candidate's final run closed clients before poweroff, so it
does not demonstrate a fix for that baseline shutdown failure.

Importantly, the candidate's bare-Xorg counters still show full-frame payloads:
frames increased from 2 to 38,917 and pixels from 2,048,000 to 39,851,008,000.
Thus the damage-aware engine's fixture improvement has NOT translated into
reduced upload area in this desktop workload. Coarse upstream damage or repeated
history invalidation must be distinguished before claiming this fixes the
remaining presentation bottleneck. Retained samples and corrected phase-bounded
parsing are in `build/nouveau-c7/prime-damage/comparison.json`; do not use the
older `perf-results.json` bare-phase average, whose parser mixed both phases
when concurrent serial output damaged an exit-marker prefix.

The installed kernel, normal ISO and normal disk remain unchanged. No 124%
causal performance improvement or complete Minecraft-performance fix is claimed.

## Dirty-pixel refinement for coarse desktop damage

The next candidate compares incoming pixels with each staging image's last
acknowledged host contents. Full-screen or missing damage, changing framebuffer
IDs and changed source layouts therefore no longer automatically require a
full upload once a host image is initialized. Trusted accumulated damage still
bounds the comparison when source history permits it. Comparisons use exact
pixel values, not hashes, and cover unaligned offsets and padded/cropped rows.

The host-valid bit is independent of producer identity and is cleared before
modifying staging. It is restored only after a successful transfer. A copy
whose publication was skipped cannot serve as host history: the next upload
falls back to a full snapshot. Unchanged snapshots skip pixel transfers but
still complete selection/flush and presentation bookkeeping. New images and
uncertain transfers keep the conservative full-upload path.

The expanded exact-pixel regression supplies 300 full/missing-damage frames
with rotating framebuffer identities. It uploads 10,142 of 888,300 possible
pixels (98.86% fewer in this small-update fixture) and detects 174 unchanged
snapshots. A mutation that compares against unpublished staging fails the
test. The full scanout, PRIME lifetime, CPU-reservation and transport-ownership
tests pass, including the scanout ASan/UBSan run with LeakSanitizer disabled
because it cannot operate under this environment's ptrace sandbox.

`/proc/drmstats` now also exposes `virtio_scanout_pixels`,
`virtio_compared_pixels` and `virtio_unchanged_frames`. Compare deltas of
`virtio_damage_pixels` and `virtio_scanout_pixels` during the same workload to
measure actual upload-area reduction. Existing DRM full/damage-commit counts
describe the caller's hints, not the refined upload region. Pixel comparison
still reads source memory and full-screen animation can still require full
uploads; this is not zero-copy and does not establish the host AMD iGPU as
the bottleneck or promise an application FPS improvement.

The main `make` build now stages the Nouveau-enabled 6.18 kernel and firmware
into the standard ISO and desktop disk. After closing QEMU, run `make`, then
`make run-nouveau` for the manual GPU test.

## Comparison with the AMD PRIME history

The user requested this comparison instead of collecting guest counters.
At `d1f3fbb` (the commit titled "AMDGPU NOW WORKS"), both
`kernel/linuxkpi/src/native_prime.c` and the native DRM `drm_prime.c` are
byte-for-byte identical to the current files. Import attachment, CPU mapping,
exporter callbacks, reservation locking and write-fence waits are shared by
AMD and Nouveau. That revision also already uses the two-image immutable
staging engine with synchronous transfer/select/flush publication. It does
not establish an AMD-only fast PRIME implementation which Nouveau could
simply select.

An earlier revision, `cf202f3`, does have a materially different VirtIO
presentation implementation: `present_batch()` publishes two or three
commands with `virtq_submit_deferred()` and one kick, and
`async_present_submit()` supports queued completion. The current engine
instead invokes separate synchronous transfer, scanout-selection and flush
commands, so an ordinary alternating-image frame requires three sequential
host response waits. Dirty-region reduction does not eliminate those waits.

This is a concrete transport optimization opportunity, not a measured cause
of the user's 8,000-to-2,000 FPS difference: the later AMD-working commit
already lacks the earlier batching. Restoring batching must preserve the
current immutable staging, timeout quarantine, response validation, resource
retirement and flip-event lifetimes. Copying the old asynchronous code back
without those guarantees is not a safe performance fix. Exporter-specific
fence completion, GPU command submission and the different userspace drivers
remain unmeasured contributors; the comparison does not identify the host
AMD iGPU as the limiting device.

## Batched-command experiment

C4 now enables `virtio_gpu.present_batch=1`. This restores the older
one-kick transport strategy, not the entire historical driver: transfer,
scanout selection and flush are submitted together and awaited as a group.
It retains immutable staging, dirty-region refinement and all required
response checks. It is still synchronous at the presentation boundary;
multiple frames are not allowed to reuse a resource while QEMU owns it.

Every submitted descriptor has its own response slot and fence identity.
Partial submission, reordered completion and timeout leave the shared DMA
unavailable until all submitted descriptors return. A failed batch leaves
scanout selection unknown; recovery disables the uncertain selection before
any resource is reused or retired. No flip is reported as successfully
published until the group completes successfully.

The ownership regression runs the actual batch builder and transport loop
against delayed, reordered and malformed host responses, partial submission,
IRQ/worker wakeup races and disabled fallback. Scanout tests exercise exact
pixels through the batch callback and all three publication failure stages.
GPU performance is for the user's manual test; no 8,000-FPS result is claimed.

Build and launch normally to enable this experiment. For the sequential
comparison, use `make NOUVEAU_PRESENT_BATCH=0` and then
`make run-nouveau NOUVEAU_PRESENT_BATCH=0`. Both commands must use the same
option because the launch gate verifies the complete baked command line.

## Low-logging timing profile

The default preparation keeps batching enabled and uses
`nouveau.debug=info drm.debug=0 gpu_diag=0`. This disables verbose GSP/DRM
logging and the per-event flight recorder for performance comparisons.
Normal warnings/errors and the hang detector remain.

The temporary GPU performance counters and serial timing snapshot command
were removed on 2026-10-29.

For a diagnostic comparison, pass `NOUVEAU_DEBUG=info,gsp=debug`,
`NOUVEAU_DRM_DEBUG=0x4` and `NOUVEAU_GPU_DIAG=1` consistently to both prepare
and run.
