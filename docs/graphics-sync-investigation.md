# Graphics synchronization investigation

The old graphics benchmark included `ARB_timer_query` in the same frames used
for its `glFinish` measurement. On this Virgl stack, query polling can hold up
fence retirement for approximately 10 ms. That result did not isolate plain
GPU completion or the desktop compositor.

The corrected `separate_queries_v2` protocol uses three phases:

1. Draw the same scene and measure submission plus `glFinish`, without queries.
2. Draw with timer queries and report query-instrumented completion, result
   readback, and device time separately.
3. Draw with normally queued swaps and measure presentation/event proxies.

Each phase excludes ten warm-up frames. Device samples exceeding the enclosing
wall interval by more than 1 ms are rejected and the batch's device metric is
skipped. The native device clock passed; guest values still sometimes fail.
The comparison analyzer rejects completion/submission comparisons across old
and new protocols rather than reporting the measurement correction as a win.

The private graphical runner now waits for the X compositing manager selection,
installs the freshly built executable into its private image, supports repeated
batches and frontend controls, bounds compositor shutdown, and removes stale
host output files before extracting new results. A startup race that previously
allowed a compositor-absent run is therefore excluded from final measurements.

## Results and retained changes

See the [complete results and caveats](../build/benchmark-results/graphics-sync-comparison.md).
Three 1000-sample batches on the existing guest kernel measured plain completion
p99 of 3929, 401, and 381 us. The median batch p99 was **401 us**, versus **330 us**
for three native CachyOS runs. The first guest batch's longer tail is retained.
The guest's query-instrumented wait still measured approximately **10.06 ms p99**.
Synthetic X11 event delivery p99 remains about **415 us versus 45 us** natively.
These are different graphics/presentation stacks, not an overall OS ranking.

An experiment boosted the GPU completion worker to nice -10. Its median batch
p99 was 610 us; it did not improve the 401 us baseline and was removed.
There is **no retained kernel speed optimization** from this investigation.
The benchmark correction changes what is measured, not how fast ordinary
applications execute. The prior scheduler/wakeup improvements remain intact.

SDL and EGL-headless both showed the query delay. SDL requested asynchronous
fences, so switching frontends alone did not solve it. GTK/X11 failed EGL window
surface creation on this host. The pinned renderer's `wait_sync()` waits for
main-thread query polling before retiring fences with pending queries; QEMU's
fallback poll timer is 10 ms. The remaining query coordination issue warrants
host-renderer/QEMU investigation, while visible desktop stalls require tracing
actual compositor frames and input delivery. Synthetic swap/OML values do not
measure physical input-to-photon latency.

## Reproduction and validation

```sh
make -C userland bench_desktop.elf
python3 scripts/tests/run_desktop_graphical_benchmark.py \
  --graphics --display sdl --samples 1000 --repetitions 3
# Query-free control; normal v2 already keeps queries out of plain completion.
python3 scripts/tests/run_desktop_graphical_benchmark.py \
  --graphics --display sdl --samples 1000 --gpu-timer off
# Native CachyOS, from the graphical session:
./userland/bench_desktop.elf graphics 1000 /tmp 2
python3 scripts/tests/test_desktop_benchmarks.py
python3 scripts/tests/test_desktop_benchmark_analysis.py
python3 scripts/tests/test_desktop_benchmarks_boot.py
```

The private runner clones the partition cache and never modifies `disk.img`.
Its repeated-run output is concatenated, with `RUN repetition=N` markers.
Save outputs between trials; the runner reuses its private output directory.
Use `AVORY_TEST_KERNEL` to select a saved kernel. `--display egl-headless` is an
offscreen diagnostic, not a replacement for visible frame-pacing measurements.
