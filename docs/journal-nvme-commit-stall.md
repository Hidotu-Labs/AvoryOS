# NVMe journal commit stalls

Date: 2026-10-15. Host regressions and ISO build pass; guest confirmation is
pending. The existing VM was left running and its disk image was not changed.

The reported waits identify one owner (`systems:disk$1`, tid 751), two metadata
records, and an outer transaction hold growing from 5.1 to 16.0 seconds in
`commit`. The counters remain unchanged between those observations. Writers
share a mount-wide journal, so one delayed commit delays other mutations.
The user confirmed this is a VM with an NVMe disk. These lines do not identify
the blocked instruction or prove a device timeout. `submitted` and `unchanged`
are cumulative mount counters, not this transaction's write count.

## Changes

- NVMe command waits now poll for up to 20 microseconds before sleeping when
  sleeping is allowed. Previously one unsuccessful CQ drain immediately entered
  the wait queue. Small synchronous journal writes can now complete without
  scheduler handoffs. The budget uses calibrated TSC frequency, with a 100,000
  cycle fallback before calibration; it never restarts after a wakeup. Timeout,
  error, reset, and IRQ-masked polling behavior are preserved. This trades a
  bounded amount of CPU polling for potentially lower request latency; no guest
  speedup has been measured yet.
- Ext2/ext4 block-cache buffers are allocated outside `cache_lock`. Previously
  reads and single/batched writes called `kmalloc` under that IRQ-masking lock.
  Allocation failure can enter file-cache/dentry reclaim and acquire other
  filesystem locks. A competing installer is rechecked under the cache lock;
  its losing allocation is freed outside the lock. Failed optional allocations
  still allow device I/O. Cache versioning and collision handling are retained.
- Commit diagnostics distinguish metadata flushing, scratch allocation, journal
  mapping, descriptor write, commit record, and journal-superblock read,
  publication, and clearing. Any remaining wait should identify which step
  needs investigation.

These changes address concrete latency/locking hazards; they do not establish
that the specific 16-second stall is resolved. Journal ownership and existing
I/O ordering are unchanged. This does not implement asynchronous/group commit
or address the broader durability issues in `journal-performance-plan.md`.

## Verification

- `test_nvme_wait_registration.py`: actual wait functions with controlled
  completions; quick completions avoid sleep, slow completions exhaust the
  budget and sleep, registration races remain safe, and error/reset/timeout
  and nonblocking paths retain their results. The old driver fails the new
  polling assertion.
- `test_ext2_cache_coherence.c`: actual block-cache implementation, forced
  allocator reentry into the same cache lock, allocation failure on reads and
  both write paths, competing buffer installation with allocation accounting,
  overlapping I/O/cache collisions, and 8,000 neighboring inode updates. The
  old source fails the allocation-under-lock assertion.
- Existing actual-source journal, ext2 write-batch, ext4 sparse-write,
  memory-pressure cleanup, storage-performance accounting, and user-copy
  regressions pass with AddressSanitizer/UndefinedBehaviorSanitizer. The
  user-copy fixture needed the existing `mmap_shared_only` VFS field added to
  its mock node before it could compile.
- `make -j8 avoryos-x86_64.iso` succeeds. The linker still reports its existing
  RWX LOAD-segment warning.

Reboot the VM into the rebuilt ISO and repeat the desktop workload that caused
the waits. If it stalls again, collect the new `[JBD2] transaction wait` lines,
including their more specific `phase`, and any NVMe timeout/error messages.
Compare `/bin/test_journal_speed DIRECTORY 100 4 5` and `/proc/storage_perf`
snapshots under the same guest/device settings before claiming a speedup.

## Follow-up: metadata-home stalls

The next guest report held a single metadata home write for 5.2–12.4 seconds,
with owner `alacrit:disk$0`, depth 3, and one recorded block. The current ELF
maps the reported owner address to `ext4_create_impl`. This phase covers cache
preparation, publication, device submission/wait, and cache completion; the log
alone cannot identify which instruction was blocked. The unchanged counters are
cumulative metadata counters, not NVMe submission/completion counts.

NVMe previously required a kernel buffer to be 512-byte aligned to take its
direct DMA path. Heap buffers commonly have a non-sector-aligned offset, causing
every journal payload/home write to allocate a bounce page and copy data. The
PRP builder already handles a first-page offset and maps following pages
individually. The direct path now accepts dword-aligned kernel buffers, and the
PRP builder rejects misalignment in the low two bits. User buffers and genuinely
unaligned buffers retain bounce staging. Disk LBA/length alignment and 4Kn
read/modify/write handling are unchanged.

The alignment follows the NVMe PRP offset requirements (bits 1:0 zero):
https://nvmexpress.org/wp-content/uploads/NVM-Express-Base-Specification-2_0-2021.06.02-Ratified-4.pdf

`test_nvme_metadata_dma.py` compiles actual routing, chunking and PRP construction
code, substituting only the privileged CR3 read and the physical translation/DMA
boundary. It verifies all 1024 dword offsets in a page on fragmented physical
pages, two-page and PRP-list mappings, and 10,000 4KiB metadata writes at a heap
style +32-byte offset without a bounce call. It also checks user/unaligned
fallback, transfer failures, chunking, and 4Kn edges under ASan/UBSan. The previous
alignment rule fails the test. This establishes allocation/copy removal; it does
not measure elapsed guest latency or prove the full 12-second stall is fixed.

Home writes now expose `metadata-home-cache-alloc`,
`metadata-home-cache-publish`, `metadata-home-io`, and
`metadata-home-cache-finish` through the existing transaction phase pointer.
This adds no mount or journal structure fields. Actual cache/coherence tests
cover the phase transitions and errors. Journal ordering, nested transactions,
concurrent owners, recovery mappings, and commit error tests continue to pass.
NVMe wait/timeout, storage accounting, sparse-write and user-copy tests also pass.
Audio, fcntl and the AMDGPU cleanup KPF fix are unchanged.

## Follow-up: transaction write-set, multi-descriptor commits and FIFO handoff

The 13.6-second `blocks=126` stall is explained by the descriptor tag limit:
at 1024-byte blocks one descriptor holds 126 tags, and the old code fell back
to unjournaled direct home writes when a transaction exceeded that, still
holding the mount transaction. The reported `hold_ms=4` versus a five-second
waiter is LIFO registration plus `try_acquire` barging. Both are fixed:
journal blocks are staged in a per-transaction write-set (one payload and one
home write per unique block), commits span up to eight descriptor groups
(instead of writing home blocks unjournaled), and ownership hands off
oldest-waiter-first with the lock continuously held. Commit phases are now
`commit-metadata`, `commit-scratch`, `commit-payload`, `commit-descriptor`,
`commit-record`, `commit-super-read/publish/clear`, and `metadata-home`, so a
remaining stall still identifies its step. Recovery validates the full record
before replay and restores escaped magic words. See
`journal-writeset-fairness.md` for the design, host verification and the
home-checkpoint-failure limitation. Guest re-test is pending.
