# C1 shared graphics migration

Updated 2026-10-19, Europe/Istanbul.

C1's implementation and software validation are in the attached worktree
`/home/offihito/.codex/worktrees/nouveau-c1/AvoryOS`, branch
`codex/nouveau-c1`, based on `883a44c9cee338836b15b96db9eb8c5362ed1eec`.
C1 sources are also integrated into the active checkout, preserving its
existing edits, including the VCN tracing changes. The combined source has
a separately built `kernel/bin-x86_64/kernel-c1` candidate; the active
`kernel` and ISO have not been replaced. Nouveau is not imported or enabled by C1.

## Selected baseline and implementation

The default import is Linux **v6.12.113**, commit
`7fdb7c1824a1f9979d648d7ae28ac446bcee9c6e`. The version and commit are checked
in together in `scripts/linux/baseline.txt`. An explicit
`LINUX_VERSION=v6.6.156` still selects the previous baseline.

The importer stages and validates the complete subset before publishing it.
Failed generation preserves the previous import and pin. An interrupted
publication also restores the previous tree and pin. Optional manifest
entries cover sources split out in newer versions. Build profile stamps force
native, KPI and imported objects to rebuild when the pin or AMD gate changes.
Imported Linux sources remain pristine; adaptations live in tracked overlays,
bridges, compiler renames and the existing checked patch series.

The Kbuild evaluator handles the selected 6.12 arithmetic, renamed objects,
per-file FPU flags and removed base flags. The generated candidate manifest
contains 776 AMD objects and eight shared display-helper objects, compared
with 656 and eight on the retained 6.6 baseline. Nouveau has no object gate yet.

Compatibility work covers the DRM scheduler API, TTM eviction, dynamic
shrinker allocation/lifetime, grow-only `kvrealloc`, allocation-profiling
names, timekeeping headers, const bus descriptors, file cleanup helpers,
wait-variable queues, topology and the AMD VM diagnostic reference lifetime.
New dependencies include GPU buddy, DRM client helpers, video command-line
parsing and bounded string/formatting helpers. `firmware_request_nowarn`
shares the existing loader with warning suppression; the 16 MiB firmware
limit remains unchanged for the later Nouveau firmware milestone.

The sleep bridge now resumes an uninterruptible sleep after unrelated early
wakeups. The semaphore test checks credit consumption and worker publication
rather than timing measured after a worker may already have run. Scheduler
test objects are initialized before the newer initialization API examines
them. The one-block EDID fixture no longer advertises an absent extension;
the modern validator receives an explicitly bounded EDID object.

Native-only builds exposed existing unconditional Linux bridge dependencies.
Without an import, native nodes cannot identify as Linux files, and native
`mprotect` retains its CPU pin through the deferred TLB drain using a native
preemption counter. Nested pins, IRQ restoration and injected migration are
covered by an actual-code host regression.

### UUID-only identifier bytes

The selected AMD/DisplayPort GUID path needs UUID generation. Only
`lib/uuid.c` is compiled with
`get_random_bytes=linuxkpi_guid_bytes`. The adapter prefers bounded RDRAND
attempts and has a sequence/time-mixed fallback for identifiers. It does not
provide cryptographic secrecy or an entropy guarantee. The final AMD image
contains `guid_gen` and `linuxkpi_guid_bytes`, and no `get_random_bytes` symbol.
Future cryptographic callers therefore still fail to link rather than silently
use this fallback. General secure randomness remains a separate KPI gap.

## Software evidence

Evidence and private kernels are under the worktree's `build/c1/` directory.
No fixture assigns a physical GPU. AMD-enabled kernel boots use
`kpi_amdgpu=0`, which verifies the compiled driver gate and shared stack,
not AMD hardware initialization.

| Check | Result | Evidence under `build/c1/` |
|---|---|---|
| Native-only kernel build | Pass | `logs/native-build-2.log`, `native-kernel` |
| Linux 6.12 shared-only build | Pass | `logs/shared-final-6.12-build.log` |
| Linux 6.12 full AMD build/link | Pass | `logs/amd-6.12-resume-build.log`, `amd-6.12-kernel` |
| Retained Linux 6.6 shared-only build | Pass | `logs/shared-6.6-build-2.log` |
| Retained Linux 6.6 full AMD build/link | Pass | `logs/amd-final-6.6-build.log` |
| 6.12 shared boot suites and rendering oracle, one/eight CPUs | Pass in strict runs | `logs/final-shared1.log`, `logs/final-shared8.log` |
| 6.12 AMD-enabled kernel, strict software boots, one/eight CPUs | Pass | `logs/amd-6.12-software1.log`, `logs/amd-6.12-software8.log` |
| Imported DRM userland GEM and atomic modesetting | Pass; all four stages recorded | `logs/amd-6.12-drm8.log` |
| Userland dma-buf tests | Test program passes; that run fails strict boot accounting | `logs/userland-dmabuf.log` |
| Native VirtIO GEM and atomic modesetting, one/eight CPUs | Pass; all four stages recorded | `logs/native-drm-final1.log`, `logs/native-drm-final.log` |
| Final shared-only eight-CPU repeat | Rendering passes; strict boot fails VKMS accounting | `logs/shared-final8.log` |

The DRM test retains a four-bit result for atomic enable, page-flip event,
vblank wait and atomic disable. It reports `0xf` after closing the display
file, when native console ownership releases serial output. A missing target
card fails instead of producing an all-skipped success. The render node remains
optional for a native display-only fixture. This is a GEM/KMS fixture, not a
full compositor, desktop workload, host-pixel comparison or performance test.

Fourteen focused host test programs pass: DMA masks, completions, MMIO ordering,
RCU lifetime, PCI access lifetime, VMAP lifetime, actual AMD per-CPU/FPU storage,
TTM eviction, migration semantics, GUID identifier bytes, native preemption,
Kbuild migration, import transaction rollback and boot result classification.
The retained 6.6 TTM and AMD per-CPU tests also pass. New checks cover early
wakeups, heap/vmap reallocation failure and zeroing, bounded formatting,
concurrent shrinker teardown, registration exhaustion and already-exited VM
faults. These host models complement the real guest scheduler checks.

The boot helper now supports explicit ISO/kernel/oracle inputs, CPU count,
output directory and static-init compiler. It supplies the firmware test
fixture, uses disposable disk overlays and can fail on boot self-test errors.
Kernel thread faults and panics fail even if an oracle returns zero or QEMU
has already exited. Earlier writable fixture disks became corrupt between
runs; their results are not treated as valid clean-baseline comparisons.

## Remaining gates and limits

1. **Physical AMD smoke test is user-reported passing (2026-10-19).**
   The user launched `make run-c7-prime`, opened the desktop, ran games on
   AMDGPU and ran `glxinfo`. This supplies the C1 hardware smoke-test gate.
   No renderer string or serial capture was supplied for independent review.
   Long-session, clean-shutdown and the previously affected workload/reset
   regressions remain limits; this report does not claim they are fixed.
2. **VKMS accounting is intermittently failing.** The preserved baseline and
   candidate fixtures both report warm-loop PMM drift. Candidate rendering and
   DRM/dma-buf programs can return zero during such a failed boot. Strict
   fixture failures remain failures; selected passing runs do not establish
   that this issue is fixed or that every drift magnitude has the same cause.
3. Full desktop/compositor and affected AMD workload checks have not been run
   in C1. The native fixture evidence above is limited to GEM/KMS operations.
4. The active checkout's combined source builds successfully as
   `kernel/bin-x86_64/kernel-c1`; `build/nouveau-c1-main-build.log` records
   the build. Its strict one- and eight-CPU software boots pass in
   `build/nouveau-c1-integrated1.log` and `build/nouveau-c1-integrated8.log`. The active import is pinned to 6.12.113.
   Keep the current boot artifacts until the separate hardware/workload
   regression gate is satisfied.

C1 has substantial build and boot evidence, but the migration is **not yet
certified regression-free**. C2's [Nouveau import and independent controls](c2-import.md) subsequently
started; real guest registration awaits C3 compilation/link work. Physical NVIDIA initialization, GSP loading,
NVK packaging and support for additional GPU families remain later work.

## Reproduce the software check

From the C1 worktree, using existing local caches:

```sh
LINUX_SRC_DIR=/home/offihito/AvoryOS/build/nouveau-c0/linux-candidate scripts/linux-import.sh
make -C kernel -j8 KPI_AMDGPU=1
python3 scripts/gpu-pipeline-boot.py --software --run --strict-boot-tests \
  --cpus 8 --output-dir build/c1/recheck \
  --iso /home/offihito/AvoryOS/avoryos-x86_64.iso \
  --kernel kernel/bin-x86_64/kernel \
  --oracle /home/offihito/AvoryOS/userland/test_gpu_pipeline.elf
```

The command uses a private root disk and copied ISO. It does not initialize
the physical AMD or NVIDIA GPU. The worktree
`build/c1/evidence.json` and active-checkout `build/nouveau-c1-evidence.json`
record the pins, kernel hashes and principal result markers for the saved
evidence.

The local source-index refresh was attempted after integration but failed on
the existing `docs/amdgpu-testing.md` txt-lines/txt-prose family mismatch.
Source checks used bounded reads and exact searches instead.
