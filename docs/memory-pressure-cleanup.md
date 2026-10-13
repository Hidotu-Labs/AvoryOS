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
