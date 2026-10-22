# C3 Nouveau compile/link compatibility

Updated 2026-10-19. C2 is complete: Nouveau imports independently, its real
PCI driver registers, and the default closed gate makes zero probe attempts.
C3 closes the compile/link gaps and supplies targeted software evidence.
This is not evidence that NVIDIA hardware initializes or renders.

The selected import is Linux v6.12.113, commit
`7fdb7c1824a1f9979d648d7ae28ac446bcee9c6e`. Imported sources remain pristine;
implementations live in tracked overlays/native bridges, or are imported
upstream algorithms. The retained v6.6.156 AMD profile also links.

## Implemented behavior

| Area | Implementation and evidence |
|---|---|
| Range trees | Import upstream `lib/maple_tree.c`, initialize its caches before clients, expose the stock types and repair RCU macros that incorrectly evaluated lockdep-only conditions. Guest test splits nodes across 96 disjoint ranges, checks gaps, erases and destroys the tree. |
| GPU virtual addresses | Import upstream `drm_gpuvm.c`, select GPUVM, and test actual GEM/VM references, reserved ranges, overlap/overflow rejection and exactly-once release. Existing DRM exec/scheduler/syncobj suites continue to run. |
| Native slab | Align the first object after page metadata, not just the object stride. Maple nodes require 256-byte alignment for tagged pointers. The original layout faulted in a real guest; the corrected layout boots. Sanitizer tests cover alignment 8–2048, capacity, free/reuse/shrink, invalid sizes and an old-layout negative control. |
| Slab helpers | Bulk cache allocation rolls back every partial allocation on failure; generic bulk free accepts a NULL cache; bounded `kstrndup` copies and terminates correctly. Allocation flags and failures are exercised. |
| DMA/SG | Noncoherent allocations use the streaming mask; coherent allocations retain the coherent mask. Low masks select the low zone and reject ranges outside the mask with cleanup. `sg_virt` resolves CPU-addressable native pages. Tests cover differing masks, overflow and failure unwind. DMA remains identity mapped on x86. |
| Timed waits | `schedule_hrtimeout` handles absolute/relative/expired/null deadlines, rounds native sleeps up to milliseconds, returns interruption after an early wake and restores TASK_RUNNING. No high-resolution timing guarantee is added. |
| Legacy MSI | Allocate exactly one MSI, publish its vector through `pdev->irq`, propagate errors, and restore the original INTx identity on either teardown path. A QEMU EDU guest delivers a real virtual interrupt through this API. |
| Header/API contracts | Restore stock kref visibility, firmware ELF metadata, x86 user address limit, ACPI handle shape, mutex initialization, GFP/object allocation helpers, workqueue constants and disabled MMU-notifier layout. Firmware metadata is version guarded for the retained 6.6 profile. |
| Optional facilities | Import stock DRM encoder-slave code, but I2C client-driver registration returns `-EOPNOTSUPP` and client creation fails. Existing DDC adapters/transfers remain supported. Native component binding also returns `-EOPNOTSUPP`; device links return NULL. No fake audio/client binding is published. |
| Isolation | The actual Nouveau registration check records both gate state and whether its probe was attempted. Build/probe defaults remain off. Build profile identity also distinguishes a native-only build with no imported objects. |

The C2 inventory's 52 distinct diagnostics (858 occurrences) was the initial
checkpoint, not 52 independent implementations. Restoring missing common
headers removed most repetitions; subsequent builds exposed additional link
requirements including GPUVM, encoder-slave, string allocation and legacy MSI.
No compiler diagnostic was suppressed to make Nouveau link. Existing build
warnings remain; successful build does not mean warning-free source.

## Builds and tests

Candidate builds use `build/nouveau-c3-kernel.make` and output `kernel-c3`.
The active `kernel` and boot ISO were not replaced. Saved kernels, logs and
hashes are indexed by `build/nouveau-c3-evidence.json`.

| Profile | Result |
|---|---|
| Native, imported object list empty | Links; eight-CPU VirtIO GEM/modeset oracle exits 0, modeset mask `0xf`. |
| Shared DRM, both driver gates off | Links. |
| AMD only, Linux 6.12.113 | Links. |
| Nouveau only, Linux 6.12.113 | Links all 715 selected Nouveau objects plus shared helpers. |
| AMD and Nouveau, Linux 6.12.113 | Links both complete drivers and shared display helpers once. |
| AMD only, retained Linux 6.6.156 | Links; GPUVM test is version guarded because that source is absent there. |

Software boot evidence (no physical PCI assignment):

- `build/nouveau-c3-nouveau-final8.log`: strict eight-CPU boot passes;
  maple/GPUVA tests and real registration with zero probes pass. The software
  rendering oracle exits 0. Its overlap stress cases are explicitly inconclusive
  where no pending earlier fence was observed; this is not NVIDIA rendering.
- `build/nouveau-c3-nouveau-final1.log`: one-CPU oracle exits 0 and C3 tests pass,
  but strict boot fails on the previously observed VKMS one-page PMM drift.
- `build/nouveau-c3-both-edu8.log`: both drivers linked, C3 checks and actual EDU
  legacy MSI delivery/INTx restoration pass. Existing 10,000-interrupt stress
  passes. Oracle exits 0, but strict boot fails on the same VKMS accounting drift.
- `build/nouveau-c3-final-both-edu8-v2.log`: the final combined candidate passes
  strict eight-CPU boot, all new C3 checks, EDU legacy MSI delivery/restore and
  the software oracle. Its exact kernel hash is recorded in the evidence JSON.
  An earlier `final-both-edu8` run started before the build completed and used
  the saved pre-final candidate; it is retained as such in the evidence.
- `build/nouveau-c3-native-drm8.log`: native VirtIO DRM oracle passes after the
  allocator correction, including close/teardown and all modeset stages.

The first Nouveau guest faulted in maple-tree traversal because the native slab
ignored the first object's requested alignment. This was investigated and fixed;
subsequent guest runs and the negative-control host regression verify the fix.
The intermittent VKMS drift is still open. Failed strict boots are not counted
as passing boots, even when their new C3 checks pass.

Host tests pass: new C3 helper and native slab regressions; Nouveau gate and
parameter namespace regressions; inspection of the final combined ELF confirms
124 unique qualified parameters with distinct DRM/Nouveau debug types; four-way Kbuild selection; transactional import;
DMA masks, completion, RCU lifetime, PCI lifecycle, IRQ synchronization, workqueue
requeue, timer teardown, firmware pin, VMAP lifetime, per-CPU storage and TTM
eviction. ASan/UBSan exercise allocation and helper regressions where applicable.
These tests cover behavior, not just presence of symbols.

The requested Xerj refresh still fails on the existing
`docs/amdgpu-testing.md` document-family mismatch. Investigation used bounded
current-source reads and exact searches instead of the stale index.

## Boundary before C4

C3 compile/link work and targeted software validation are complete, including
a passing strict both-driver boot. Whole-boot qualification retains the
intermittent VKMS failure from earlier runs; there is no new unexplained C3
fault. Physical AMD validation after these changes is pending. The earlier
C1 desktop/games/glxinfo result is user reported and is not reused as C3 evidence.

Before NVIDIA probing, C4 must address and validate:

1. The 16 MiB firmware loader bound versus the selected roughly 38 MiB GSP image,
   exact staging and firmware failure handling.
2. A dedicated explicit-BDF launcher, isolated host handoff, GSP/RPC/IRQ tracing,
   and failure cleanup without changing the default closed gate.
3. Inherited runtime limits: `orderly_poweroff` and `emergency_restart` are still
   no-ops; thermal shutdown is not implemented. The strong cache-flush fallback
   flushes only the current CPU despite its `wbinvd_on_all_cpus` name; its reachability
   and any required SMP implementation must be resolved for the tested hardware.
4. Actual hardware DMA ordering, GSP memory mappings, interrupts, teardown and
   reset. A software-only boot cannot establish these contracts.

SVM/HMM, guest IOMMU translation, platform/Tegra, runtime PM, Linux ACPI laptop
integration and optional backlight remain disabled. The UUID-only random adapter
is still not a generic cryptographic random provider. No NVIDIA card was bound,
reset or probed, and no host display device was disturbed by this work.
