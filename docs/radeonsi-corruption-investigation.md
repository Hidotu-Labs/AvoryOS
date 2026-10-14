# RadeonSI rendering corruption investigation — 2026-10-13

The rendering symptom is **not yet confirmed fixed**. This investigation found
and repaired a driver-owned CPU mapping fault that could silently substitute
anonymous memory for GEM/TTM backing. It also corrected three LinuxKPI fault and
sequence contracts, added an offscreen pixel oracle and bounded resource tracing,
and built the kernel and guest diagnostics. The fixes pass host regression tests.
No RadeonSI, Minecraft or passed-through GPU validation was executed this turn.
The user's latest reply confirms corruption persisted after the earlier LinuxKPI
changes; it does not identify the exact ISO used.

## A. Source map and resource flow

Paths below are relative to this repository. The import manifest
`kernel/linux/Makefile.files` pins Linux **v6.6.156**, commit
`8b73de7da85fde281a385e0b26eda9bffd3ca477`; this is a port atop AvoryOS,
not a running Linux kernel.

| Layer | Actual implementation / role |
|---|---|
| Mesa | Packaged `build/alpine/rootfs/usr/lib/libgallium-26.2.4-arch1.1.so`; package `mesa-1:26.2.4-1`. RadeonSI and AMD winsys source are not checked out here. Shader/upload implementation was not source-verified against this binary. |
| Syscalls and DRM bridge | `kernel/src/syscalls/sys_mm.c`, `kernel/linuxkpi/src/file.c`: ioctl dispatch, mmap bridge, imported `vm_ops` invocation and lifetime. |
| Native DRM integration | `kernel/src/drivers/gpu/drm/`: native DRM/GEM/PRIME integration and recorder hooks. |
| Imported DRM/GEM | `kernel/linux/drivers/gpu/drm/drm_gem.c`, `drm_vma_manager.c`; AMD `amdgpu_gem.c`, `amdgpu_object.c`, `amdgpu_ttm.c` under `kernel/linux/drivers/gpu/drm/amd/amdgpu/`. |
| TTM | `kernel/linux/drivers/gpu/drm/ttm/`: `ttm_bo.c`, `ttm_bo_vm.c`, `ttm_bo_util.c`, `ttm_tt.c`, `ttm_pool.c`; placement, migration, fault insertion and backing lifetime. |
| GPU virtual memory | AMD `amdgpu_vm.c`, `amdgpu_vm_sdma.c`, `amdgpu_vm_cpu.c`, `amdgpu_gart.c`, `gmc_v10_0.c`: BO VA bindings, PTE updates, dependencies, invalidation and faults. |
| Submission and completion | AMD `amdgpu_cs.c`, `amdgpu_job.c`, `amdgpu_ring.c`, `amdgpu_fence.c`, `gfx_v10_0.c`; `kernel/linux/drivers/gpu/drm/scheduler/`. |
| Fences and reservations | Imported `kernel/linux/drivers/dma-buf/dma-fence.c`, `dma-resv.c`; their waits, callbacks and reference operations are real implementations, not unconditional-success local stubs. |
| DMA and pages | `kernel/linuxkpi/src/dma-mapping.c`, `page.c`, `mmap.c`, `vmalloc.c`, `x86_stubs.c`; `kernel/linuxkpi/include/linux/dma-mapping.h`, `mm.h`. |
| Synchronization | Local `wait.c`, `completion.c`, `mutex.c`, `ww_mutex.c`, `rcu.c`, `workqueue.c`, `timer.c`, `irq.c`, `irq_work.c`; overlay `spinlock.h`, `seqlock.h`. Imported `kernel/linux/include/linux/{atomic,kref,refcount}.h` and x86 `asm/{atomic,barrier}.h`. |
| Native memory | `kernel/src/mm/pmm.c`, `pmm_buddy.c`, `pmm_pcp.c`, `vma.c`, `mapping_unmap.c`, `vmm_map.c`, `vmm_fault.c`, `tlb_shootdown.c`. |
| Build and evidence | `kernel/GNUmakefile`, root `GNUmakefile`, `kernel/src/linuxkpi/gpu_diag.c`, `/proc/gpu_debug`, `scripts/gpu-debug-capture.sh`, host serial logs. |

The relevant flow is:

1. Mesa GEM create ioctl reaches AMDGPU BO creation, TTM placement/backing,
   LinuxKPI page allocation and native PMM. The existing AvoryOS-owned allocation
   policy lives in `kernel/linuxkpi/src/amdgpu_memory.c`.
2. CPU mmap retains a driver VMA through `file.c`. A native page fault invokes
   `linuxkpi_vma_fault()`, TTM's BO fault handler and PFN/PTE insertion. CPU uploads
   must reach the same BO pages subsequently mapped for the GPU.
3. GEM VA ioctls bind BO ranges into GPUVM. TTM's DMA page addresses feed GART/VM
   mappings. VM updates and BO reservations supply submission dependencies.
4. CS ioctl validates/reserves buffers, constructs a scheduler job, and emits
   ring commands and fences. IRQ processing signals completion; reservation
   fences and object references govern reuse, migration and destruction.

Raphael GC 10.3.6 follows `gmc_v10_0`/`gfx_v10_0` discovery, not the similarly
named v11 implementation. Default GFX VM updates use SDMA; the CPU update path
has a barrier/HDP commit, and ring commit has a barrier. Merely seeing those
barriers does not prove complete resource ordering. HMM/userptr MMU-notifier
paths are disabled in this build.

## B. Confirmed defects and patches

| File / function | Violated invariant | Implemented correction and scope |
|---|---|---|
| `kernel/src/mm/vmm_fault.c`, driver VMA branch | A driver-owned mapping cannot acquire generic anonymous backing when its driver rejects the fault. Previously the rejection fell through into generic demand paging. CPU writes could consequently target a different physical page than the GPU BO. | Release the bridge reference and reject the fault immediately. Successful faults and concurrent-unmap checks remain. The test exercises actual branch code and proves the previous fallthrough fails the invariant. No Minecraft trace yet proves this branch caused its artifacts. |
| `kernel/linuxkpi/include/linux/mm.h`, `fault_flag_allow_retry_first()` | First retry is allowed only with ALLOW_RETRY and **without** TRIED. The overlay required TRIED. | Correct the predicate. The current native bridge sets WRITE only, so this fix is not demonstrated to affect the reported workload. |
| Same header, `VM_FAULT_*` | Overlay masks must match the imported Linux fault ABI: SIGBUS=0x2, RETRY=0x400, etc. The previous definitions disagreed with Linux 6.6. | Match the imported enum, include HWPOISON_LARGE in ERROR, remove obsolete VM_FAULT_WRITE. Some old overlay users could have been internally consistent; numerical mismatch alone does not establish pixel causation. |
| `kernel/linuxkpi/include/linux/seqlock.h`, `read_seqbegin()` / `read_seqcount_begin()` | Readers must wait for an even sequence. The previous reader could return an odd sequence and accept it unchanged while a writer was paused. | Acquire-load and pause until even, then use the existing retry contract. The imported DRM vblank count/time reader is a concrete caller. This is not evidence that vblank caused source framebuffer corruption. |
| `kernel/linuxkpi/src/file.c`, `linuxkpi_vma_fault()` | A bounded wait for mmap rebase cannot consume an odd sequence after exhausting its loop. | Reject an unfinished rebase before reading the VMA triple. This does not prove all rebase, prefault and invalidation races safe. |

Upstream comparison: [Linux 6.6 fault masks](https://raw.githubusercontent.com/torvalds/linux/v6.6/include/linux/mm_types.h),
[retry predicate](https://raw.githubusercontent.com/torvalds/linux/v6.6/include/linux/mm.h),
[sequence readers](https://raw.githubusercontent.com/torvalds/linux/v6.6/include/linux/seqlock.h),
[TTM fault handler](https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/gpu/drm/ttm/ttm_bo_vm.c).

Rejecting faults now exposes errors rather than silently creating unrelated RAM.
In particular, the existing bridge also rejects a changed rebase sequence. That
case can now surface as a process fault; a future recoverable-retry change needs
a defined native retry contract and concurrency validation, rather than restoring
anonymous fallback. Capture the native rejection and raw FAULT result if this
occurs. No physical GPU reset test was performed.

## C. Evidence and root-cause ranking

**Strong reported evidence:** corrupted Minecraft F2 PNGs survive viewing on the
host, RadeonSI corrupts gameplay and menu text, llvmpipe is clean, both desktops
are affected, native Linux on the same GPU is clean, and fog is not required.
These observations prioritize upstream rendering/resource data over final
scanout. They do not distinguish shader output, uploads, BO backing and readback.
The screenshot was inspected but has no exact CPU oracle; no invented artifact
count is attached to it.

1. **Confirmed integration defect, plausible contributor:** rejected CPU BO
   fault becoming anonymous memory. This directly violates resource identity.
   The conditional is proven; occurrence in a corrupt frame is still unproven.
2. **Unverified high-priority hypotheses:** CPU/GPU visibility or cache alias,
   resource migration/residency/lifetime, stale GPUVM updates or dependencies,
   and premature reuse. Correlating an affected capture with a BO and fence is
   still required. Earlier IRQ, PCI, DMA-mask, workqueue and timer corrections
   are documented separately in `linuxkpi-stability-audit.md`; persistence after
   them means they cannot be credited as a demonstrated rendering fix.
3. **Open alternatives:** shader/descriptor/uniform uploads, userspace format
   transitions, tiling/compression, firmware and differing Mesa versions.
   Minecraft text and random scene pixels are not assumed to share one cause.
4. **Observed separate failure:** `build/gpu-diagnostics/minecraft-crash-evidence.txt`
   records high-order allocation failures and CS -ENOMEM before Java abort.
   Memory pressure is real evidence, but not proof of the random-pixel cause.
5. **Rejected specific hypothesis:** contiguous page descriptors crossing sparse
   128 MiB sections. Current native buddy blocks are power-of-two aligned and
   LinuxKPI MAX_PAGE_ORDER=10 (4 MiB). Valid blocks do not cross those boundaries;
   no speculative page model rewrite was made.

**Ruled out as exclusive explanations by the reported captures:** physical
monitor, final scanout alone and compositor-only output. PRIME/shared-buffer
operations upstream of capture remain possible. Fog as a necessary trigger is
explicitly ruled out. VirtIO rewrites and ordinary syscall leak cleanup are not
the current strategy.

DMA mappings were traced to the actual guest architecture: the shim uses guest
physical DMA addresses, validates supported masks and builds unmerged SG entries.
There is no guest IOMMU mapping implementation in this configuration. VFIO's
host mapping of guest DMA is distinct from guest physical-to-host physical
identity. No-op unmap is consistent with a mapping that installs no guest IOMMU
state, but does not establish correct BO lifetime. Existing device synchronization
drains WC writes with sfence; CPU synchronization uses dma_mb. Cache attributes,
alias lifetime and actual data visibility remain hardware audit targets.

`gmc_v10_0.c` fault handling decodes VA/PASID, filters retry faults, reads status,
acknowledges and logs through a rate limit. This is source inspection, not a
successful live fault-reporting experiment. No deliberately invalid GPU command
or fault injection was executed.

The existing PRIME recorder summary had zero source_changed, stable_copy_mismatch,
cache_mismatch and anomalies. Those checks primarily concern the copy path;
they do not establish that the rendered source BO was correct. The saved host
tunnel heuristic found no conspicuously green pixels in 116 samples per mode;
it is not an exact rendering oracle. The saved guest tunnel replay identifies
llvmpipe and cannot be counted as RadeonSI validation.

Mesa's [documented RadeonSI options](https://docs.mesa3d.org/envvars.html) include
`nodcc`, `notiling`, `usellvm`, `nowc` and `nongg`. The reported `nogg` spelling
does not match `nongg`. Strings in the packaged guest library also include
`nowcstream` and `nodma`; an exact `llvm` option was not found. Binary strings
do not prove parser support or that a particular draw used the altered path.
Exact 26.2.4 parser source was unavailable during this audit. Therefore none of
the prior flag experiments eliminates its target subsystem.

## D. Bounded tracing and deterministic workload

`kernel/linuxkpi/src/amdgpu_trace.c` wraps selected upstream symbols through
per-translation-unit renames in `kernel/GNUmakefile`. Imported AMDGPU source is
unchanged. Original errors/results are forwarded; hooks add no waits or resource
data changes. The GEM creation hook takes and releases a temporary lookup
reference only when tracing. Mode 3 arms **1024 total resource events**, retained
separately from the existing 2048-event metadata ring (~72 KiB extra static
storage). Production starts in the existing metadata mode; resource tracing is
off until requested. Freeze before reading. Mode 2 retains its PRIME pixel checks.

All entries include sequence, timestamp, thread, kind, object, a/b/c/d and result:

| Event | Object | a / b / c / d |
|---|---|---|
| BO_HANDLE | AMD BO | DRM file / GEM handle / size / requested domains |
| BO_BACKING | AMD BO | size / current TTM memory type / reservation / TTM BO pointer |
| CPU_VMA | mmap bridge | driver vm_private_data (TTM BO for AMD) / CPU VA / pgoff / fault error |
| VM_BIND | DRM file | handle<<32 OR operation / GPU VA / map size / flags |
| VM_OFFSET | DRM file | handle / offset in BO / GPU VA / map size |
| VM_UPDATE | AMD BO | VM / clear flag / last_pt_update fence / 0 |
| PAGE_BIND | AMD BO | page index / physical address / DMA address / refcount |
| DMA_MAP | page | physical+offset / DMA / size / offset |
| PAGE_ALLOC | head page | physical address / page count / GFP / refcount |
| PAGE_FREE | head page | physical address / page count / cache flags / refcount |
| FENCE_EMIT | fence | context / sequence / ring / flags |
| RESV_FENCE | reservation | fence / context / sequence / usage |
| FENCE_SIGNAL | fence identity | context / sequence / phase (0 before, 1 after call) / 0 |

Existing CS_IN/CS_OUT, MAP/FAULT and copy events supplement these. VM_BIND's
operation includes unmap operations. BO_BACKING reports placement, not a complete
reservation-lock snapshot. PAGE_BIND samples boundary pages only. VM_UPDATE's
last_pt_update is one dependency identity, not the complete submission graph.
Calls renamed within the same upstream translation unit can bypass the wrapper;
`dma_fence_signal_locked` is also not intercepted. This is **partial tracing**,
not proof of complete lifetime coverage. Fence callbacks may legitimately free
resources before phase 1; that alone is not premature reuse. No object is
dereferenced after signal callbacks. Budget exhaustion or missing events cannot
prove a resource was never mapped/freed/signaled. Tracing can perturb timing;
compare unarmed and armed runs.

`userland/test_gpu_corruption.c` creates a GL 3.3 offscreen RGBA8 FBO, reuses four
source textures and renders sixteen deterministic tiles. Texture/mixed cases
upload known byte patterns; stream/mixed cases orphan/update vertex buffers.
Per-draw uniforms alter the expected bytes. Default source is normalized RGBA8;
`--integer-source` supplies an RGBA8UI control. There is no fog, compositor,
scanout or PRIME copy. Shader/texture output is compared exactly with a CPU oracle.
The hardware mode selects an amdgpu render node and refuses other renderers.
`--software` is an explicit llvmpipe test mode, not a system acceleration change.

Readback selects COLOR_ATTACHMENT0, RGBA/UNSIGNED_BYTE, PACK_ALIGNMENT=1,
PACK_ROW_LENGTH=0 and no PBO. Normal GL readback synchronization is used;
per-draw glFinish exists only behind the optional `--serialize` experiment.
CSV records every frame's bad-pixel count, actual/expected CRC and elapsed time.
The first eight failing frames save actual/expected PNG pairs with corrected
OpenGL-to-PNG row orientation. Exit 0=clean, 1=mismatch, 2=setup/API failure.
`--inject-readback-error` is explicitly a CPU-side oracle self-test, not GPU
corruption reproduction. This workload has **not yet reproduced the AvoryOS
artifact**; a clean run cannot clear every Minecraft path.

## E. Executed validation

Commands executed from the repository:

```sh
make -C kernel -j4
make gpu-diagnostics
python3 scripts/tests/run_gpu_corruption_audit.py
git diff --check
```

The runner invokes every `scripts/tests/test_kpi*.py` plus cache/file,
native DMA, PCI transaction, AMD memory policy, GEM initialization and GPU
recorder/ioctl/resource/oracle tests, in four host workers. The complete selected
test list and captured outputs are in `radeonsi-corruption-tests.txt`.

| Validation | Actual result |
|---|---|
| Kernel compile/link | Passed; existing compiler warnings and RWX LOAD-segment linker warning remain. |
| Guest diagnostic package | Built; executable at `build/gpu-diagnostics/test_gpu_corruption`. This is not an ISO installation or guest execution. |
| Selected host regression scripts | 39/39 passed. |
| Oracle clean controls | 400 llvmpipe frames, 26,214,400 pixels, zero mismatches: both source formats, each mixed=100, texture=50, stream=50. |
| Oracle deliberate CPU error | One changed pixel on frame 2 detected; exit 1, frame counts [0,0,1,0], valid PNG chunks/CRCs and orientation. |
| Hardware-mode guard on this host | Setup exit 2 with no AMD render node, as expected; not a RadeonSI rendering result. |
| Fault / sequence negative controls | Prior anonymous fallthrough, inverted retry predicate and odd-sequence reader fail their targeted checks. |
| Resource wrappers | Actual wrapper code under ASan/UBSan passes failure forwarding, IDs, disabled tracing, null fence handling and callback-release/no post-signal dereference. |
| Recorder | Mode 1 ignores resources; mode 3 caps/retains 1024 despite later metadata overwrite; freeze preserves records. |
| Minecraft gameplay/menu/F2, tunnel fog on/off, four basic demos, GPUVM/hang checks | **NOT RUN** with the new kernel on RadeonSI. |

One initial broader run failed because the shmem lifetime test fixture lacked
the new disabled-telemetry declarations; that fixture was updated and the suite
rerun. No kernel behavior was bypassed to obtain that pass.

Evidence logs: `build/gpu-corruption-audit/kernel-build-final.log`,
`diagnostic-package-build.log`, `test-summary.log`, `tests.json`. Host oracle
renderer is llvmpipe Mesa 26.2.1-arch3.1 / LLVM 22.1.8, different from guest Mesa
26.2.4. At inspection, Raphael 0000:0e:00.0 was bound to vfio-pci, the available
render node belonged to NVIDIA, and no QEMU process was running. The agent did
not start a guest, change VFIO binding or reset the physical GPU.

## F. Exact hardware validation and next experiments

Build an updated ISO using the normal workflow before the next user-controlled
boot. The kernel-only build does not update an already booted guest or ISO:

```sh
cd /home/offihito/AvoryOS
make -j4
make gpu-diagnostics
python3 -m http.server 9007 --bind 127.0.0.1 --directory build/gpu-diagnostics
```

Boot the updated ISO using your existing passthrough procedure. The server above
is a foreground transfer service; stop it after downloading. In the AvoryOS
guest with the usual QEMU 10.0.2.2 host forwarding, as root:

```sh
cd /tmp
wget -O test_gpu_corruption http://10.0.2.2:9007/test_gpu_corruption
chmod +x test_gpu_corruption
./test_gpu_corruption --software --frames 1000 --case mixed --output /tmp/oracle-software > /tmp/oracle-software.log 2>&1
echo 3 > /proc/gpu_debug
./test_gpu_corruption --frames 1000 --case mixed --output /tmp/oracle-mixed > /tmp/oracle-mixed.log 2>&1
echo freeze > /proc/gpu_debug
cat /proc/gpu_debug > /tmp/oracle-mixed-resources.log
echo dump > /proc/gpu_debug
echo 3 > /proc/gpu_debug
./test_gpu_corruption --frames 1000 --case texture --output /tmp/oracle-texture > /tmp/oracle-texture.log 2>&1
echo freeze > /proc/gpu_debug
cat /proc/gpu_debug > /tmp/oracle-texture-resources.log
echo 3 > /proc/gpu_debug
./test_gpu_corruption --frames 1000 --case stream --output /tmp/oracle-stream > /tmp/oracle-stream.log 2>&1
echo freeze > /proc/gpu_debug
cat /proc/gpu_debug > /tmp/oracle-stream-resources.log
./test_gpu_corruption --frames 1000 --case mixed --integer-source --output /tmp/oracle-integer > /tmp/oracle-integer.log 2>&1
```

Record each exit status immediately (`echo $?`), keep logs, CSV and PNG pairs,
and retain the host serial log. Use fresh output directories for repeats. Remove
previous experimental AMD_DEBUG overrides for the baseline. Confirm each hardware
log names RadeonSI and record Mesa, firmware, kernel/ISO hashes and VM settings.
Repeat without mode 3 to check timing perturbation. If a case fails, compare:

```sh
./test_gpu_corruption --frames 1000 --case mixed --serialize --output /tmp/oracle-serialized > /tmp/oracle-serialized.log 2>&1
```

A change with serialization ranks an ordering hypothesis; it is not a production
fix. A normalized-only versus integer-source difference ranks sampling/format
paths but also changes shaders and resource state. A clean oracle leaves more
complex Minecraft paths unresolved.

For the actual symptom, arm mode 3 immediately before an affected menu or scene,
take Minecraft F2 screenshots when artifacts appear, freeze and export the log.
The bounded helper supports this:

```sh
wget -O /tmp/gpu-debug-capture http://10.0.2.2:9007/gpu-debug-capture
chmod +x /tmp/gpu-debug-capture
/tmp/gpu-debug-capture 5 15 3 /tmp/minecraft-resources.log
```

Repeat gameplay, menu text and F2 capture; repeat tunnel with fog both on and off
(neither is a pass/fail isolation criterion); rerun glxgears, multitex, fbotexture
and readpix under confirmed RadeonSI. Their executable paths and interactive
controls are not established by this checkout, so no invented commands are given.
Check serial logs for GPU faults, CS errors, timeouts and native driver-fault
rejections. Compare captured files on the host, not only displayed output.

For a rejected CPU fault, join CPU_VMA's TTM BO pointer to BO_BACKING.d, then
BO_HANDLE's DRM file/handle to VM_BIND/VM_OFFSET. Correlate PAGE_BIND/DMA_MAP
physical addresses with PAGE_ALLOC/PAGE_FREE, and reservation fence identity
with FENCE_EMIT/SIGNAL context/sequence. A convincing lifetime diagnosis requires
the responsible submission and complete dependent-fence coverage, not just
nearby timestamps. Budget may expire early: arm nearer the artifact or repeat a
smaller failing workload before expanding instrumentation.

Remaining work is to obtain a real RadeonSI failing oracle or resource capture,
tie the confirmed mapping defect (or another defect) to that failure, and repeat
the full hardware matrix on the patched kernel. Native Linux comparison should
use matched Mesa/firmware and the same workload; no such matched comparison was
performed here. Exact shader binaries/descriptors and full BO content checking
remain follow-up work if kernel tracing does not isolate the fault.

## User-reported recurrence — 2026-10-14

The user initially reported disappearance of the pixel artifacts after the
compatibility corrections, then reported another whole physical host reboot
and returning pixel artifacts on a subsequent test. Treat both symptoms as
unresolved. The initial clean run is not proof of permanent artifact resolution.
The exact loaded kernel/ISO and GPU diagnostic traces for the recurrence have
not been established. See the [updated stability audit](linuxkpi-stability-audit-2026-10-14.md).

The user subsequently reported another whole-host reboot during AMDGPU
initialization. No specific initialization operation has been identified.
