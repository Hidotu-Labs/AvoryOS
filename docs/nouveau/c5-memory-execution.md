# C5: memory, explicit VM and first GPU execution

Started 2026-10-21. **C5 core (memory, explicit VM, deterministic GPU
execution) is closed (scope decision 2026-10-21)** on the hardware evidence
below plus the root-caused warm-loop PMM drift fix verified by eight
consecutive strict software boots. Pressure/eviction, zeroing-on-reuse,
outstanding-work exit and extended stress oracles are **deferred known gaps**,
not satisfied by any recorded run (see Remaining C5 gates); hardware
re-validation of the fixed allocator is pending the next hardware session.
No rendering or NVK desktop acceleration is claimed from a copy-engine test.

## Configuration and isolation

- Linux 6.18.42 / GSP 570.144. The recorded hardware evidence used kernel
  SHA256 `e332cd472663bde15a9ddc27ed2326acaf0a27ff91b8ae1c5615ec165d20d8d9`;
  the closed C5 core candidate (2026-10-21) is the drift-fixed kernel SHA256
  `c5e8c9fed4ca8610ef038a898b3e38a543e80caa6ff083c14a185c39d49d7780`.
- One vCPU, private 4 GiB Q35 VM and snapshot diagnostic disk; no installed
  desktop-disk writes. The normal C4 ISO remains unchanged by these tests.
- Only GPU function `01:00.0` is assigned behind root port `00:03.0` at guest
  `01:00.0`. Both physical functions remain isolated and VFIO-bound. No host
  reset-policy, driver-binding or power-policy writes are made.
- Software qualification matches kernel, firmware, topology, oracle kind,
  oracle source and imported ABI/class-header hashes. A C4 probe result cannot
  qualify C5, nor can a memory-only result qualify command execution.

## Memory and explicit VM oracle

`userland/test_nouveau_memory.c` compiles against the private import's actual
`drm.h` and `nouveau_drm.h`, with ABI-size assertions. It discovers a Nouveau
render node and verifies PCI identity `10de:2803` rather than assuming a minor.

Two rounds each of GART and VRAM exercise:

- GEM_NEW/GEM_INFO geometry and actual domain; 12 KiB + 137-byte requests
  round to 16 KiB on this configuration.
- CPU_PREP/CPU_FINI, full-buffer byte patterns crossing page boundaries,
  alias mappings and a partial base-offset mapping.
- Foreign-file handle rejection (`ENOENT`) and mmap denial (`EACCES`).
- Legacy GEM prevents later VM_INIT on the same file (`ENOSYS`).
- GEM handle deletion and fd closure while mappings remain live; another
  client's allocation/write must not corrupt those surviving mappings.
- Checked unmap/handle/fd cleanup rather than treating ioctl success alone
  as a completed lifecycle.

A fresh file initializes explicit GPU VM before creating BOs. It rejects
overflowing VM_INIT, bad handles and unaligned MAP addresses, then synchronously
maps and unmaps a GART BO, including unmap after deleting its GEM handle.
Partial CPU mapping here starts at the object's base mmap token; interior-token
mapping is not assumed to be supported by DRM's exact-offset lookup.

## Exposed LinuxKPI defect

The first hardware run reached GEM creation and ownership checks, but an
unauthorized mapping incorrectly returned `ENOMEM` instead of `EACCES`.
`linuxkpi_file_mmap_file()` discarded the imported driver's error by returning
the generic MAP_FAILED sentinel; native mmap translated that sentinel to ENOMEM.

The bridge now returns the driver's negative error, ENODEV for a missing mmap
operation and ENOMEM for wrapper allocation failure. Failed mappings drop their
file reference and bridge without publishing a pending VMA. A positive, nonzero
driver return is rejected as EINVAL. `test_kpi_file_mmap_errors.py` exercises
the actual helper, error propagation, allocation failure and success/failure
ownership. Adjacent file-export and devres sanitizer regressions pass.

The first run also retained the previously observed VKMS warm-loop PMM drift
diagnostic; `hardware-copy-final` later still failed strict on a one-page
delta. It remained a release gate until its root-cause fix (warm-loop PMM
drift resolved below). The diagnostic, its two-pass shape and its strict
classification were never relaxed.

## Warm-loop PMM drift resolved (software verification, 2026-10-21)

The intermittent VKMS warm-loop global PMM drift was root-caused and fixed in
the allocator without touching the gate or its classification. After the fix,
eight consecutive software boots (`fix4-r1..r8`) pass the strict boot result
8/8; the warm second-pass settled delta is 0 in every run, and the sysfs
lifecycle baseline is restored exactly (+0 with two runs +1, inside its
unchanged +2-page tolerance).

Root cause — three stacked bookkeeping effects, none of them a leak:

1. The per-CPU heap freelist parked freed slots with their slab bitmap still
   set (up to sixteen per class) and the slow-path prefetch batch over-drew
   up to four extra objects ahead of demand. Balanced loop traffic (512
   alloc/free per pass) therefore kept pulling fresh PMM pages whose boxes
   stayed parked in the cache until a future pop or shrink.
2. Empty slab pages were released only by the OOM-shrink path. Once parked,
   every page the warm cache had ever touched stayed out of PMM between
   measured passes, so the free-count baseline drifted run-to-run.
3. The named object caches kept the same OOM-only empty retention; a single
   mid-pass `kmem_cache_alloc` page slipped the exact-zero requirement
   (`fix2-r2`, pinned through the PMM site table).

Per-class counters and PMM site tables carried the attribution end to end:
the entire drift was the class-1024 slab arena transient
(`tr4-r6`, `tr5-r*`, `fix2-r2`), with loop API traffic exactly balanced.

Fix, in kernel code:

- `kernel/src/mm/heap_slab.c`: removed the per-CPU park/pop fast path and the
  prefetch batch; every object is served from the class slab lists, and a
  slab whose last live slot is freed returns its frame to PMM immediately.
- `kernel/src/mm/slab_cache.c`: the identical immediate empty-page return in
  `kmem_cache_free`; the OOM-shrink paths are retained for the unwind case.
- Dead per-caller attribution hooks (`heap_class_site_*`) were removed. The
  gated per-class `heap_slab_dbg` counters and `pmm_site` dumps remain as the
  diagnostics that produced the evidence chain.

Fix progression in software overlays:

- `fix1-r1..r6`: bounded retention (`HEAP_FREE_EMPTY_KEEP=1`); drift bounded
  16–49 → 0–1 pages; 4/6 vkms gate passes.
- `fix2-r1..r8`: deterministic heap service; 7/8 gate passes; the single miss
  was the named-cache one-page straggler above.
- `fix3-r1..r8`: both allocators deterministic; 8/8 vkms gate passes with
  second-pass settled = 0; strict boot then exposed a new deterministic
  sysfs check failure in 5/8 runs, explained next.
- `fix4-r1..r8`: 8/8 strict pass (closed-core candidate).

New exposure fixed at quiescence, not relaxed: with page-level motion now
real, the sysfs lifecycle check's immediate post-loop PMM sample raced the
deferred node reclaimer (`vfs_rcu.c` queues freed vfs nodes and its drainer
thread waits ~2 ms before reclaiming; 30–47 pages' worth of pending frees).
The parked arena had masked this before the allocator fix. Attribution
(`diag/sysfs-d1..d2`, `diag/sysfs-e1..e3`): the 64-cycle loop pulls 189
pages, ~147–157 frees complete in-thread, and every run reaches the same
conserved quiescent state (191 frees). The test now samples after
`vfs_rcu_synchronize()` — the same convention `vfs.c` and `page_cache.c`
already apply when accounting state after node release. Tolerance and failure
semantics are unchanged; a genuinely retained page still fails.

Hardware re-validation of the fixed allocator is pending (next hardware
session). The earlier hardware evidence stands for its own kernels;
`hardware-copy-final`'s one-page drift failure is explained and fixed by this
work but has not been re-run on hardware.

## Deterministic GPU execution

`userland/test_nouveau_copy.c` reuses the exact memory helpers and the cached
Mesa 26.2.5 NVIF and NVIDIA class headers. Its push layout follows Mesa's copy
engine path, not invented register definitions. Each client:

1. Initializes explicit VM, creates a CE channel, queries supported classes
   and creates the reported `AMPERE_DMA_COPY_B` (`c7b5`) NVIF object.
2. Allocates four 16 KiB BOs: source GART, destination VRAM, readback GART
   and GART pushbuffer; maps them at explicit GPU virtual addresses.
3. Writes the source pattern and distinct destination/readback canaries,
   ending CPU access before GPU submission.
4. Submits two real copy commands via EXEC: 8,256 bytes at nonzero offsets
   from GART to VRAM and back to GART. No CPU memcpy performs these transfers.
5. Waits on the real syncobj fence with a ten-second absolute deadline.
6. Verifies source integrity, both GPU-written payloads and every byte outside
   the destination ranges. Channel, VM, mappings, syncobj and handles are released.

Two separate clients use different seeds. The first uses a binary syncobj.
The second asynchronously binds the push BO and signals timeline point 1;
EXEC explicitly waits on point 1 and signals point 2. A timeline wait and query
must confirm point 2, and an unsubmitted point 3 must time out with ETIME.
This validates accepted dependency ordering and GPU output without a CPU wait
between bind and execution; it does not prove the bind was still pending when
the EXEC ioctl began.

## Evidence and reproduction

Evidence under `build/nouveau-c5/` retains inputs, commands, serial, host-kernel
before/after, QEMU reset traces and PCI-health snapshots:

- `hardware-first/`: original foreign-mmap errno failure, no lost PCI access.
- `hardware-mmap/`: strict memory/VM pass after the bridge correction.
- `hardware-copy/`: strict two-client deterministic GPU-copy pass.
- `hardware-timeline/`: strict binary plus async-bind/timeline dependency pass.
- `readiness-memory-final/`, `readiness-copy-final/`: final oracle qualification
  with cleanup results mandatory.
- `hardware-memory-final/`: final memory oracle and strict boot pass.
- `hardware-copy-final/`: final copy, async timeline, all cleanup and PCI checks
  pass; strict result fails the existing one-page VKMS PMM drift diagnostic
  (now root-caused and fixed, see above). Exact results and source hashes are
  retained in `assessment.json`.
- `fix1-r1..r6/`, `fix2-r1..r8/`, `fix3-r1..r8/`: software drift-fix
  verification chain (per-class counters + PMM site dumps in every serial log).
- `fix4-r1..r8/`: closed-core candidate verification — eight consecutive
  strict software boots on the fixed kernel
  `c5e8c9fed4ca8610ef038a898b3e38a543e80caa6ff083c14a185c39d49d7780`.
- `diag/sysfs-d1..d2`, `diag/sysfs-e1..e3`: sysfs lifecycle drain-race
  attribution; `diag/resolve_callers.py` maps site tables to symbols.

Prepare/readiness uses no GPU assignment:

```sh
python3 scripts/nouveau-boot.py --c5-memory --software --run \
  --pcie-root-port --gpu-only --cpus 1 \
  --kernel build/nouveau-618/kernel --firmware-dir build/nouveau-618/firmware \
  --output-dir build/nouveau-c5/readiness-memory-final
```

Use `--c5-copy` for execution qualification. Hardware uses the same oracle flag,
`--run --gpu 0000:01:00.0 --host-on-igpu --remove-on-shutdown`, the matching
`--readiness .../result.json` and a distinct output directory. Stop on unreadable
PCI state; never assign the audio sibling as an attempted recovery.

Host regressions:

```sh
python3 scripts/tests/test_nouveau_memory_oracle.py
python3 scripts/tests/test_nouveau_copy_oracle.py
python3 scripts/tests/test_kpi_file_mmap_errors.py
```

## Remaining C5 gates — deferred (recorded scope decision 2026-10-21)

At closure the project owner chose to close the C5 core once every recorded
bug was fixed and verified, deferring the remaining unimplemented oracles.
They are **deferred known gaps** — no passing evidence exists for them, and
none is claimed by the runs above:

- Controlled memory pressure, eviction/migration and allocation-failure unwind.
- Explicit zeroing-on-reuse and broader malformed mapping/synchronization cases.
- Concurrent processes and process exit with demonstrably outstanding GPU work.
- Post-quiescence resource accounting beyond successful per-object destruction.
- Larger/repeated workloads and additional asynchronous dependency/error cases.
- Hardware re-validation of the fixed allocator (software-verified 8/8 strict
  only).

The successful copy/VM tests do not silently satisfy these untested gates, nor
the carried-forward physical failed-probe and SMP/AMD qualification work.
