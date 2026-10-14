# LinuxKPI / AMDGPU audit verification — 2026-10-14

The guest compatibility defects below were fixed or verified by CPU simulation.
**The physical host reboot is not attributed, reproduced, or demonstrated fixed.**
No physical GPU test, host reset/power setting change, or shared-bridge operation
was performed. Imported Linux/AMDGPU sources were not edited.

## Reported hardware recurrence after validation

On 2026-10-14 the user initially reported that the pixel artifacts disappeared,
then reported another **whole physical host reboot** during a subsequent test
and returning pixel artifacts afterward. The apparent rendering improvement was
not durable. The host reboot and rendering corruption remain unresolved; this
commit is a checkpoint of verified compatibility corrections, not a stability
or artifact-resolution claim.

Read-only journal inspection after that report listed the previous boot ending
at 18:07:44 and the current boot beginning at 18:08:23 (Europe/Istanbul). No
previous-boot kernel entries were available through the current journal access,
including the unfiltered kernel query. That does not prove there was no kernel
fault or hardware event. The user subsequently reported another whole-host reboot specifically while
AMDGPU was initializing. Initialization is therefore a user-reported trigger
phase; its last completed step, loaded ISO/kernel identity and diagnostic trace
are still unknown. Reading `/sys/fs/pstore` was denied by host permissions, so
no firmware/kernel persistent crash record was retrieved.

## Scope and ownership

The starting tree contained substantial uncommitted work, including fixes in
IRQ, irq_work, PCI power/state, initcalls, DMA mapping, timer and workqueue code,
plus the [2026-10-13 audit](linuxkpi-stability-audit.md). Those changes were
preserved. This report distinguishes newly applied corrections from the prior
changes that were inspected and retested. It does not claim authorship of the
starting tree or treat a prior report as proof.

The local retrieval refresh was started; short retrieved IRQ passages were
checked against current source. The full refresh was still running during the
audit, so current bounded source reads and exact `rg` searches determined the
findings, rather than assuming the index represented every current edit. A second
refresh is queued after the first to reconcile the final source edits; neither
refresh is claimed complete by this report.

Compared contracts with primary Linux 6.6 sources:
[IRQ management](https://raw.githubusercontent.com/torvalds/linux/v6.6/kernel/irq/manage.c),
[irq_work](https://raw.githubusercontent.com/torvalds/linux/v6.6/kernel/irq_work.c),
and [workqueues](https://raw.githubusercontent.com/torvalds/linux/v6.6/kernel/workqueue.c).
In particular, full IRQ synchronization drains threads while hard synchronization
reports their activity; upstream workers keep execution state outside objects
that callbacks may release. This is a compatibility audit, not a full port of
those subsystems.

## Newly confirmed bugs and exact changes

All locations below refer to the corrected tree. **None has evidence connecting
it specifically to the reported physical host reboot.** Plausibility here means
guest initialization, rendering, recovery or teardown consequences only.

| Classification / location | Root cause and affected path | Applied correction / AMDGPU relevance |
|---|---|---|
| CONFIRMED BUG — [workqueue.c:123](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:123), [workqueue.c:50](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:50) | Worker wrote `work->running` after the callback. A callback may release the containing allocation. Imported [ttm_bo.c:305](/home/offihito/AvoryOS/kernel/linux/drivers/gpu/drm/ttm/ttm_bo.c:305) finishes delayed deletion with `ttm_bo_put()`, which reaches `bo->destroy()` at line 382. | Per-worker execution pointers in the queue replace object-owned running state. Queue selection, flush and cancel inspect these pointers under the existing queue lock. There is no post-callback dereference, extra global lock or upstream edit. Confirmed by ASan negative control. Plausible BO cleanup corruption, including initialization unwind and later rendering. |
| CONFIRMED BUG — [workqueue.c:365](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:365), [workqueue.h:81](/home/offihito/AvoryOS/kernel/linuxkpi/include/linux/workqueue.h:81) | Synchronous delayed cancellation deleted the timer before waiting for a running callback; the callback could then rearm it, leaving activity after cancellation returned. | Temporary per-delayed-work cancellation flag blocks queue/mod rearm during timer and work drain, then permits later scheduling. Regression holds a callback while cancellation starts and attempts delayed rearm. Relevant to AMDGPU delayed initialization, GFX-off and VCN/JPEG idle teardown; does not establish a first-probe fault. External producers must still be excluded by the owner. |
| CONFIRMED BUG — [workqueue.c:420](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:420) | Destruction stopped workers before pending work drained, abandoning callbacks and leaving their queue pointers referencing freed storage. | Drain the queue before stopping and joining workers and freeing it. Regression requires ten queued self-free callbacks to finish during destruction. Relevant to TTM/device cleanup and initialization failures. |
| CONFIRMED BUG — [pci.c:513](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:513), [pci.c:245](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:245), [native_pci.c:19](/home/offihito/AvoryOS/kernel/src/linuxkpi/native_pci.c:19) | Wrappers accepted misaligned offsets, used overflow-prone `where + width`, and advertised success beyond the native 256-byte CF8/CFC range. Native access rejected or dropped the transaction. | Record native accessible size (256 or 4096 with ECAM); validate alignment and bounds using subtraction. Invalid accesses return `PCIBIOS_BAD_REGISTER_NUMBER` without reaching the backend. Test includes `INT_MAX`, end-of-space and aligned/unaligned transfers. Can hide missing capability configuration or recovery failures; no observed Raphael reproduction. |
| CONFIRMED BUG — [pci.c:775](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:775) | Disabling an already disabled function decremented `enable_cnt` below zero. A later enable could skip hardware command setup. | Zero-count disable is a no-op; valid nesting remains balanced. Relevant to failure cleanup/retry. Enable/disable callers still need device-level lifecycle serialization, as in Linux. |
| CONFIRMED BUG — [pci.c:795](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:795), [pci.c:789](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:789) | Managed enable published its cleanup before enable succeeded. Failed enable retained an invalid cleanup; successful cleanup also left `is_managed` set, allowing a later managed enable to return without enabling the device. | Enable first, register `devm_add_action_or_reset()` afterward, and clear managed state on release. Allocation failure rolls back the acquired enable. Tests cover enable failure, action allocation failure, duplicate enable, cleanup and reprobe. Main AMDGPU probe uses ordinary enable; this is a compatibility failure-path defect, not a demonstrated Raphael trigger. |
| CONFIRMED BUG — [pci.c:411](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:411), [pci.c:495](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:495) | Failed probes and driver removal cleared binding but never released devres. Permanent PCI wrappers never reached `device_unregister()`. AMDGPU allocates its DRM object with `devm_drm_dev_alloc()` before device enable. | Release managed resources on failed probe and after the driver's remove callback; clear drvdata afterward. Tests verify failed-probe cleanup, preserved successful-probe ownership and remove-before-resource-release ordering. Real devres release remains LIFO in `device.c`. Plausible leaks/stale managed IRQs and objects during failed initialization or reprobe. |

The public workqueue interfaces remain unchanged. `work_struct.running` stays
reserved for layout compatibility; execution is recorded in the worker pool.
`INIT_DELAYED_WORK` initializes cancellation state, and static designated
initializers zero it. The stale bounded-wait comment in `initcall.h` was corrected.

## Prior fixes independently inspected and retested

These were already present at task entry. The earlier report describes their
original defects and edits in more detail. Current corrected locations:

| Classification / corrected location | Verified behavior and affected path |
|---|---|
| CONFIRMED BUG (prior fix) — [irq.c:249](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:249), [irq.c:299](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:299) | Full synchronization accounts for queued/running threads, including removed actions. Hard synchronization drains hard dispatch and returns false for thread activity without waiting on it. Disable uses full sync. Teardown race is plausible; primary AMDGPU IH is a hard handler. |
| CONFIRMED BUG (prior fix) — [irq.c:58](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:58), [irq.c:150](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:150) | ONESHOT holds supported hardware masks through threaded execution and disable nesting; unsupported masking is rejected. Shared registration requires agreement, dev_id and matching ONESHOT. Relevant to threaded users, not demonstrated on AMDGPU IH. |
| HIGH-CONFIDENCE DEFECT (prior fix) — [irq.c:55](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:55), [irq.c:68](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:68) | Static valid locks avoid concurrent first-use reinitialization; mask registration/removal is locked. Concurrent vector lifecycle corruption was plausible, not reproduced on hardware. |
| CONFIRMED BUG (prior fix) — [irq.c:203](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq.c:203) | Free unlinks under descriptor lock and drains queued/running work before freeing the action. Concurrent dispatch/free stress passes. |
| CONFIRMED BUG (prior fix) — [irq_work.c:113](/home/offihito/AvoryOS/kernel/linuxkpi/src/irq_work.c:113) | Queued/running snapshot uses the queue lock and execution state lives outside callback storage. Self-requeue and final-put fence-array execution pass. No imported irq_work_sync caller was found, so the sync race itself is not a demonstrated current init path. |
| CONFIRMED BUG (prior fix) — [pci.c:755](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:755), [pci.c:704](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:704) | Unsupported power transitions do not fabricate cached success; enable rejects non-D0 state. CONFIG_PM/ACPI are absent. Switcheroo/runtime-PM calls exist in imported code but ordinary probe uses device enable. No Raphael power domain was changed. |
| CONFIRMED BUG (prior fix) — [pci.c:1296](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:1296), [pci.c:1356](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:1356) | PCI snapshots include selected PCIe device controls and MSI routing. Restore excludes W1C/readonly header fields and FLR, restores BARs before Command, and uses narrow writes. Active MSI-X and bridge saves fail. Reached by successful AMDGPU initialization's cache/restore at `amdgpu_device.c:3994/6003/6033`; plausible configuration/recovery defects. |
| CONFIRMED BUG (prior fix) — [initcalls.c:34](/home/offihito/AvoryOS/kernel/linuxkpi/src/initcalls.c:34), [kernel.c:873](/home/offihito/AvoryOS/kernel/src/kernel.c:873) | Boot waits for completion beyond 600 seconds, then joins; dependent tests cannot start while the walker is unresolved. Failures are logged by `init.c` and probe result is retained by `pci.c`. No unsafe thread termination. |
| CONFIRMED BUG (prior fix) — [dma-mapping.c:19](/home/offihito/AvoryOS/kernel/linuxkpi/src/dma-mapping.c:19), [dma-mapping.c:100](/home/offihito/AvoryOS/kernel/linuxkpi/src/dma-mapping.c:100), [dma-mapping.c:170](/home/offihito/AvoryOS/kernel/linuxkpi/src/dma-mapping.c:170) | Masks/range overflow and vmalloc single mapping are checked; SG DMA fields/nents are populated and rolled back on failure. GART, IH and TTM/import users can otherwise corrupt guest memory. |
| CONFIRMED BUG (prior fix) — [workqueue.c:238](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:238), [workqueue.c:273](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:273), [workqueue.c:260](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:260), [workqueue.c:346](/home/offihito/AvoryOS/kernel/linuxkpi/src/workqueue.c:346) | Locked flush/cancel snapshots and rechecks drain callback requeue; delayed flush handles armed timers, and nonblocking cancellation handles already queued work. New changes above close additional lifetime/rearm/destruction gaps. |
| CONFIRMED BUG (prior fix) — [timer.c:99](/home/offihito/AvoryOS/kernel/linuxkpi/src/timer.c:99), [timer.c:331](/home/offihito/AvoryOS/kernel/linuxkpi/src/timer.c:331), [timer.c:176](/home/offihito/AvoryOS/kernel/linuxkpi/src/timer.c:176), [timer.c:350](/home/offihito/AvoryOS/kernel/linuxkpi/src/timer.c:350) | Expired batch/dequeue/running transitions are locked; deletion drains callback rearm. Hrtimer try-cancel reports running, restart resolves before running clears, and cancellation drains restart. Relevant to GPU delayed work/display timer teardown. |

There is no direct host-reset evidence for any prior finding either.

## Reset, DMA, cache and initialization paths

`pci_reset_function()` remains unsupported at
[pci.c:1756](/home/offihito/AvoryOS/kernel/linuxkpi/src/pci.c:1756).
Its AMDGPU wrapper is `amdgpu_device.c:1002`; four callers are
`si.c:1437`, `soc15.c:610`, `nv.c:483`, `soc21.c:399`. Each returns or propagates
the result. No imported C call to `pci_try_reset_function()` or `pci_reset_bus()`
was found. `soc21.c:393` selects PCI, BACO, MODE2 and MODE1 separately. Refusal of
PCI reset does not prove that GPU-internal reset or firmware transitions are
host-safe. Separate IOMMU groups do not establish separate reset/power domains.

DMA addresses use guest physical memory, not CPU virtual or host physical
addresses. Coherent/page mappings use `page_to_phys`, direct-map single buffers
use HHDM conversion, and vmalloc single buffers are rejected. IH coherent DMA
is used at `amdgpu_ih.c:64`; GART/TTM and SG-table imports use their page/SG APIs.
BAR mappings use resource physical addresses. `vmalloc.c:412` selects PAT UC
entry 3 for ioremap and WC entry 7 for ioremap_wc. Cache-alias, MMIO-ordering and
high-RAM bounds regressions pass; those do not validate live GPU coherency.

Fence-array and chain work retain references across queued execution and release
them in callbacks (`dma-fence-array.c:45/65`, `dma-fence-chain.c:126/141`).
irq_work handles self-requeue without touching callback storage after return.
Synchronization callers must retain the object's lifetime and exclude external
producers; a raw pointer to a self-freed object cannot be made safe by sync.

The imported IP initialization sequence checks firmware loading and phase-1/2
hardware init results (`amdgpu_device.c:2495` onwards); top-level init unwinds on
IP-init failure at line 3909. Firmware loader `firmware.c:60` reports missing
blobs, invalid sizes, allocations and short reads. The delayed IB-ring test at
`amdgpu_device.c:2969` logs failure after scheduling. Real Raphael PSP/SMU/GFX,
ring execution and firmware effects remain unverified.

## Validation and negative controls

- `make -C kernel -j4`: PASS, linked full kernel with imported AMDGPU. Existing
  compiler warnings and linker RWX LOAD warning remain. The final incremental
  build after the last drain fix also passed.
- `git diff --check`: PASS.
- **40/40 selected CPU-only scripts passed**. Exact scripts and actual output:
  [test log](linuxkpi-stability-audit-2026-10-14-tests.txt). The workqueue test
  was rerun after the final destruction fix; the other 39 were already passing.
- Workqueue: actual worker/queue functions with pthread-backed locks, 1,000
  self-free callbacks under ASan/UBSan, serialized requeue, flush/cancel,
  callback requeue during cancellation, delayed flush, delayed rearm rejection,
  post-cancellation scheduling, and ten callbacks drained on destruction.
- PCI: actual wrapper functions with simulated config and managed cleanup;
  CF8/ECAM bounds, alignment, overflow, failed enable/action registration,
  balanced nesting, managed cleanup/reprobe, and failed-probe/unregister release.
  Registry/devres collaborators are simulated, not a full native driver-core run.
- Existing coverage includes 300 concurrent IRQ dispatch/free iterations,
  ONESHOT and hard/full sync contracts, irq_work self-free/requeue, 2,000 imported
  fence-array completions, timer rearm, completions, RCU/task lifetimes, mutexes,
  DMA/SG masks, PCI save/restore, init beyond 600 seconds, PAT and MMIO ordering.
- Negative controls: restoring the old workqueue post-callback write produces
  ASan heap-use-after-free; pre-fix delayed cancellation leaves the timer armed;
  pre-fix destruction fails the ten-callback drain assertion; original config
  wrapper accepts invalid access; original managed-enable lifecycle retains
  cleanup after failure; original probe wrapper fails managed-release assertions.
  These are expected failures, not failing final regressions.

No VM boot, EDU hardware-delivery test, native GPU initialization/recovery or
physical reset experiment was run. Passing compilation is not the validation
basis for the lifetime fixes; simulation still cannot prove hardware stability.

## Remaining risks and recommended debugging

- HIGH-CONFIDENCE DEFECT — [x86_stubs.c:156](/home/offihito/AvoryOS/kernel/linuxkpi/src/x86_stubs.c:156)
  still flushes only the local CPU for `wbinvd_on_all_cpus()`. A correct native
  SMP rendezvous is required before relying on that fallback. It was not
  replaced with an unvalidated IPI implementation in this audit.
- POSSIBLE ISSUE — hard IRQ handlers run under the descriptor lock. Own-vector
  disable_nosync/registration calls can deadlock; free/full sync from a handler
  are also unsuitable. Main AMDGPU IH has no discovered own-vector call.
  Native pending-interrupt drain/vector reuse and real masking are unverified.
- POSSIBLE ISSUE — incomplete extended PCI capability and MSI-X recovery;
  standalone `pci_restore_msi_state()` at `pci.c:1784` remains inert, reached by
  SMU 13.0.6 recovery at `smu_v13_0_6_ppt.c:2050`, not a demonstrated Raphael path.
  Config status reports bounds/access availability, not physical write
  acknowledgement or hot-unplug faults; the native API lacks that error channel.
- POSSIBLE ISSUE — workqueues do not implement upstream policy/affinity/colors,
  cross-queue execution migration, or producer exclusion during drain. Teardown
  owners must stop external submissions; a permanently requeuing callback may
  keep a drain waiting. Delayed queue/mod pending semantics remain less complete
  than Linux. Atomic-context timer cancellation and timer_shutdown rearm
  limitations described in the prior audit remain.
- POSSIBLE ISSUE — no guest IOMMU/bounce-buffer or general coherent DMA-attribute
  implementation. Concurrent cache-mode transitions across live aliases remain
  unverified. Wedged initcall intentionally holds the boot barrier indefinitely;
  thread-creation inline fallback in idle context remains a limitation.
- UNVERIFIED HYPOTHESIS — shared bus reset, firmware/platform power transition,
  GPU-internal reset, DMA fault or another host component could participate in
  the reset. CF9/ACPI reason strings, missing panic/MCE/watchdog logs and delayed
  host failure after VFIO reset do not establish a cause.

Next: boot the newly linked kernel in an emulated SMP VM without passthrough;
exercise EDU MSI/threaded IRQ teardown and the native boot suites. Then preserve
host journal/serial/reset-reason evidence externally with synchronized guest
flight-recorder timestamps. Compare no-AMDGPU boot and ordinary probe failure
stages before recovery experiments. Capture read-only PMCSR/config and selected
GPU-internal reset method. Any later physical reset/power experiment requires
separate explicit authorization; do not use the shared Raphael bridge as a
fallback implementation for unsupported reset APIs.
