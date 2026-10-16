# Memory exhaustion after Roblox joined

The user's later capture shows repeated 1,024-byte NVMe bounce allocation
failures, ext4 metadata allocation failure, and demand-page failures with
`free_pages=0 free_pages_pcp=0`. The window manager then aborts because it
cannot create a thread. This establishes physical memory exhaustion, rather
than an isolated NVMe allocation-size limit. It does not identify how much
memory is held by each process, cache, or leak. The configured RAM is unchanged.

Two cleanup defects were found and corrected:

- `nvme_io_bounce` freed its allocation only when its final return value was
  zero or the whole controller was quiesced. A copy fault, failed PRP build,
  or completed command error consequently leaked the allocation even though
  no device transfer could still use it. The path now tracks DMA safety
  independently of the operation's success. Transport failures still retain
  memory unless the controller has stopped, preserving DMA safety.
- `mm_file_cache_reclaim` pinned up to 32 unique file nodes but stopped the
  release loop once its page target was met. Later nodes retained references
  without owners. Every collected pin is now released, including nodes that
  were not reclaimed.

NVMe allocation-failure messages now report the first eight failures and
subsequent powers of two, keeping repeated shortages visible without the
unbounded console flood in this capture.

`scripts/tests/test_memory_pressure_cleanup.py` executes the actual cleanup
functions under ASan/UBSan with allocator and reference counters. It checks
1,000 completed error statuses, copy-in/out faults, successful reads, safe
shutdown cleanup, retention while DMA remains unsafe, and early-target
release of all file pins. Each saved previous implementation independently
fails its corresponding regression assertion. The existing NVMe wait
registration regression also passes. LeakSanitizer is disabled because it
cannot run under this sandbox's tracing; explicit counters check ownership.

These are proven leaks, but the capture does not prove they consumed all
guest RAM. Further process/cache accounting is still needed to find other
growth. The current demand-page OOM handling terminates one worker instead
of the whole shared address-space group; that policy remains a separate
issue for the planned memory audit. No RAM increase or guest disk edit was
made for this change.

## Firefox multi-tab pressure at 4 GiB

The later capture reports both free_pages and free_pages_pcp as zero before
NVMe bounce allocation failures, SQLite cache reads/writes failing, and a
forkserver crash after clone returned ENOMEM. vfs_cached_pages is approximately
204,264 (about 798 MiB). That total includes mapped pages and is not a promise
that all of it can be reclaimed. The user reproduced this after opening several
Firefox tabs and requested that the VM remain at 4 GiB.

The old pressure hook only searched at most 32 unique cached files mapped into
the current mm. It could not reach clean cache pages kept by dentry references
elsewhere, including files no longer mapped in that process. A bounded dentry
scan now pins regular-file children under try-locked buckets and reclaims after
releasing the bucket. It releases every pin and skips recursive global scans.
Page-cache reclamation itself now uses a try-lock, avoiding self-deadlock when
an allocation fails while the caller already owns that node's cache lock.

Private tmpfs mappings use MAP_PAGECACHE but their nodes lack FS_PAGE_CACHE.
Current-mm reclaim and process-exit cleanup now recognize that VMA flag as
well as the node flag. Tmpfs writes and truncation update/invalidate cache copies
used by private mappings, without routing ordinary tmpfs reads through a cache
that could become stale after direct shared-mapping writes.

NVMe bounce allocation makes one bounded reclaim/drain/retry attempt when
blocking is permitted. It does not reclaim with interrupts masked. Draining PCP
pages makes released frames available to the contiguous buddy allocation.
A still-failing request returns an error as before; unsafe DMA buffers retain
the existing ownership rules.

Validation:

- test_global_cache_reclaim.py executes the real global scan, per-node reclaim
  and process-exit cache helper. It checks cold files outside the current mm,
  dirty/loading/writeback/mapped exclusions, bucket/page-lock contention,
  recursion, duplicate nodes, target limits, reference release, and tmpfs
  private-cache cleanup after PTE teardown under ASan/UBSan.
- test_memory_pressure_cleanup.py checks NVMe recovery after an injected first
  allocation failure, no recovery in nonblocking context, existing bounce/DMA
  cleanup, and inclusion of private tmpfs VMAs in current-mm reclamation.
- test_download_preallocation.py also checks tmpfs truncation invalidates its
  private mapping cache. mmap reservation/tmpfs dispatch and NVMe wait tests
  remain passing. Kernel and regular ISO builds pass.

No QEMU RAM setting was changed. A guest repeat of the original tab workload
and before/after process/cache accounting are still required to quantify the
remaining memory growth or establish that this eliminates every OOM event.

## LinuxKPI allocation failure relief and free-list tags (2026-10-15)

The 2026-10-15 desktop capture
(`build/logs/prime-runs/20261015T113656Z-hi5x707j`) establishes the abort
chain for the reported Xorg crash: PMM free fell to 291 pages, an order-0
`GFP_KERNEL` LinuxKPI allocation failed, `amdgpu_cs_parser_bos()` returned
`-ENOMEM` (`Not enough memory for command submission!`) and Mesa aborted
`Xorg:cs0` with SIGABRT. A rejected CS is immediately fatal to the client.
A second capture (`20261015T110004Z-1bkdm8tc`) died the same way from
fragmentation: TTM pool refills failed from order 9 down to order 2 (reported
free 693-1319 pages with no contiguous run even for 4 pages) and a later BO
allocation failed the submission.

LinuxKPI allocations bypass the native PCP/zero pools, and the `alloc_pages()`
failure path only drained shrinker-registered caches. Three changes close the
gap:

- `kernel/linuxkpi/src/page.c`: on failure, callers that permit reclaim
  (`__GFP_DIRECT_RECLAIM` set) now get parked-frame drains (PCP + zero pool),
  shrinker reclaim and clean file-cache reclaim (64-512 pages), then one
  retry. Callers that forbid reclaim (`GFP_ATOMIC`/`GFP_NOWAIT`) get no
  relief; previously the sleeping shrinker could run in their context.
- High-order TTM refills carry `__GFP_NORETRY|__GFP_RETRY_MAYFAIL`. The
  LinuxKPI headers resolve to the upstream `gfp_types.h` values, where
  `__GFP_NORETRY` is `0x10000` (the `src/include` copy's `0x8000` is not what
  this translation unit sees). Those requests now still get the cheap
  parked-frame drain and the single retry — merged parked frames are what
  lets a high-order request succeed — but not the sleeping reclaim.
- `kernel/src/mm/vmm_fault.c`: the fault OOM rescue drains the whole PCP
  instead of only the zero pool.

Buddy free-list headers now carry a per-boot tag (`kernel/src/mm/pmm_internal.h`,
`pmm_buddy.c`, `pmm_init.c`): an rdtsc-derived zone tag is stamped on list
insert, cleared on unlink and on every returned block (covering split paths
whose retained half's header is not rewritten), and required by the free-list
validity check. Boot 2 of the crash capture logged eight
`[PMM] CORRUPTION: double free of linked block` reports during the LinuxKPI
self-test: stale headers from boot 1 satisfied the validity heuristic, the
first frees were dropped, and the pages were silently lost (free in the
bitmap, absent from every list, so the free counters inflate while the pages
cannot be allocated). The tag makes stale headers fail the check.

Validation: kernel and ISO build clean; `test_kpi_page_physical_bounds.py`
and `test_global_cache_reclaim.py` pass (`test_pmm_references.py` fails
identically with these changes stashed — pre-existing fixture ordering). A
headless KVM two-boot warm-reboot check (boot 1 -> `system_reset` -> boot 2)
passed the page self-tests and free-count invariant on both boots with zero
corruption reports and zero allocation failures. The headless re-check of
the final revision could not run while a desktop VM held `disk.img`. The
acceptance test is a full KDE Plasma 6 + Firefox session at 4 GiB; the
high-order path depends on probabilistic parked-frame merging. No RAM
setting or guest disk was changed.

## Chunked mem_map and demand-fault reclaim (2026-10-16)

The 2026-10-16 desktop capture
(`build/logs/prime-runs/20261016T110831Z-fwp9wppy`, serial lines 374-393)
shows the two failure classes side by side. Eight `[KPI-PAGE] alloc_pages`
failures at orders 9, 8 and 7 occur while buddy free is 7,666-9,257 pages:
the machine is fragmented, not exhausted, and the failing requests are TTM
pool refills. Each `KPI-PAGE` failure makes `pfn_to_page()` return NULL for
the target frame, so `alloc_pages()` frees the frame it had just obtained
and returns NULL; `DRM_IOCTL_AMDGPU_GEM_CREATE` then fails
(`GPUDBG PRIME stage=1 error=-12` is `GPU_PRIME_SOURCE_ALLOC` in
`kernel/linuxkpi/src/file.c`). Separately, two `[VMM] OOM` demand-page kills
fire at `free_pages=0` and `free_pages=40` — true exhaustion arrived before
any reclaim did.

Two root causes were fixed:

- `kernel/linuxkpi/src/page.c`: the sparse mem_map previously demanded one
  order-9 (2 MB) buddy block per newly touched 128 MB section. That
  allocation was the order-9 consumer in the capture, and its failure broke
  page-model translation for the whole section. Descriptor storage is now
  chunked: a section owns a lazily allocated one-page table of chunk
  pointers, and descriptors live in order-0 chunks of 64 (one page each).
  `pfn_to_page()` gained a two-level lookup (`section -> chunk -> slot`);
  `_Static_assert`s keep the table and chunk within one page as the
  descriptor grows. No metadata allocation in the page model is above
  order-0 anymore.
- `kernel/src/mm/vmm_fault.c`: reclaim previously started only after a fault
  allocation had already failed. A user fault now checks a low watermark
  (2,048 pages / 8 MB, 250 ms rate limit, up to 4,096 pages per pass) before
  allocating and drops clean cache plus drained PCP/zero-pool frames when it
  dips below, so the allocator keeps reclaimable headroom during a
  Firefox/KDE burst instead of first reaching zero. If a 4 KB anon fault
  still cannot get a frame, the rescue path now runs an escalated pass
  before declaring OOM: `heap_oom_reclaim()`, the LinuxKPI shrinker registry
  (`linuxkpi_shrink_reclaim(0, 512)`), a 2,048-page file-cache reclaim and a
  PCP drain, all with interrupts enabled around the sleeping reclaim, then
  one retry. Failing that, fault OOM now kills the whole thread group
  (`sched_terminate_thread_group()` plus the faulting thread) instead of a
  lone thread, matching the fatal-signal default action and the existing
  kill-path precedent in `kernel/src/cpu/isr.c`.

Validation: kernel build clean; `test_kpi_page_physical_bounds.py` was
rewritten for the chunked model (64-byte descriptor mirror, all metadata
allocations order-0, per-chunk laziness and boundary pages, high RAM above
the PCI hole, bounds) and passes together with `test_file_cache_cow.py`,
`test_kpi_driver_fault_result.py`, `test_kpi_shmem_lifetime.py`,
`test_kpi_gpu_kernel_correctness.py`, `test_lazy_fork.py` and
`test_memfd_private_views.py`, the source-slicing tests that read the edited
files. The acceptance test remains a full KDE Plasma 6 + Firefox session at
4 GiB; the headless re-check needs `disk.img` free of a running desktop VM.
No RAM setting or guest disk was changed. If fragmentation ever persists
for non-mem_map high-order consumers, a dedicated order-9 reserve pool is
the noted follow-up.

## Chunk-boundary descriptor walks (2026-10-16 regression fix)

The chunked mem_map above broke amdgpu init in
`build/logs/prime-runs/20261016T112330Z-1gg14upg`: eight
`[PMM] CORRUPTION` free-list reports (serial lines 502-557) plus PSP
`LOAD_IP_FW` failures (`0xFFFF3072` then `0xFFFF0008`), a
`gc ucode autoload timeout`, `hw_init of IP block <gfx_v10_0> failed -110`
and a KPF thread fault in teardown.

Root cause: a chunk holds only 64 descriptors (`CHUNK_PAGES`), and chunks
are independent order-0 pages, so `struct page` descriptors are no longer
contiguous across a chunk boundary. `page_set_order()`,
`page_set_order_plain()` and `split_page()` still walked `&head[i]` over
all `1 << order` pages of a block. For any block whose descriptors crossed
a chunk end, the walk wrote descriptor fields into the physical page that
follows the chunk's storage page: `__ClearPageTail/__ClearPageHead()` bit
clears at +0, `atomic_set(&p->_refcount, 1)` at +12, `_mapcount = -1` at
+16, `compound_head` at +24. Every corrupted report decodes exactly that
way, and every corrupted node was the physical neighbor of a chunk storage
page that the PMM history shows being allocated for metadata:
`0x6D0000`→`0x6D1000`, `0x75C62000`→`0x75C63000`,
`0x75C6A000`→`0x75C6B000`, `0x75C74000`→`0x75C75000`, etc. The free-list
"next"/"prev" values were not stale data at all: `next` was the list head
after PG_tail (bit 21) and PG_head (bit 6) were cleared in its low dword,
and `prev` was the same head with its high dword replaced by the
`_refcount = 1` write. The wild writes also landed on live pages during
amdgpu firmware upload, which is why PSP/GC init failed; the teardown KPF
fault in `amdgpu_ttm_tt_unpopulate()` is downstream of the failed init.

Fixes:

- `kernel/linuxkpi/src/page.c`: new `page_block_entry(head, index)` helper
  resolves the descriptor for `index` pages into a block through
  `head->pfn` + `pfn_to_page()` whenever the step leaves `head`'s chunk
  (pointer arithmetic is kept for the intra-chunk fast path). All three
  block-metadata writers now step through it, so a block that crosses a
  chunk boundary only ever touches its own descriptors. A
  power-of-two `_Static_assert` documents the XOR trick's requirement.
- `kernel/linuxkpi/include/linux/mm.h`: `nth_page(page, n)` used to be
  `(page) + (n)`; SG iterators (`sg_page_iter_page()` walks scatterlist
  pages) would have read descriptors from the wrong physical page after a
  chunk boundary. It now resolves through
  `pfn_to_page(page_to_pfn(page) + (n))`, matching the definition in
  `kernel/src/include/linux/page.h`.

Validation: kernel build clean; new
`scripts/tests/test_kpi_page_chunk_walk.py` slices the real helpers into a
host harness whose chunk allocator hands out independent pages (a raw
pointer walk therefore corrupts heap instead of the expected descriptor),
drives a block across a chunk boundary and checks `page_set_order()`,
`page_set_order_plain()` and `split_page()` write only real descriptors;
it also source-guards the three writers and the `nth_page()` definition.
It passes with `test_kpi_page_physical_bounds.py` and the other
source-slicing tests. Acceptance remains `make run-c7-prime`: no
`[PMM] CORRUPTION` reports and amdgpu reaching `amdgpu_initialized=1`.

### Second wave: TTM pool walks (same class, other consumers)

The first wave removed every `[PMM] CORRUPTION` report (runs
`20261016T121247Z-*` and `20261016T121410Z-*` log `corr=0`), but the PSP
`LOAD_IP_FW` failures and the teardown KPF remained. Two more walks had
the same shape:

- `ttm_pool_page_allocated()` filled `ttm->pages[]` with
  `for (i = 1 << order; i; --i, ++(*pages), ++p) **pages = p;`. For any
  block crossing a chunk boundary, every entry past the chunk end was a
  pointer into the physical page that follows the chunk storage, not a
  page descriptor. TTM allocates most multi-page blocks that way, so the
  amdgpu firmware BO (GTT, `psp->fw_pri_bo`/the ucode BO) ended up with a
  page array that no longer described its backing pages.
  `amdgpu_bo_kmap()` maps system-memory BOs through
  `ttm_kmap_iter_tt_map_local()` → `kmap_local_page(tt->pages[i])`, so the
  firmware `memcpy` wrote the ucode into whatever PFNs the neighboring raw
  bytes happened to decode to, and the PSP read unwritten pages back:
  every `LOAD_IP_FW` returned `0xFFFF3072`/`0xFFFF0008`. The
  populate/unpopulate loops writing `ttm->pages[i]->mapping` at `+0x28`
  wrote into those same unrelated physical pages.
- `ttm_pool_type_give()` cleared blocks returned to the pool with
  `clear_page(page_address(p + i))`; past a chunk boundary that decoded
  the neighboring raw bytes as a PFN and zeroed a full 4 KB of unrelated
  memory.

Those continuing wild writes are what poisoned the page array that
`amdgpu_ttm_tt_unpopulate()` walked in teardown — the KPF reported
`pages[77] == NULL` at `amdgpu_ttm_tt_unpopulate+0x4B`. It killed the
`kpi/initcalls` thread before `amdgpu_device_fini` returned, so the boot
parked at "initcalls still running" and the device managers never started
(the user-visible "device manager doesn't work", downstream of the same
class of bug).

Fix: both loops resolve every descriptor through `nth_page()` (the
PFN-based definition fixed above). The amdgpu GART/DMA paths need no
change: they consume `ttm->dma_address[]`, which `ttm_pool_map()` fills
from the block's head page.

Validation update: `test_kpi_page_chunk_walk.py` now also source-guards
both TTM pool loops against `++p`/`p + i` and models the `nth_page()`-based
block fill across a chunk boundary.

`kernel/linux/` is a gitignored vendored tree re-created by
`scripts/linux-import.sh`, so the `ttm_pool.c` fix is recorded as a
divergence patch, `scripts/linux/patches/0003-ttm-pool-chunked-memmap-walks.patch`,
which the import script reapplies (verified to apply cleanly to a pristine
upstream copy and reproduce the fixed file byte-for-byte).

Acceptance met: run `20261016T123758Z-gvzmy9v1` boots with zero `failed to
load ucode` lines, no `[KPF]`, no `[PMM] CORRUPTION`, no `[VMM] OOM` kills,
and proceeds past initcalls into KDE userspace where the earlier failure
parked the boot.
