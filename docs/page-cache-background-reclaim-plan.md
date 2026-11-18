# Background page-cache reclaim (kswapd-style) — implementation plan

Status: proposal, not started. Decisions needed are in [Policy decisions](#policy-decisions-to-settle).

## Goal

Keep free memory above a watermark without waiting for an allocation to fail.
Release clean file-cache pages when free memory drops below the low mark, and
write dirty pages back in the background so they can be released too.

## Where things stand

- **Reclaim triggers today.** Page-cache reclaim runs only when a PMM allocation
  fails (`heap_oom_reclaim` → `mm_file_cache_reclaim`), or on `drop_caches`.
  The idle trim (30 s, added with this work) is a timer, not a pressure signal.
- **Reachability.** `mm_file_cache_reclaim` covers the caller's mappings, the
  dentry cache, and every file in the page-cache registry
  (`kernel/src/fs/page_cache.c`, `vfs_pc_reclaim_walk`).
- **Dirty pages are rare.** Writes are write-through (`vfs_write` updates the
  cache clean). Dirty pages come only from shared-mapping teardown
  (`kernel/src/mm/vmm_free.c:194`) and are flushed by `vfs_cache_sync` on
  last close, `fsync`, or `sync`. Dirty pages are never reclaimed.
- **A pressure signal exists.** `zram_under_pressure()` compares
  `pmm_get_free_pages_including_pcp()` to `low_pages` (`ram / 16 pages`, clamped
  to 4096–32768 pages) and drives a worker thread
  (`kernel/src/mm/zram/zram.c`, `worker()`). It's the closest precedent for the
  reclaim thread and should be reused in shape.
- **Accounting.** `MemAvailable` already counts page cache as reclaimable
  (`kernel/src/fs/procfs.c`). Once background reclaim exists, `MemAvailable`
  stays honest without changes.

## Design

### 1. Watermarks

- Derive `low` and `high` from usable RAM, the same way zram derives `low_pages`.
  Free pages are counted with `pmm_get_free_pages_including_pcp()`.
- Reclaim starts below `low` and stops at `high`. Two marks give hysteresis, so
  the reclaim thread doesn't flap around one threshold.
- The thread only sleeps while free memory is above `high`. It polls every
  `250 ms` when idle and every `5 ms` under pressure, matching zram's worker.

### 2. Background reclaim thread (`kswapd`)

- New kernel thread, started next to `vfs_pc_trim_init()`.
- Each pass calls the existing registry walk with a page target equal to
  `high - free`, without the idle filter. Pages are taken from the least
  recently touched nodes first (`pc_last_ms`), so the walk doesn't evict the
  hottest files.
- Reclaim only clean, unmapped pages. Dirty and mapped pages are skipped, as
  they are today.
- The thread never blocks an allocator. Allocators only wake it.

### 3. Wake-up from the allocator

- When `pmm_alloc` falls below `low`, wake the thread. Don't reclaim inline.
- Keep `heap_oom_reclaim` as the last resort. It still runs on a failed
  allocation, so the synchronous path is unchanged.

### 4. Background writeback

- A second thread, or the same one on alternate passes, writes dirty pages back.
- Use `vfs_cache_sync` on nodes that have dirty pages, one node at a time, with
  no locks held. Writes already batch per node inside `vfs_cache_sync`.
- Expire rule: write a page once it has been dirty longer than the expire age.
  Keep the expire age a constant, not a guess.
- Pages become clean after the write completes, so the next reclaim pass can
  take them.

### 5. Ordering against other reclaim

- Clean page cache first, then slab shrink (`kmem_cache_shrink_all`), then
  thread stacks, then zram swap-out of anon memory. Cache is cheapest to
  re-create, so it goes first.
- The idle trim stays. It's deterministic and cheap, and it handles the
  pattern that started this work. Background reclaim handles pressure.

## Phases

1. **Observability.** Add counters for reclaim passes, pages reclaimed by source
   (idle trim, background, OOM, `drop_caches`), and pages written back. Expose
   them through `/proc/vmstat` or the existing `/proc/meminfo` block. This is
   the first thing to land, because every later phase needs it to verify
   behaviour.
2. **Watermarks and the reclaim thread.** Clean-page reclaim only, no
   writeback. Test with the guest harness.
3. **Allocator wake-up.** Wire the low-watermark check into the PMM allocation
   path. Keep it lock-free and cheap.
4. **Background writeback.** Add the dirty-page pass once clean reclaim is
   verified.
5. **Tuning.** Watermarks, expire age, and the ordering against zram, measured
   on the desktop image.

## Risks

- **Lock order and deadlock.** Writeback calls into filesystems, which may need
  memory or locks held by the allocator. Mitigate by never running writeback
  from an allocation path, keeping the writeback thread out of the
  low-memory fallback, and holding no kernel locks across `vfs_cache_sync`.
- **Refault storms.** Reclaiming too hard drops files the desktop is about to
  use. Hysteresis and the least-recently-touched ordering limit this. Watch
  `pgfault` against reclaim counts.
- **Writeback latency.** A slow disk can keep dirty pages around under pressure.
  Writeback has to rate-limit itself so it doesn't saturate I/O.
- **CPU cost.** Walking the registry under pressure costs CPU. The walk is
  already bounded per pass.
- **Interaction with zram.** Both respond to the same pressure. Their order
  decides whether anon pages get swapped while cache is still reclaimable.

## Test plan

- **Harness.** Reuse the headless guest harness in the scratchpad
  (`drive_test.py`). It boots `snapshot=on` and never writes `disk.img`.
- **Idle and watermark behaviour.** Read a set of files, let the watermark
  trigger, and check `Cached` falls toward the high mark without OOM.
- **Pressure.** Run a memory-consuming workload and confirm cache is released
  before zram swap-out begins, and that no allocation fails.
- **Writeback.** Create dirty shared mappings, unmap, wait past the expire age,
  and confirm the pages are written and then reclaimed.
- **Regression.** Re-run the 50× `fastfetch` test and the idle-trim check.
- **Blocker.** `drop_caches` needs root, and the guest `avory` user can't run
  it. Either add a root test path to the harness or allow the write for the
  test image. This blocks verifying Phase 1.

## Policy decisions to settle

1. **Watermark levels.** Recommend `low` = 10% free, `high` = 15% free, based
   on usable RAM, with zram's absolute floors kept as minimums.
2. **Dirty expire age.** Recommend 30 s, the Linux default, with a 5 s
   writeback wake interval.
3. **Reclaim order against zram.** Recommend cache first, zram second, as
   above. The alternative is to swap anon pages earlier, which keeps the cache
   warmer but costs more CPU per fault.
4. **Slab shrink.** Recommend running it only below `low`, since it's
   heavier than cache reclaim.
5. **Keep the idle trim.** Recommend yes. Its 30 s window is a separate
   policy from pressure-driven reclaim, and it covers the pattern that started
   this work.
