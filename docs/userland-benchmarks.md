# Userland performance benchmarks

Build the individual executable targets like the other userland tests:

```sh
make -C userland avory_bench.elf bench_cpu.elf bench_iterations.elf bench_fork.elf bench_vfs.elf bench_desktop.elf
```

Each target can also be built separately, for example
`make userland/avory_bench.elf` from the repository root. This uses the normal
AvoryOS glibc toolchain, sysroot and linker settings. No network access
is needed: the selected UnixBench sources are vendored and pinned; see
[provenance](../userland/benchmarks/UPSTREAM.md).

The normal `make disk.img` build compiles and installs all six native programs into
`/bin`, without the `.elf` suffix. Shut down the guest before updating its disk,
then boot the updated image and run `avory_bench all 3 32 /tmp`.
For manual copies, keep all six executables together with matching suffixes:

| Program | Workload |
| --- | --- |
| `avory_bench` | Suite runner; native RAM, metadata and startup workloads |
| `bench_cpu` | UnixBench Dhrystone 2 integer workload |
| `bench_iterations` | UnixBench Hanoi recursive workload |
| `bench_fork` | UnixBench fork, exit and wait throughput |
| `bench_vfs` | UnixBench fstime read, write and copy throughput |
| `bench_desktop` | Scheduling, storage, application, graphics and tail latency workloads |

When MinGW is available, image builds also build and install
`/bin/bench_compat.exe`, a real Win32 workload for Wine. It can be built
separately with `make -C userland bench_compat.exe`. Without MinGW or an
existing executable, the native suite still builds; compatibility is skipped.

Run in an AvoryOS terminal:

```sh
./avory_bench.elf all 3 32 /tmp
./avory_bench.elf core 3 32 /tmp
./avory_bench.elf cpu 5
./avory_bench.elf iterations 5
./avory_bench.elf fork 3
./avory_bench.elf startup 100
./avory_bench.elf memory 64
./avory_bench.elf vfs 3 /home
```

`all [seconds=3] [RAM_MiB=32] [workdir=/tmp]` runs the original component
tests followed by the desktop suite with 120 samples and two load workers.
`core` runs only the original component tests, preserving the old workload
for comparisons with earlier logs. Durations
apply to each upstream test, not the whole suite. fstime additionally primes
its files and sleeps between phases, so the suite takes longer than the sum
of the requested measurement times. RAM size is bounded to 1..1024 MiB;
choose a size appropriate to the guest. Startup accepts 1..100000 launches.

The harness discovers sibling executables through `/proc/self/exe`; keep all
six together. It also recognizes the `.elf` build names when run directly
from `userland/` on Linux. Individual upstream components can be run directly:

```sh
bench_cpu 3
bench_iterations 3 16
bench_fork 3
bench_vfs -w -b 8192 -m 32768 -t 3 -f -d /path/to/private/scratch
```

Use a private scratch directory for direct fstime runs. The harness creates
unique directories under the selected filesystem and removes them on normal
completion. A failed or interrupted run can leave its scratch directory;
the path begins with `avory-meta-` or `avory-io-`. The suite uses two files of
up to 32 MiB each and never opens a raw disk.

## Desktop latency suite

Run these from a graphical desktop terminal for graphics coverage. Choose a
directory on a persistent filesystem for storage coverage; `/tmp` on either
OS may be tmpfs. The suite creates private scratch files under that directory.

```sh
mkdir -p "$HOME/avory-bench"
avory_bench desktop 120 "$HOME/avory-bench" 2
avory_bench scheduling 1000 /tmp 2
avory_bench storage 1000 "$HOME/avory-bench"
avory_bench apps 120
avory_bench graphics 1000
avory_bench browser 3 "$HOME/avory-bench"
avory_bench command 5 -- /path/to/application --self-test
```

The same modes work directly with `bench_desktop.elf`; its `all` mode is
the desktop suite, while `avory_bench all` includes the original suite too.
The native executables dynamically load optional graphics/font libraries,
so a headless minimal installation can run scheduling/storage/application
tests without installing X11, Mesa or font packages.

Every sampled latency reports min, mean, p50, p90, p95, p99, max and counts
above 16.667, 50 and 100 ms. Percentiles use the nearest-rank definition.
`AVORY_BENCH_RAW=1` emits every measured sample in acquisition order, after
its timed phase. Use at least 1000 samples for useful tail comparisons;
with 120 samples p99 is approximately the second-largest sample, and a
three-run browser result cannot establish a stable p99. One-off initialization
timings have count=1 and their percentile fields are that single observation.
There is no combined score that trades latency against throughput.

| Category | Workload and interpretation |
| --- | --- |
| Scheduling | 5 ms deadline wakeup lateness idle and under competing integer/8 MiB memory workers. Main and workers share one CPU when affinity is available; the original affinity is restored afterward. Missed periods are counted, rather than replaying overdue samples in a catch-up burst. |
| IPC | Verified 64-byte pipe and Unix-socket ping/pong round trips, idle and under the same CPU/memory load. One warm-up exchange is excluded. |
| Storage contention | Timer wakeup lateness while a background process repeatedly writes 4 KiB and calls fdatasync on the selected filesystem. On tmpfs this is memory-filesystem contention, not physical disk contention. |
| Real filesystem | A private 32 MiB file: buffered writes, write plus fdatasync, writeback completion, sequential and random 4 KiB reads, repeated write-plus-fsync latency, directory fsync and traversal of eight directories containing 128 files. Read data is validated. Only explicit write-plus-fsync is scored as synchronous writing; accepting O_DSYNC alone does not prove the kernel implements that flag. |
| Dynamic loading | Repeated dlopen/resolve/call/dlclose of libm, and four concurrent fork/exec workers that load libm and send readiness timestamps. Reports last-worker readiness and full exit/reap completion separately. |
| Fonts | Real Fontconfig initialization/matching and FreeType face loading and rasterization of 95 ASCII glyphs at 18 pixels, with a bitmap checksum and the selected font path. Match the font file/version when comparing OSes. |
| Rendering/composition | An 800x600 GLX window draws 256 triangles and blends twelve textured translucent layers. Reports CPU submission and plain glFinish completion in a query-free phase. A separate phase reports query-instrumented completion, query readback, and device time through ARB_timer_query when supported. Impossible device durations are rejected. This is synthetic layer composition, not an isolated timing of the desktop compositor. The suite records whether an X compositing manager is active. |
| Frame pacing | A separate queued-swap phase requests swap interval 1 where supported, measures swap calls and wall intervals, and uses OML swap-completion timestamps/refresh counts where supported. Ten warm-up frames per phase are excluded. Synthetic X11 actions measure event delivery and action-to-swap completion. Without OML, wall completion is explicitly a proxy; none of these is physical input-to-photon measurement. |
| Browser | Chromium-family headless process startup through verified local-page completion and process exit. The generated page performs integer work, creates 1000 styled DOM nodes, forces layout and draws 2000 canvas rectangles. A DOM completion attribute and checksum must be present in the dumped DOM. Each run has a separate fresh profile. This is not interactive browser scrolling or a web-framework score. |
| Compatibility | The bundled PE uses Win32 thread/event ping-pong, temporary-file write/flush/read and data validation. Wine plus the sibling PE are discovered automatically where installed. Uses a fresh private WINEPREFIX, then kills/waits only that prefix's wineserver and removes it. The first run includes prefix initialization; individual command runs are printed so it is distinguishable from subsequent runs. |

Cold reads require a positive cache-residency control on a known-warm file,
followed by successful `POSIX_FADV_DONTNEED` and zero resident pages reported
by mincore. The suite never drops the machine's global caches. Eviction is
checked again between sequential and random phases. Device/controller caches
may still be warm: these are verified OS-page-cache-cold reads, not a cold
physical device. Memory filesystems are skipped for cold reads.

AvoryOS currently returns success from fadvise without eviction and mincore
reports PTE presence instead of unmapped file-cache residency. The warm
positive control detects this and emits `SKIP`, preventing a false cold score.
Fsync timings measure the OS's completion contract; power-loss durability is
not independently verified by this suite.

Optional controls:

```sh
AVORY_BENCH_BROWSER=/usr/bin/chromium avory_bench browser 3 /home/me/scratch
AVORY_BENCH_COMPAT_RUNNER=/usr/bin/wine \
AVORY_BENCH_COMPAT_PROGRAM=/bin/bench_compat.exe avory_bench apps 120
AVORY_BENCH_TIMEOUT_MS=5000 avory_bench command 3 -- /path/to/latency-test
```

Chromium is discovered at common installed paths. Set
`AVORY_BENCH_BROWSER=none` or `AVORY_BENCH_COMPAT_RUNNER=none` to disable
those optional workloads. A root browser run is skipped unless explicitly
enabled with `AVORY_BENCH_BROWSER_NO_SANDBOX=1`, which adds `--no-sandbox`
only for that private local-page run. Commands are executed as argv vectors,
without a shell, and measure process exit, not arbitrary GUI readiness.
Their default timeout is 30 seconds, configurable from 10 to 120000 ms;
completion polling has 1 ms resolution. Failures/timeouts return nonzero.

Unavailable display/libraries/extensions/programs emit `status=SKIP` with a
reason. Execution failures emit `status=ERROR`, and the suite exits nonzero.
`status=COMPLETE errors=0` means requested available workloads finished; it
does not mean that every optional workload ran. Preserve skip records with
results. Do not compare software rendering to hardware GPU results, compositor
enabled to disabled, different displays/refresh rates, or different storage
mounts as if they were equivalent.

## Reading results

- **VFS:** 1000 create/close/stat/rename/unlink cycles report nanoseconds per
  cycle and cycles/second. fstime reports `COUNT|...|...|KBps` and its own
  elapsed time (`TIME|...`); retain both. These are buffered, warm-cache file
  workloads with 8 KiB buffers. They measure the selected filesystem and VFS,
  not cold-cache storage bandwidth. `-f` uses fsync on the test files instead
  of syncing the whole system, but the timed write loops do not fsync each
  write. `/tmp` and `/home` may exercise different filesystems.
- **CPU:** Dhrystone exercises integer, branch, string and procedure work.
  `COUNT` is completed iterations over the requested duration; divide by
  that duration for iterations/second. `METRICS` reports wall seconds, worker
  CPU seconds and CPU/wall percent (one fully occupied CPU is about 100%).
  Kernel scheduling accounting has 1 ms precision. User/kernel classification
  is sampled by the scheduler, not a cycle-accurate syscall profiler.
- **Process startup:** `fork_exec_to_main` measures from just before fork to
  the child's first timestamp in `main`, after exec and dynamic loader work.
  The child sends that timestamp through a pipe; the parent validates the
  handshake and exit status. `fork_exec_wait` includes child exit and reaping.
  Pipe creation and result printing are outside the timed interval. The
  executable is this small harness; other programs will have different costs.
  The first launch can warm caches; subsequent launches reuse them.
- **RAM:** separate snapshots show current RSS, virtual size, and system free
  memory before mapping, after mapping, after touching every page, and after
  unmapping. Times cover mmap, first touch and munmap. Eight verified streaming
  read/write passes report logical bytes accessed per second (reads plus
  writes), including cache effects; this is not a DRAM bus bandwidth score.
  RSS uses `/proc/self/status`'s page-table-backed count, including shared
  resident mappings, not `getrusage().ru_maxrss`. These are current snapshots,
  not a peak measurement. Missing `/proc` fields print `-1`.
- **Heavy iterations:** Hanoi with 16 disks performs 65535 recursive moves
  per completed iteration, reporting `COUNT` and worker CPU usage. Workload
  results remain observable; builds disable LTO and avoid native CPU flags.

CPU metrics exclude child processes. In particular the fork worker's CPU
percentage covers its parent loop, not the sum of its short-lived children.
AvoryOS does not yet accumulate `RUSAGE_CHILDREN`, and its legacy `ru_maxrss`
estimate is not used by these benchmarks. Process CPU clocks and getrusage
retain reaped group members' CPU time, and getrusage/times now use the existing
separate user/system counters instead of reporting all time as user time.

Record OS revision, CPU model, emulator/accelerator, vCPU count, guest RAM,
filesystem/mount, compiler flags, and workload arguments with results. Run
several times with the same environment and compare medians. These are
component benchmarks, not the official UnixBench index, and TCG performance
results should not be compared to KVM or hardware results.

## Verification

`python3 scripts/tests/test_userland_benchmarks_boot.py` runs an isolated guest
against the compiled kernel, checks sleep versus CPU time, group accounting
after thread reaping, and executes all workloads with small settings. It uses
KVM when available and TCG otherwise, a private filesystem image, and does not
open `disk.img`. Build the kernel first after accounting changes.

`python3 scripts/tests/test_desktop_benchmarks.py` runs native integration
checks for percentile/stall calculations against raw samples, verified IPC,
storage cleanup, false cold-cache claims, command argv preservation, failure
and timeout handling, and image install mappings.

`python3 scripts/tests/test_desktop_benchmarks_boot.py` runs the desktop suite
with a small sample count on a private ext4 guest and checks the cold-cache
positive control. The full boot test above now includes the default desktop
suite too. Graphics, fonts, browser and Wine need a sufficiently provisioned
graphical installation for full coverage. Native graphics/browser/Win32
validation logs are under `build/benchmark-results/desktop-native-*.log`;
guest checks are `desktop-private-smoke.log` and `desktop-full-boot.log`.

Protocol references: [GLX specification](https://registry.khronos.org/OpenGL/specs/gl/glx1.4.pdf),
[OML swap completion](https://github.com/KhronosGroup/OpenGL-Registry/blob/main/extensions/OML/GLX_OML_sync_control.txt),
[swap interval](https://wikis.khronos.org/opengl/Swap_Interval),
[page-cache advice](https://www.man7.org/linux/man-pages/man2/posix_fadvise.2.html),
[Fontconfig](https://www.freedesktop.org/software/fontconfig/fontconfig-user.html),
and [Chromium headless DOM dumping](https://developer.chrome.com/docs/chromium/headless).

## Full graphical comparison runner

After building `disk.img`, the private graphical runner uses a reflink of
`build/arch/disk-partition.img`, four KVM vCPUs, 4 GiB RAM, virgl, Xorg and
XFWM compositing. It never writes the desktop disk. `--host-chromium` stages
the installed host Chromium and its non-glibc dependencies only in that
private image so the browser build can match CachyOS. This disposable root
guest explicitly enables the local-page browser's no-sandbox option.

```sh
python3 scripts/tests/run_desktop_graphical_benchmark.py --samples 1000 --host-chromium
python3 scripts/tests/run_desktop_graphical_benchmark.py --core
python3 scripts/analyze-desktop-benchmarks.py \
  build/benchmark-results/desktop-cachyos-1000.log \
  build/benchmark-results/desktop-avoryos-1000.log \
  --output build/benchmark-results/desktop-comparison-1000.md
```

The runner extracts results even when a workload fails. A suite with an
execution error is not a pass; unaffected measurements remain in the log.
The analyzer preserves errors/skips, marks device-timer outliers that are
impossible relative to completion wall time as INVALID, and flags OML
timelines grossly inconsistent with wall intervals. It never rewrites raw
measurements or ranks unlike GPU/presentation/storage environments as an
overall OS score. Default font selections can also differ and are recorded.

Loaded-wakeup scheduler measurements and reproduction steps are in
[wakeup latency optimization](wakeup-latency-optimization.md). The graphical
runner accepts `--scheduling` to repeat only scheduling tests with Xorg and
the compositor active.


Graphics protocol `separate_queries_v2` separates GPU timer queries from plain
completion waits. The old combined phase could incur roughly 10 ms per frame
in Virgl's query/fence path; its completion metric is not comparable with v2.
`AVORY_BENCH_GPU_TIMER=0` disables the separate query phase for diagnosis.
`AVORY_BENCH_REQUIRE_COMPOSITOR=1` waits up to 60 seconds for the X compositing
manager selection and fails if it never appears; the private graphical runner
sets this to avoid scoring startup races as compositor performance.
The runner supports `--graphics`, `--display sdl|gtk|egl-headless`,
`--gpu-timer on|off`, and `--repetitions 1..10`. Frontend support depends on the
host. EGL-headless measures an offscreen path and cannot establish visible
frame pacing. See [graphics synchronization investigation](graphics-sync-investigation.md).

The retained filesystem optimization and matching before/after write tests
are documented in [journal transfer coalescing](journal-transfer-coalescing.md).

The next storage pass groups adjacent blocks within mixed journal batches; see
[mixed journal transfers](mixed-journal-transfers.md) for fresh paired results.
