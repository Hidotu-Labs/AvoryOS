# LinuxKPI AMDGPU stability audit — 2026-10-13

Verified compatibility defects were corrected. The physical host reboot has
**not** been reproduced, attributed or demonstrated fixed. No physical GPU,
VFIO reset-method, host bridge, PSP firmware or host power-management tests or
settings changes were performed.

The working tree already contained changes in `irq_work.c`, its lifetime test,
GPU coherency code, Xorg configuration and build/setup files. Those changes were
preserved. In particular, callback self-free/requeue support predates this audit;
this audit fixes synchronization around that behavior. Imported Linux/AMDGPU
sources were not edited.

## Scope and comparison

The import manifest pins Linux **v6.6.156**, commit
`8b73de7da85fde281a385e0b26eda9bffd3ca477`. Local imported call sites and headers
were inspected; the compatibility semantics were also compared with the
[upstream IRQ core](https://raw.githubusercontent.com/torvalds/linux/v6.6/kernel/irq/manage.c),
[irq_work implementation](https://raw.githubusercontent.com/torvalds/linux/v6.6/kernel/irq_work.c)
and [PCI implementation](https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/pci/pci.c).
The online comparison uses the 6.6 base tag, not a claim that every later stable
implementation is identical. Upstream distinguishes hard-handler synchronization
from draining threaded handlers, preserves a busy claim across irq_work requeue,
and restores PCI capability state with BARs before Command. These guide the
supported shim behavior; this is not a complete port of the Linux core.

Locations below refer to the **corrected working tree**, so each reference leads
to the implementation containing the fix. No finding below has direct evidence
connecting it to the physical host reset. “Plausible” describes guest execution
consequences, not an established cause of the reported incident.

## Findings and applied changes

| Classification | Location | Defect, correction and affected path | Plausibility for AMDGPU initialization |
|---|---|---|---|
| CONFIRMED BUG | [irq.c:249](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:249), [irq.c:299](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:299) | `synchronize_irq()` omitted pending/running threads; hard synchronization incorrectly always returned true. Count queued/running invocations under the descriptor lock, including removed actions. Full sync waits; hard sync only drains hard dispatch and reports thread activity. `disable_irq()` uses full sync. | Can permit guest teardown races. Main AMDGPU IH registration uses a hard handler, so threaded synchronization is not a demonstrated direct cause on that path. |
| CONFIRMED BUG | [irq.c:58](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:58), [irq.c:97](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:97) | ONESHOT had no threaded masking state. Keep supported MSI/MSI-X vectors masked across queued/running threaded invocations and explicit disable nesting. Cache mask state to avoid config writes on every normal interrupt. Reject ONESHOT without hardware masking. | Relevant for threaded IRQ users; main AMDGPU IH does not request ONESHOT. |
| HIGH-CONFIDENCE DEFECT | [irq.c:55](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:55), [irq.c:68](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:68) | Unsynchronized first-use initialization could reset a live descriptor lock; mask function/data publication/removal was also unlocked. Use valid static zero-initialized locks and serialize the mask registry with dispatch. | Concurrent registration/vector teardown could corrupt guest IRQ lifetime. No observed reproduction on physical GPU. |
| CONFIRMED BUG | [irq.c:150](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:150), [irq.c:203](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:203) | A SHARED request could attach to an exclusive action. Enforce shared agreement, non-null shared dev_id and matching ONESHOT flags. Free unlinks under lock, masks an empty descriptor and drains queued/running work before freeing. | Registration/teardown correctness; not evidence of the host reboot. |
| CONFIRMED BUG | [irq_work.c:113](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq_work.c:113) | Separate queued/current loads could read queued=0, then miss a callback's requeue when current became NULL. Sample both under the existing queue lock; clear the side running record under that lock. Never dereference the work after callback return. | No imported `irq_work_sync()` caller was found, so this particular sync defect does not explain current initialization directly. Fence-array/chain callback lifetime and requeue remain important. |
| CONFIRMED BUG | [pci.c:694](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:694), [pci.c:745](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:745) | Power-state changes and device enable invented D0/D3 success without transitions. Initialize current state from PMCSR where available; reject unsupported transitions and enabling D3/unknown devices without changing hardware or cached state. Choose D0; reject unsupported wake enable. | Power requests occur in switcheroo/runtime-PM paths, not ordinary initial probe. CONFIG_PM/ACPI are absent, limiting reachability. Device enable is on probe, and now exposes an actual non-D0 state as failure. |
| CONFIRMED BUG | [pci.c:1280](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:1280), [pci.c:1340](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:1340) | Restore replayed all header dwords, including adjacent W1C Status, and Command before BARs. Save/read errors are now checked; restore is restricted to normal endpoint writable fields, disables decoding/mastering, restores BARs and supported capabilities, and restores Command with a word write last. Store/load retains capability snapshots. Save PCIe DEVCTL/DEVCTL2 and MSI address/data/mask/control; exclude FLR. Active MSI-X/bridge snapshots fail explicitly. | Directly reached by `amdgpu_device_init()` at the end of successful initialization via cache/restore. Clearing diagnostics and restoring stale routing/decoding are plausible guest failures; host-reset attribution remains absent. |
| CONFIRMED BUG | [initcalls.c:34](/home/offihito/AvoryOS/kernel/linuxkpi/src/initcalls.c:34), [kernel.c:873](/home/offihito/AvoryOS/kernel/src/kernel.c:873) | After 600 seconds boot could start tests/services while the probe thread still owned hardware. Keep the dependency barrier until completion, retain progress logs and join afterward. No forced termination. Failed initcalls and PCI probe results continue to be logged by the existing walker/registry. | Directly allows initialization/test contention if probe exceeds the deadline; fixed without imposing a shorter deadline. |
| CONFIRMED BUG | [dma-mapping.c:19](/home/offihito/AvoryOS/kernel/linuxkpi/src/dma-mapping.c:19), [dma-mapping.c:165](/home/offihito/AvoryOS/kernel/linuxkpi/src/dma-mapping.c:165), [dma-mapping.h:126](/home/offihito/AvoryOS/kernel/linuxkpi/include/linux/dma-mapping.h:126) | `dma_map_sg()` returned a count without populating DMA fields; map routines ignored device masks/range overflow; multi-page vmalloc single mapping could describe noncontiguous RAM as contiguous. Populate and validate SG address/length, roll back fields on failure, set mapped nents, reject vmalloc single mapping and invalid ranges. Validate coherent masks/allocation bounds. SG sync now performs the existing CPU barrier/WC store drain. | DMA page/coherent/SG-table APIs are reached by GART, IH and TTM/import paths. Invalid DMA addresses can corrupt guest memory. Raphael's requested masks may avoid some range failures; no particular observed fault was attributed. |
| CONFIRMED BUG | [workqueue.c:223](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:223), [workqueue.c:258](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:258) | Flush had the same split-observation problem; cancellation could wait for a running callback and leave its new pending invocation linked. Sample under the queue lock and recheck/remove requeues after each running instance. Cancellation reports whether pending work was removed. | GPU reset/teardown/late-init work can otherwise outlive owners and signal/use stale data. |
| CONFIRMED BUG | [workqueue.c:245](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:245), [workqueue.c:327](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:327) | Delayed flush ignored an armed timer; nonblocking delayed cancellation ignored work already queued by its timer; sync cancellation's return ignored queued work. Flush now drains its timer and queues immediate execution; cancellation handles both phases. | AMDGPU delayed_init_work, gfx-off and hotplug flush paths are affected. Scheduler TDR cancellation uses nonblocking delayed cancellation. |
| CONFIRMED BUG | [timer.c:99](/home/offihito/AvoryOS/kernel/linuxkpi/src/timer.c:99), [timer.c:330](/home/offihito/AvoryOS/kernel/linuxkpi/src/timer.c:330) | Expired private batches were manipulated outside the lock used by cancellation, and dequeue preceded running publication. A sync delete could also miss callback rearm. Protect batch reads/dequeue/running transitions and recheck pending/running under the timer lock. | Delayed GPU work and timeout teardown can otherwise race or leave a callback armed against released storage. |
| CONFIRMED BUG | [timer.c:176](/home/offihito/AvoryOS/kernel/linuxkpi/src/timer.c:176), [timer.c:349](/home/offihito/AvoryOS/kernel/linuxkpi/src/timer.c:349) | Hrtimer try-cancel falsely reported idle while running; running was cleared before restart was resolved. Return -1 when running, resolve restart under the timer lock before clearing running, avoid duplicate enqueue after callback-side start, and drain process-context cancel/rearm. | Relevant to imported display/vblank timer lifetime; not tied to the physical GPU incident. |

## Exact AMDGPU and fence call paths

- Main IRQ: [amdgpu_irq.c:307](/home/offihito/AvoryOS/kernel/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_irq.c:307)
  uses `request_irq(..., IRQF_SHARED, ...)`. Fence shutdown at
  [amdgpu_fence.c:622](/home/offihito/AvoryOS/kernel/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c:622)
  uses `disable_irq()`.
- PCI snapshot/recovery: [amdgpu_device.c:3994](/home/offihito/AvoryOS/kernel/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c:3994),
  cache at line 6003 and load/restore at line 6033. The generic PCI reset wrapper
  at line 1002 has four callers: `si.c:1437`, `soc15.c:610`, `nv.c:483`,
  `soc21.c:399`. Each returns/propagates the reset result; the shim keeps
  `pci_reset_function()` returning `-ENOTSUPP`. No imported C caller of
  `pci_try_reset_function()` or `pci_reset_bus()` was found; declarations alone
  do not require implementing a dangerous fallback.
- GPU-internal MODE1/MODE2/BACO reset is separate from PCI SBR/FLR. For example,
  `soc21.c:393` selects separate branches. Refusing PCI reset cannot make
  GPU-internal resets host-safe by itself, but does not trigger a bus reset.
- Fence-array callback `dma-fence-array.c:65` and chain callback
  `dma-fence-chain.c:141` queue irq_work with references keeping the fence
  alive. Their work callbacks drop references; chain rearming can requeue
  during callback execution. No `irq_work_sync()` call exists in these files.
  Owners synchronizing other objects must exclude external producers and retain
  object lifetime until sync returns; sync cannot make callback self-free plus
  a concurrent raw-pointer owner safe without an ownership reference.
- IH coherent allocation is at `amdgpu_ih.c:64`; GART maps pages at
  `amdgpu_gart.c:81/138`; TTM maps pages at `ttm_pool.c:198`; AMDGPU maps SG tables
  at `amdgpu_ttm.c:816` and dma-buf import at `amdgpu_dma_buf.c:202`.

## Additional subsystem review and remaining limitations

**POSSIBLE ISSUE / intentionally incomplete support:** PCIe link/slot/root and
extended capability state (LTR, AER, DPC, PTM, VC, PCI-X) and MSI-X tables are not
fully modeled. `pci_restore_msi_state()` at `pci.c:1768` remains inert outside
ordinary snapshot restore; its one imported caller is SMU 13.0.6 recovery
(`smu_v13_0_6_ppt.c:2050`), not a demonstrated Raphael path. Bridges and active
MSI-X snapshots now fail explicitly. No claim is made that arbitrary Linux
PCI reset/resume recovery works.

**POSSIBLE ISSUE / intentional IRQ limitation:** hard callbacks still execute
under the descriptor lock. Own-vector registration/free/disable/synchronize
calls can deadlock, and `disable_irq_nosync()` may spin behind a hard handler
although it does not drain threaded work. Main AMDGPU IH has no such own-vector
call. Legacy INTx disable remains software dispatch gating; ONESHOT is rejected
there. Native interrupt hardware pending-state drain, vector reuse and real
MSI/MSI-X delivery have not been tested on hardware here. Callers must free
handlers before freeing vectors, following Linux ownership ordering.

**POSSIBLE ISSUE / intentional work/timer limitations:** callbacks run in
kthreads; workqueue policies/affinity and upstream color-based flush boundaries
are not fully modeled. External producers must be excluded for teardown.
Atomic-context `hrtimer_cancel()` retains the preexisting nonblocking behavior
and must not authorize freeing an executing timer. `timer_shutdown_sync()` is
still a deletion alias, not a permanent prohibition on future rearm; there
are no imported C callers. A separate shutdown-state implementation is needed
before enabling such users. A wedged initcall now keeps boot waiting indefinitely;
thread-creation failure still uses the logged inline fallback, whose blocking
behavior in idle context remains unverified.

**HIGH-CONFIDENCE DEFECT / unchanged fallback:**
`x86_stubs.c:156` implements `wbinvd_on_all_cpus()` with local WBINVD only. This
cannot provide an all-CPU flush on SMP. It is preexisting, explicitly documented,
and not changed in this audit; supported Raphael CLFLUSH paths and cache-alias
regressions do not exercise that fallback. A native cross-CPU cache rendezvous
is needed before depending on it.

DMA is identity-mapped in the **guest physical address domain**. This does not
mean a guest address equals host physical memory under VFIO. LinuxKPI DMA uses
`page_to_phys()` for page/coherent mappings and HHDM conversion for direct-map
single buffers; CPU virtual addresses are returned separately. No guest IOMMU,
bounce buffers or general DMA attribute handling were added. Coherent
WRITE_COMBINE/NO_KERNEL_MAPPING attributes still need separate support before
new users rely on them.

BAR/MMIO mapping uses resource physical addresses and native VMAP mappings.
`ioremap()` selects native PAT entry 3 (UC), WC entry 7; TTM cache transitions
track page cache flags and update aliases. Existing tests cover those paths,
MMIO fences, virtual-map lifetime and managed high RAM. Concurrent cache-mode
transition across live aliases and fallback all-CPU flushing remain outside
this simulated validation.

Firmware loading at `firmware.c:60` reports missing blobs, invalid sizes,
allocation failures and short reads. Probe failure unbinds the wrapper and
records the AMDGPU result. Imported early-init, PSP/SMU firmware loading,
IP initialization and ring-test error paths were inspected, not changed:
`amdgpu_device.c:3765`, `:3909`, `:2495`, `:2969`. Actual PSP/SMU/GFX/ring and
firmware interaction on Raphael remains **UNVERIFIED**, not cleared by a build.

## Validation actually performed

- `make -C kernel -j4`: **PASS**, full kernel linked, including imported AMDGPU.
  Existing compiler warnings and the linker RWX LOAD-segment warning remain.
- `git diff --check`: **PASS**.
- **32/32** relevant CPU-only Python regression scripts passed. Exact names
  and output are preserved in [the test results](linuxkpi-stability-audit-tests.txt).
  Run an individual test with `python scripts/tests/<name>`.
- New IRQ test uses the actual IRQ core with pthread-backed locks/work dispatch:
  queued and blocked-running thread drain, hardirq return contract, ONESHOT
  masking, disable nesting, shared rejection and 300 concurrent dispatch/free
  iterations under ASan/UBSan. Its workqueue shim simulates delivery; it does
  not prove native hardware masking or native vector allocator behavior.
- Actual irq_work queue/worker: 1,000 self-free and 2,000 self-requeue callback
  invocations under ASan/UBSan; synchronization waits through both invocations.
  The former post-callback write is rejected by the lifetime regression.
- Imported fence-array callbacks on actual irq_work: 2,000 signal/final-put
  completions under ASan/UBSan. Reference release is a CPU shim, so this is
  not a test of the entire native DMA-fence/RCU implementation; separate RCU
  and task/completion regressions also pass.
- Actual workqueue queue/worker/flush/cancel: concurrent requeue serialization,
  cancellation after callback self-requeue, armed delayed-work flush and queued
  delayed-work cancellation. Actual timer expiry blocks/cancel functions:
  expired batch removal, callback rearm and hrtimer restart/cancel. The former
  `del_timer_sync()` fails the new rearm regression as expected.
- Simulated PCI config verifies writable-field selection/W1C protection,
  BAR-before-Command order, 32/64-bit MSI and saved-state round trips, exclusion
  of FLR, read failure and MSI-X/bridge rejection. Power tests verify unsupported
  transitions/wake/device-enable fail without invented state or config writes.
- DMA tests verify high physical RAM, mask/range overflow, vmalloc rejection,
  SG address/length publication, mapped nents and rollback on failure.
- Initcall simulation completes at 620 seconds and verifies that the boot
  dependency does not return at 600 seconds. Completion joins once; creation
  failure takes the logged inline fallback.

The first broader run was 27/30: three extracted-code harnesses were stale
(missing msleep stub after this change; preexisting missing klogf stub; obsolete
cache-test extraction marker). Those harnesses were repaired without changing
the unrelated implementations. The final expanded run is 32/32.

No guest boot/EDU IRQ integration run or physical GPU init/recovery/reset run
was performed. Passing simulated regressions does not establish physical
AMDGPU stability or fix the host reboot.

## Host-reset hypotheses and next steps

**UNVERIFIED HYPOTHESIS:** a shared upstream bus reset, platform/firmware power
transition, GPU-internal reset, guest DMA fault or another host component could
participate in the platform reset. No trace here establishes any of them.
Separate IOMMU groups do not establish separate reset/power domains. The reported
CF9 and ACPI reset-reason strings, absent panic/MCE/watchdog logs, and continued
host execution after VFIO reset are insufficient to assign responsibility.

1. First run the corrected kernel in an emulated CPU-only VM with EDU MSI,
   threaded IRQ and SMP boot suites; use the newly linked kernel, not an older
   existing ISO. That isolates native dispatch/masking from physical passthrough.
2. Preserve host journal/kernel logs externally, guest serial logs and existing
   GPU flight-recorder output with common timestamps. Capture host firmware
   reset-reason evidence before each later controlled test.
3. Compare guest no-AMDGPU, ordinary probe and documented failure stage using
   logs before attempting recovery. Record actual PMCSR/config state and the
   selected GPU-internal reset method; read-only inspection is sufficient first.
4. Validate supported PCI state recovery and native masking in emulation,
   especially pending MSI delivery and vector reuse, before physical recovery.
5. Treat any later physical GPU reset/power experiment as a separate explicitly
   approved test. Do not change bus reset methods or touch the shared Raphael
   bridge/functions merely to make an unsupported API return success.
