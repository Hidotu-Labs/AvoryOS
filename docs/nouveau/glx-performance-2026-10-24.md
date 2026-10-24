# glxgears synchronization investigation — October 24, 2026

The retained evidence points to submission/completion and GLX presentation
latency, rather than a demonstrated loss of NVIDIA rendering throughput. It
does not yet distinguish time spent waiting for GPU completion from time spent
waiting to run after notification. No measured 8,000-FPS AMD comparison with
the same session, dimensions, synchronization settings and instrumentation is
available in these artifacts.

## Renderer and workload

`build/nvidia-fps/fps-final-renderer.txt` identifies direct, accelerated rendering
on the RTX 4060 Ti through Zink/NVK, Mesa 26.2.5. The final sample has fourteen
five-second readings, 4,573–5,352 FPS, mean 4,921 FPS (4,891 after discarding the
first two). The launch script explicitly sets `vblank_mode=0` and
`MESA_VK_WSI_PRESENT_MODE=immediate`. This rules out a simple 60-FPS cap in that
sample; it does not prove identical synchronization settings in every run.

The user's screenshot shows roughly 3,100–4,800 FPS. Its generic “synchronized
to the vertical refresh” message does not establish that every frame was
actually presented at the physical display's refresh rate.

## Application traces

`scripts/analyze-glx-performance.py` takes deltas between cumulative snapshots,
excludes intervals intersecting a swap pending for 100 ms or more, and marks
truncated counter sets. It does not turn the unaccounted portion of an
unfinished swap into apparent rendering work. Detailed results are retained
in `build/glx-investigation/trace-analysis.json`.

| Retained trace | Steady FPS | Time in intercepted swap per completed frame |
| --- | ---: | ---: |
| `baseline2-fps-render-only` | 6,496 | 152 µs |
| `headless-traced-fps-repeat-2` | 3,729 | 266 µs |
| `headless-traced-fps-repeat-3` | 4,349 | 228 µs |
| `gtk-traced-fps-repeat-2` | 133 | 7,280 µs |
| `gtk-traced-fps-repeat-3` | 131 | 7,600 µs |

These are historical experiments, not a new controlled A/B comparison. The
GTK/headless names identify saved profiles; their difference alone does not
establish a display-backend cause. The later final sample already performs
far better than the older 130-FPS experiments. Tracing also adds overhead.

The probe's render-only mode replaces `glXSwapBuffers` with `glFinish`; it
still submits and waits for GPU rendering. It is not an asynchronous draw-rate
benchmark. In complete post-warmup counter intervals, render-only makes about
one Nouveau EXEC and one DRM SYNCOBJ_TIMELINE_WAIT per frame. EXEC takes about
12 µs/frame and timeline waiting 89–93 µs/frame. Normal healthy headless
intervals make about 5.3–5.5 timeline waits/frame, totaling 70–210 µs/frame.
The normal swap path therefore adds synchronization/presentation activity;
the finish-only result shows that removing presentation alone is insufficient
to demonstrate 8,000 FPS.

Old GTK intervals spend 6.7–7.3 ms/frame in timeline waits, accounting for most
of their swap time. Some traces later stop making progress inside a swap.
Those stall intervals must remain separate from steady-state throughput.

Ioctl times are summed caller wall time, can overlap between threads, and are
already included in swap duration. They must not be added to swap duration or
called GPU execution time. Initialization costs and partial final reports are
excluded from the per-interval ioctl comparisons above.

## Latest retained kernel snapshot window

In `build/nvidia-fps/final-serial.log`, snapshots at 26.930807431 and
100.103415240 seconds span 73.172607809 seconds. Their counter differences are:

- Native scanout: 59.76 frames/sec.
- Producer CPU-access/fence synchronization: 0.030 ms of caller time/sec.
- Pixel comparison/copy: 33.80 ms/sec.
- VirtIO transport: 7.43 ms/sec.
- Nouveau EXEC: 4,773 calls/sec, 9.73 µs/call.
- Timeline waits: 28,836 calls/sec, 17.40 µs/call, 501.7 ms of summed caller
  wall time/sec.
- Already-ready timeline fast paths: approximately 19,094/sec.

These kernel counters combine DRM clients/devices. They corroborate substantial
synchronization traffic, but do not attribute every call to glxgears. The
scanout path is operating near 60 Hz while application submission runs at
thousands/sec; full desktop uploads are not happening once per glxgears frame
in this composited sample.

## Added measurement

With `gpu_perf=1`, compact `gpup` snapshots now include
`syncobj_upstream=poll` and `syncobj_upstream=wait`, each with counts, total
caller time, negative-return counts and exclusive latency buckets: below
5 µs, 5–50 µs, 50–250 µs, 250 µs–1 ms, 1–10 ms, and at least 10 ms. Poll means
the ioctl's absolute timeout is zero; wait means nonzero, which may still be
an expired deadline or return without sleeping. A negative polling result can
be an expected timeout, not a driver fault. The existing ready-fast counter
remains separate. Counters are cumulative and combined across clients; compare
deltas around one controlled workload.

This instrumentation preserves the upstream result, wait flags, timeout and
fence behavior, and is disabled with `gpu_perf=0`. It reports only when the
existing compact snapshot is requested; it adds no per-frame logging.

The next controlled comparison should use equal window sizes and the same
session, compare ordinary swaps with the finish-only probe, and inspect these
new poll/wait distributions. If blocking-wait latency dominates, completion
versus wake-to-run timing is the next split. Do not remove completion waits,
fabricate fence completion, or globally disable synchronization to inflate FPS.
Minecraft's reported higher throughput remains compatible with a faster GPU
and a different, more substantial per-frame workload; its FPS cannot identify
which part of this small GLX benchmark is limiting.

## Validation and limits

The parser tests cover stalled swaps, truncated reports, cumulative deltas and
counter resets. The native profiling test covers bucket boundaries, poll/wait
separation, negative results and disabled profiling. The timeline-ready test
covers 176 wait/flag/fault/refcount cases while preserving upstream fallback.
Both changed kernel objects compile. No new GPU VM has been run, no FPS gain
is claimed, and neither installed images nor driver packages were changed.

## Local wakeup scheduling correction

The user's subsequent live samples contain real upstream wait tails (about
26% at least 1 ms in one 23.30-second interval), while zero-timeout polls average
under 1 µs. This does not establish which client or GPU caused the waits.

A source audit did identify an avoidable local scheduling delay: a kernel
worker waking a more eligible thread on its own CPU set need_resched and
armed a timer at now+1 ms, without requesting an immediate scheduling boundary.
An IRQ or syscall return could service it, but ordinary kernel-worker execution
had no such guaranteed boundary. Remote wakeups already sent reschedule IPIs.

`sched_notify_runnable` now shares the notification policy between enqueue and
wake paths. It marks the local request, retains the timer backstop and sends
a self reschedule IPI after releasing the runqueue locks. Interrupts remain in
the caller's saved state, so native IRQ-masked regions defer delivery until
unlock. KPI atomic sections retain the request through the existing IRQ-exit
preemption guards and preempt-enable hook. A local hardirq already has an
EOI/irq-exit scheduling boundary and skips the redundant IPI. Remote notification
and EEVFD's eligibility decision are retained. This grants no new priority
boost and changes no fence completion, timeout or rendering semantics.

The local-wake regression compiles the real notification and scheduling-boundary
functions. It verifies handover before a timer tick, outer IRQ masks, KPI pins,
MSI-batch coalescing, remote notification and APIC-not-ready behavior. Removing
the self IPI fails the immediate-handover case. The wake-publication fixture
was refreshed for current NUMA/runqueue structures and valid on-CPU switch-tail
states; its forced concurrent wake/publication cases pass. IRQ-exit/pinning,
prepared-wait and deadline-tree tests pass, and the scheduler object builds.

This fixes a concrete latency gap; the existing counters do not establish how
much it contributes to glxgears. A rebuilt/rebooted kernel and repeated workload
are required to measure FPS and wait-tail changes. No new installed image or
live VM has been modified for this correction.

## Follow-up: no demonstrated FPS gain

The user's post-reboot screenshot still shows approximately 3,000–5,000 FPS.
This does not demonstrate that the self-IPI correction improved glxgears. The
next step is attribution rather than another scheduler policy change.

Opt-in profiling now marks the duration of an upstream syncobj ioctl on its
native thread. Native KPI block/timeout paths mark park and resume, and the
KPI wake helper records the first notification for that particular park.
Reports include `syncobj_sleep caller=glxgears` and `caller=other`:

- `parks`: profiled native sleep/yield cycles within upstream syncobj calls.
- `notified`: those with a recorded first KPI wake before resume.
- `before_notify_ns`: summed park-to-first-notification time for notified parks.
- `after_notify_ns`: summed notification-to-resume time for notified parks.
- `unnotified_ns`: sleep time with no recorded KPI notification, such as a
  timeout or a different native wake source. It cannot be assigned to either
  side of the split.

These are cumulative counters. Divide the before/after time deltas by the
`notified` delta for averages. They do not measure hardware fence execution:
before-notification includes GPU work, interrupt handling and completion-worker
latency; after-notification includes wake processing, CPU scheduling and the
remaining waker critical section. Notifications may also be spurious. The
`other` bucket combines compositor and other clients. Native mutex sleeps
inside a profiled ioctl are included, rather than silently called GPU work.

A park-specific compare/exchange token prevents a late profiler writer from
stamping a subsequent sleep. Multiple notifications retain the first one.
The fields are appended to the native thread layout to preserve assembly
offsets. No scheduler decision or fence behavior depends on the timestamps.

The host script supports `--perf --wait`, which prints one fresh, complete
report through `[GPUPERF] end`. The prior send-and-immediate-tail command could
observe a new header before the histograms were written, as in the user's
pasted output. `--log` supports custom serial-log paths. Tests cover partial
report writes and log truncation; the default command still sends only the
compact `gpup` request.

After rebuilding/rebooting for the new kernel fields, run twice around the same
workload:

```sh
python3 scripts/prime-dump.py --nouveau --perf --wait
```

The changed native objects compile. Profiling tests cover split timing, first
notification retention, a stale-writer/new-park interleaving, disabled mode,
poll/wait histograms and unchanged wait results. Native prepared-wait and
concurrent wake-publication regressions pass. No new FPS gain is claimed.

## Measured notification/resume split

The user's complete snapshots at 41.931937694 and 81.462433006 seconds
finally attribute the sleep delay to glxgears itself. Across 19,287 additional
notified glxgears parks:

| Component | Average per notified park |
| --- | ---: |
| Park to first KPI wake notification | 842.99 µs |
| First KPI notification to resume | 16.05 µs |

98.13% of the measured notified sleep time precedes the wake notification.
There is also one additional unnotified park totaling 1,764 ns. These totals
exclude ready fast paths and include all native sleeps within the profiled
syncobj ioctl. This evidence does not support further scheduler handover
changes as the primary way to close the FPS gap.

The pre-notification interval is not hardware-only time. The imported Nouveau
completion path can pass through `nouveau_fence_wait_uevent_handler`, which
queues `nouveau_fence_uevent_work` onto system_wq; that callback reads fence
progress and signals completions. The EXEC path itself runs through a DRM
scheduler submission workqueue. Hardware completion, IRQ delivery and these
worker stages are therefore the next attribution targets. The current timing
split does not justify deleting fence waits, signaling fences prematurely,
or claiming that GPU execution alone takes 843 µs.
