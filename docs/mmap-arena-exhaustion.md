# File-backed mmap arena exhaustion (Chrome `[MMAP-FAIL]`)

A Chrome session produced sixteen `[MMAP-FAIL]` lines whose `cursor` field was
within ~0x101000 of `0x7FF000000000` in three different processes
(`Chrome_ChildIOT`, `ThreadPoolForeg`, `Compositor`). The diagnostic prints
`mm->mmap_next_addr` when `mmap` returns `-ENOMEM`; that value is the
high-water mark of the file-backed arena `[0x7F0000000000, 0x7FF000000000)`
(960 GiB, `sys_mm.c:MMAP_REGION_*`). In every process the watermark had
reached the top of the arena.

## Two defects, one terminal state

Physical memory was not the cause: no page is reserved for a failed mapping,
and the memfd/shared-memory mappings involved cannot total 960 GiB on the
guest RAM size.

1. **Failed file-backed mmaps kept their reservation.** For a non-fixed
   file-backed request, the selected range is committed to `mmap_next_addr`
   before the descriptor is looked up and before the mapping is installed
   (`sys_mm.c`, "a failed mmap may leave a harmless hole"). A call that
   returned EBADF/EINVAL or whose handler failed still advanced the cursor by
   the full length. The `[MMAP-FAIL]` `fd` values
   (`0xffffffff00000026`…) are Chromium's shared-memory mapper passing an
   FDPair with unrelated bits in the upper half; before the `fd = (uint32_t)fd`
   conversion in the same working tree, `fd_node_get()` rejected them as
   `>= MAX_FDS`, so every such shared-memory mmap returned EBADF *and burned
   64 KiB–2 MiB of arena*. That is how ~960 GiB disappeared without a single
   early ENOMEM.

2. **The monotonic cursor made the state permanent.** Non-fixed file-backed
   mappings only searched `[cursor, MMAP_REGION_LIMIT)` and had no
   lower-address fallback (anonymous mappings do). `munmap` never lowers
   `mmap_next_addr` either. Once the watermark reached the limit, every
   file-backed mmap — Chrome's tiles, canvases, shared buffers — failed
   `ENOMEM` forever even with gigabytes free below.

Evidence from the host harness that compiles the real `sys_mmap`:

```
BURN-REPRO: file mmap with bad fd -> -9, vmas 0->0, cursor advanced to 0x7f0000001000
EXHAUST-REPRO: file-backed mmap with free hole at BASE+0x1000 -> -12, vmas=2
```

`sys_mremap` already searches the whole arena under `mm->lock` (its handler
runs while the lock is held), so reuse below the watermark is not new policy;
the mmap path had no safe way to do it because its file-backed handler runs
after `mm->lock` is dropped.

## Fix

`sys_mmap` now falls back to a whole-arena gap search when the high-water
search cannot fit the request. The file-backed handler still runs outside
`mm->lock`, so the chosen gap is reserved by publishing a placeholder VMA
(`PROT_NONE`, private anonymous) under the lock that found it. The real VMA
replaces the placeholder in the same critical section that installs it, on
both the driver-handler and page-cache paths, and every failure path
(`fd_node_get` miss, handler error, non-file node, unaligned offset,
descriptor-less non-anonymous request) calls `mmap_gap_release()` so a
rejected mmap returns the gap instead of stranding it. A reused interior gap
does not move the cursor. The fast path above the watermark is unchanged.

## Validation

- `python3 scripts/tests/test_mmap_fixed_reservations.py` (extracts and
  compiles the production `sys_mmap` and `mmap_gap_release`): new cases cover
  a watermark pinned at `MMAP_REGION_LIMIT` with an interior gap for the
  page-cache path and for a driver handler, placeholder replacement, and
  release after EBADF and after a handler error. Existing Wine reservation
  behavior, fixed overlap/teardown, compat32 bounds and failure paths still
  pass.
- `python3 scripts/tests/test_kpi_vmap_lifetime.py` and
  `test_kernel_shared_vmap_tables.py` still pass.
- The rebuilt kernel contains `mmap_gap_release` and boots in the QEMU
  `run-c7-prime` capture. A guest repeat of the Chrome workload should show
  either no `[MMAP-FAIL]` lines or a burst that recovers on later mmaps
  instead of the terminal state above.

## Remaining

- Device handlers (`memfd_mmap`, `drm.c`, `ext2_file`, `pty`) still allocate
  VAs through `mm_alloc_mmap_region()`, which shares `mmap_next_addr` and has
  the same no-reuse behavior; they are reached from their own ioctl paths
  when no address was supplied. That allocator needs the same treatment if
  those paths ever exhaust the arena.
- The 960 GiB arena is smaller than Linux's usable mmap space, but with gap
  reuse it is no longer a lifetime limit; the guest workload repeat will
  decide whether a larger arena is still warranted.
