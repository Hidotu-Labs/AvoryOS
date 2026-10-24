# RadeonSI / TTM memory lifetime diagnostic

## Historical tunnel trace results

A recovered guest trace contained 1,154 frames (72,767,462 bytes). It replayed
on host NVIDIA and software renderers. Sampling 116 frames from each replay
found no conspicuous green pixels in the gray corridor; this did not check every
pixel or frame. Headless host replay emitted an Xlib warning but produced the
expected images. The AMD guest replay remains unverified; an earlier guest run
fell back to llvmpipe after a VirtIO DRI3 failure, so its output was not AMD
validation. Tracing and replay can change timing.

Build with `make userland/radeonsi_vram_leak.elf`. The executable links the
same guest EGL, GBM, GLES and libdrm libraries as `test_kpi_radeonsi`; it uses
`/lib64/ld-linux-x86-64.so.2`. It makes no kernel or Mesa changes.
`make gpu-diagnostics` also stages it in `build/gpu-diagnostics`.
Future disk image builds install it as `/bin/radeonsi_vram_leak.elf`.

Copy the ELF into the running guest and run it as a user with render-node
access. The build alone does not update `disk.img`. With the usual QEMU user
network, one transfer option is:

Host:

```sh
mkdir -p build/gpu-diagnostics
install -m 0755 userland/radeonsi_vram_leak.elf build/gpu-diagnostics/radeonsi_vram_leak.elf
python3 -m http.server 9007 --bind 127.0.0.1 --directory build/gpu-diagnostics
```

Guest:

```sh
wget -O /tmp/radeonsi_vram_leak.elf http://10.0.2.2:9007/radeonsi_vram_leak.elf
chmod +x /tmp/radeonsi_vram_leak.elf
/tmp/radeonsi_vram_leak.elf --cycles 20 --mib 64 > /tmp/vram-leak.log 2>&1
rc=$?
cat /tmp/vram-leak.log
printf 'exit=%s\n' "$rc"
```

Close Minecraft and other 3D workloads for the first run. Ideally run from a
console with the desktop stopped, then repeat under Xorg/Plasma: counters
include all processes and the kernel, so compositor activity can obscure the
trend. Preserve the guest serial/kernel log, especially on a panic. Each line
is flushed immediately, so a partial log still identifies the last phase.

## Phases

Default `--phase all` runs these phases in order, stopping at the first runtime
or content failure:

| Option | Workload | What it isolates |
| --- | --- | --- |
| `raw-vram` | 4 MiB CPU-accessible VRAM GEM objects, full CPU pattern write/read | VRAM allocation, mappings, release without Mesa |
| `raw-gtt` | Same workload in GTT | System pages, GTT and mapping lifetime |
| `raw-prime` | VRAM objects exported as dma-bufs, imported on a separate AMD DRM file | GEM handles, dma-buf references and DRM file teardown |
| `gl` | 1024×1024 RGBA8 textures, patterned uploads, shader sampling and full readback | RadeonSI texture allocation, upload, GPU access and deletion |
| `prime` | Linear GBM BOs exported to EGL dma-buf images and bound as GL textures | Mesa/GBM/EGL image and dma-buf lifetimes |

Raw phases alternate explicit `GEM_CLOSE` and implicit handle release on DRM
file close. All mappings are unmapped first. `raw-prime` closes the original
and imported DRM files while exported dma-buf fds still exist; elevated usage
at `dma_fds_only` is expected. The `released` sample follows closure of those
fds. The importer uses the same AMD GPU; it does not exercise the virtual
display driver.

GL phases keep a single context alive through repeated allocation/deletion
cycles. Every batch is filled with a per-coordinate, per-object, per-cycle
pattern; the whole batch remains live until all objects have been checked.
The shader uses exact texel coordinates, with blending and dithering disabled.
The checks are synchronized with readback/`glFinish`, which helps isolate
lifetime and content failures but can hide bugs requiring asynchronous overlap.
A final `context_destroyed` sample follows GL object deletion, EGL context and
display teardown, GBM destruction and DRM file close.

The PRIME phase requires linear ABGR8888 renderable BOs and EGL dma-buf
import. An unsupported allocation/import is reported as an error or incomplete
phase, not interpreted as a leak. These are headless AMD tests: neither QEMU
scanout nor Xorg DRI3 cross-driver imports are exercised. Use the existing
`test_gpu_pipeline` and `test_kpi_radeonsi` display-sharing diagnostics if these
phases stay clean while the displayed image corrupts.

## Reading results

`MEM` is tab-separated:

```text
MEM phase cycle stage monotonic_seconds vram_bytes gtt_bytes visible_vram_bytes memavailable_bytes
```

`-1` means an unavailable counter. `HEAPS` reports capacities if supported.
`CHECK` reports content mismatches; a few first differing values are printed
as `MISMATCH`. `TREND` compares the last post-release sample against the first;
`DELTA` compares it against the pre-phase baseline. `TEARDOWN` separately
compares the sample after context destruction against that baseline. A missing
counter's delta is printed as zero and is not evidence of stable usage.

Interpret all cycle samples, not just the final difference:

- A plateau after initial growth can be normal Mesa caching or kernel page pools.
- Repeated growth in `released` samples, especially when it survives teardown,
  is a retention candidate. Repeat with a longer settle interval and idle clients.
- Growth in a raw phase narrows the problem below Mesa. VRAM and GTT placement
  can still change through eviction, so track both counters and system memory.
- Growth limited to `raw-prime` or `prime` points toward exported/imported BO or
  image lifetime paths, rather than basic GEM allocation alone.
- Pixel mismatches with stable allocation counters make a leak alone an
  insufficient explanation; investigate mappings, synchronization and contents.
- A clean headless run leaves cross-driver PRIME, asynchronous rendering,
  display presentation and other workloads untested.

Exit codes: `0` means the requested workload completed without observed API or
pixel failures; it **does not certify absence of a leak**. `1` means a runtime
or content error. `2` means invalid arguments, no AMD node, wrong renderer,
missing VRAM/GTT counters or an incomplete required feature.

## Follow-up runs

```sh
# Isolate an interesting phase for more cycles and allow deferred frees time.
/tmp/radeonsi_vram_leak.elf --phase raw-vram --cycles 100 --mib 64 --settle-ms 3000
/tmp/radeonsi_vram_leak.elf --phase gl --cycles 100 --mib 64 --settle-ms 3000
/tmp/radeonsi_vram_leak.elf --phase prime --cycles 100 --mib 64 --settle-ms 3000

# Software pixel-oracle control only; no GPU allocation/leak conclusion.
/tmp/radeonsi_vram_leak.elf --software --cycles 3 --mib 16 --settle-ms 0
```

The default live payload is 64 MiB; raw objects are 4 MiB each and textures are
4 MiB each. Driver metadata, page tables, staging allocations, PRIME layout
padding and the 4 MiB output texture add overhead. VRAM placement is requested,
not guaranteed in the presence of eviction. `--mib` accepts multiples of 4 from
4 to 1024. This is a bounded churn test, not a deliberate OOM/eviction test.
`--node /dev/dri/renderD129` overrides AMD render-node discovery when needed.

Return the full log, process exit status, GPU model/VRAM size, whether the
desktop was running, and the kernel OOM/panic lines with their backtrace. Those
results determine whether the next investigation should target TTM/page
lifetime, Mesa/dma-buf ownership, or the presentation/synchronization path.

## Guest evidence, 2026-10-13

The supplied X11 run completed the raw VRAM, GTT and PRIME churn phases with
zero content mismatches. Released usage fluctuated or plateaued; the visible
samples did not establish a sustained allocation leak. The final GL teardown
and overall result were not readable in the supplied damaged screenshots.

`test_kpi_radeonsi` also passed with `AVORY_GPU_TEXTURE_STRESS=1`: 768 MiB of
textures, 240 checked frames / 62,914,560 pixels including queued draw/reuse,
both fog cases, and 64 AMD-produced PRIME frames with matching GPU/native CPU
contents. These successful workloads do not certify all application rendering.

An original Minecraft F2 PNG was transferred from the guest and decoded on the
host, independently of guest PRIME presentation. It contains two conspicuously
green pixels at zero-based coordinates (318,275) and (319,275), both RGBA
(66,255,17,255), on the ground above the stone. Nearby (317,275) and (320,275)
are (67,43,17,255) and (67,46,17,255). This confirms the reported spot is present
in the saved file, rather than being introduced only by its guest image viewer
or QEMU scanout. It does not distinguish rendering corruption from framebuffer
readback corruption, or establish a particular kernel/Mesa/hardware cause.

Next hardware check: `test_gpu_pipeline`, which compares textures/mips and
uniform/storage reads with CPU oracles and includes overlapping-work and
serialized controls. The staged endpoint `/test_gpu_pipeline` on the host's
port 9007 was verified available; transfer it while the VM remains running.

The guest's GLES pipeline run subsequently passed with `failures=0` and
`inconclusive_overlap_cases=0`: each texture case checked 2,097,152 pixels;
reused UBO uploads observed a pending previous fence in 64/64 trials, and
fresh-descriptor uploads in 62/64 trials. Uniform reads, storage reads, CPU
source checks and heavy-work checks all matched. The serialized control also
matched. Thus overlap really occurred, but this workload did not reproduce
the Minecraft corruption.

`test_gpu_pipeline_gl` now runs the same CPU oracles through a desktop OpenGL
4.3 core EGL context, linked against libGL, with GLSL 430 shaders. It is a
comparison against the GLES path, not a Minecraft/GLX reproduction. Both
variants passed the local llvmpipe harness; the desktop variant's hardware
run subsequently also passed under RadeonSI / desktop OpenGL 4.6 core with
`failures=0` and `inconclusive_overlap_cases=0`. Reused and fresh-descriptor
cases each observed pending previous fences in 64/64 trials; the serialized
control observed 0/64. Texture, uniform, storage, CPU-source and heavy-work
checks all matched. Neither API variant reproduced the saved-image artifact.

The next evidence needed is a flight-recorder capture during visibly affected
Minecraft rendering, using the already installed
`/usr/bin/gpu-debug-capture 5 15 2`. This freezes and exports the capture to
the host serial log and saves a guest copy. A record captured from a passing
synthetic workload cannot identify the failing Minecraft operation. An
actual OOM/panic backtrace is still needed to investigate the reported
long-running Xorg exhaustion; it has not been reproduced by these tests.

## Minecraft flight capture and abort

The subsequent real-workload capture reproduced an ENOMEM submission failure.
In `build/logs/p6-c7-prime.log`, allocation failures at lines 2960–2967 show
orders 9 through 6 and reported free pages declining from 3,207 to 1,995.
These correspond to about 12.5 to 7.8 MiB of free 4 KiB frames, not an
accounting of potentially reclaimable cache. The high-order failures alone
cannot distinguish fragmentation from exhaustion. AMDGPU then reports
`Not enough memory for command submission!` at line 2968. Its source emits
this message when BO-list processing returns ENOMEM. Mesa reports a rejected
CS with -12 and the `java:cs0` worker receives SIGABRT; the user backtrace
contains libgallium frames. This is a userspace abort following a kernel
allocation/submission failure, not evidence of a kernel panic or Java exception.

The frozen recorder header reports 59,048 events, 57,000 overwritten ring
entries, 798 copies/presentations and 6,311 cache checks. All 64 retained pixel
checks have equal before/staging/after hashes; source_changed,
stable_copy_mismatch, cache_mismatch and anomalies are zero. The pixel samples
span only 1.513 seconds at the start of the approximately 15-second capture.
They establish matching sampled copies, not correct game rendering or that
the crash-time copy was sampled. The serial dump lacks its end marker, so the
full trailing event export should not be assumed complete. A focused crash
extract is saved as `build/gpu-diagnostics/minecraft-crash-evidence.txt`.

This establishes physical allocation pressure and CS rejection during the
affected workload. It does not establish a leak, identify the memory owner,
or link the saved green pixels to that pressure. Investigate live process,
native file-cache and TTM-pool accounting, reclaim and contiguous allocation
availability before changing policies. High-order TTM allocations deliberately
use NORETRY and fall back to smaller orders; those fallbacks must not be
mistaken for independent allocation leaks.

Follow-up, 2026-10-15: the allocation-failure and reclaim gaps behind this
crash class are addressed in
[memory pressure cleanup](memory-pressure-cleanup.md) (LinuxKPI failure
relief with parked-frame drains and bounded file-cache reclaim, plus
free-list tags for the reboot-time dropped frees). The high-order TTM
fallbacks were confirmed to carry `__GFP_NORETRY|__GFP_RETRY_MAYFAIL` and
now get a cheap parked-frame drain before their order fallback continues. A
repeat of the real workload at 4 GiB is the acceptance test.

## Pixels persist after the lifetime fixes

The user reports the pixel artifacts still occur on the updated kernel. The
settled syscall repeat results are clean for WAIT4, SCHED, CLONE/FUTEX,
ACCEPT4 and UNIX STREAM; those results do not resolve the graphics fault.

The earlier texture/pipeline oracles explicitly disabled depth testing.
`test_gpu_depth` now checks D16, D24, D32F and D24/S8 renderbuffers at 1x and
4x MSAA. It compares every color pixel after occluded/front draws, alternating
normal/reversed depth, stencil rejection, partial depth/stencil clears, and
renderbuffer storage reallocation every 16 frames. Hardware mode uses 128
frames per case (67,108,864 pixels total if all eight cases complete). It is
still headless EGL desktop GL and uses per-frame readback, not GLX presentation
or unrestricted inter-frame overlap. It cannot by itself identify whether an
observed failure comes from Mesa, LinuxKPI or hardware.

The llvmpipe control passes all eight cases over 18 frames each, including the
storage-reallocation boundary (9,437,184 pixels). A compiled negative control
with depth disabled reports mismatches in all eight cases and exits 1.
The binary builds without warnings and is staged as `/test_gpu_depth` on
host port 9007. `check-depth.sh` downloads it, saves the log once (no append),
prints results and uploads the log to the temporary host `/depth-log` receiver.
No kernel/disk/ISO changes are required to run this diagnostic. Hardware
results remain pending.

The hardware depth oracle subsequently passed all eight cases on RadeonSI /
desktop GL 4.6: 128 frames and 8,388,608 pixels per format/sample case,
67,108,864 pixels total, zero mismatches, zero incomplete cases. This includes
partial clears, reversed depth, stencil rejection and repeated storage
reallocation. The uploaded log is `build/gpu-diagnostics/gpu-depth-guest.log`.
These workloads have still not reproduced the application's saved-image
corruption. The next reproduction should use a small affected application
and its actual GLX/window path, rather than treat another passing headless
oracle as evidence that application rendering is correct.
