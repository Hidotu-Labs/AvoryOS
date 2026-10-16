# Journal write-set, multi-descriptor commits and FIFO ownership

Date: 2026-10-15. Host regressions and ISO build pass; guest confirmation is
pending. The user's VM was left running and its disk image was not changed.

## Evidence

Two guest logs identify the same structural problem. In the first stall
(`build/logs/prime-runs/20261015T162336Z-o8z0bh1r/serial.log`, ~lines 3331-3359)
one `ThreadPoolForeg` owner holds the mount's transaction at `blocks=126` for
13.6 seconds while at least twenty writers report `[JBD2] transaction wait` every
five seconds. 126 is exactly the tag capacity of a 1024-byte journal descriptor
block (`(1024 - 12) / 8`). In the second report a waiter has been blocked for
at least five seconds while the owner's `hold_ms` is 4, with `phase=write-data`.

The old code explains both symptoms:

- `ext3_journal_block` wrote every payload and its home block synchronously
  while holding the mount transaction, with no write-set and no dedup. A
  metadata-heavy transaction issued two device round-trips per call; the
  allocator's repeated bitmap/descriptor updates were each written twice.
- When the staged block count exceeded the tag capacity of one descriptor
  block, the fallback dropped out of journaling and wrote the home block
  directly, still holding the transaction — a 126-block transaction did 126
  unjournaled writes, and any crash mid-transaction left the filesystem
  partially updated. Those synchronous writes are the seconds of `hold_ms`
  growth.
- Waiters registered at the queue head and re-tried `try_acquire` on every
  wakeup. A finishing owner could hand the lock to a fresh arrival that had
  just registered, so a thread that had waited since the beginning could be
  passed by every later writer (LIFO + barging starvation).

## Changes

### A. Per-transaction write-set

`ext3_journal_block` now stages copies in memory only. Blocks are stored once
per transaction in a chunked list (`ext3_txn_entry_t`, 32 entries per
`ext3_txn_chunk_t`) keyed by block number: restaging a block updates its image
in place, so the allocator's repeated bitmap and group-descriptor updates
collapse to one payload and one home write each. Readers
(`ext2_read_block`) consult the staged image before the block cache, so
read-modify-write sequences inside the transaction see the newest data even
though the home block still holds the pre-transaction copy. Staging is under
`cache_lock`; images are freed only after the commit finishes (or aborts) so a
reader never observes a freed image.

### B. Multi-descriptor transactions

A commit writes each unique block once to the journal payload, then one
descriptor block per tag-capacity group (up to `EXT3_JOURNAL_SEG_MAX` = 8
segments), each terminated by `LAST_TAG`, then one commit record, then
publishes `s_start`, checkpoints the home blocks, and clears `s_start` with the
next sequence. When the write-set or journal space cannot hold the transaction
the commit fails cleanly (ownership released, error returned); it never falls
back to unjournaled home writes. Metadata whose first word equals the journal
magic is escaped (zeroed in the log copy, tag `JBD_FLAG_ESCAPE`) so recovery
scans cannot mistake payloads for headers; the in-memory staging image keeps
the original first word and recovery restores it.

### C. FIFO ownership handoff

Waiters append to the journal wait queue (`wait_queue_add_tail`) and a
finishing owner transfers ownership to the oldest live entry
(`wait_queue_take_head`) while keeping the transaction lock continuously held.
The grant (`granted_tid`) is published under the small `sched_lock` before
`sched_wakeup`, so a woken waiter cannot miss the handoff and a fresh arrival
cannot barge. A registration that races a release observes `active == false`
and retries instead of parking; the old 5-second safety wake remains only as a
backstop. `ext3_journal_release_owner` (thread reaper) handles both a dead
active owner — discarding its write-set and granting the next waiter — and a
dead granted waiter that never started, passing the grant on.

### D. Recovery

Recovery now validates the full descriptor chain and the commit record before
touching any home block; a descriptor set without a commit record is ignored.
It then replays every descriptor group and payload of each committed
transaction, restores escaped magic words, and returns success if at least one
transaction was replayed. Partial final transactions are no longer replayed
and the following stale block is skipped.

## Verification

Host tests compile the actual `kernel/src/fs/ext3/ext3.c` and
`kernel/src/fs/ext2/ext2_block.c` under ASan/UBSan:

- `scripts/tests/test_ext3_empty_commit.c`: empty/nested transactions,
  staging-order commit (payload → descriptor → commit record → publish →
  home → clear), dedup (300 restages of one block → one payload + one home
  write, zero I/O while staging), 130-block two-descriptor commit with
  per-segment `LAST_TAG`, FIFO handoff with three gated writers (oldest first,
  late arrival cannot barge), four concurrent owners, every commit write/read
  failure including home-checkpoint failure and retirement, invalid mappings,
  all five allocation-failure points with leak accounting, 2/4 KiB block tag
  capacities, and recovery of single/multi-descriptor/escaped/incomplete
  transactions. 20 test groups pass.
- `scripts/tests/test_ext2_cache_coherence.c`,
  `test_ext2_metadata_writeback.c`, `test_ext2_alloc_run.c`,
  `test_ext2_write_batch.c`, `test_journal_usercopy.py` and
  `test_ext2_data_run.py` pass with the new `ext2_read_block` staged-read hook
  and the extracted `ext2_cache_prepare` helper.
- `make -j8 avoryos-x86_64.iso` succeeds; only the pre-existing linker RWX
  LOAD-segment warning remains.

## Limitations

- A home-checkpoint write error after a successful commit returns failure but
  retires the journal (`s_start` cleared, sequence advanced). The log record is
  complete, so recovery would replay it; leaving `s_start` set would make every
  later transaction overwrite the same replay region. The affected home block
  stays dirty for retry.
- The transaction is still one-at-a-time per mount and still commits
  synchronously; there is no batching window, background commit or
  checkpointing worker. Group commit remains the Stage 2 goal in
  `journal-performance-plan.md`.
- The journal does not wrap (`commit_pos < journal_blocks` is required); large
  transactions are rejected rather than blocked. Circular-log recovery remains
  unproven and is still required before Stage 2.

## Next step

Reboot the VM into the rebuilt ISO and repeat the desktop workload that caused
the waits. If a wait appears, collect the `[JBD2] transaction wait` line with
its `phase`; `commit-*` phases identify the commit step, `metadata-home`
identifies the checkpoint. Compare `/bin/test_journal_speed DIRECTORY 100 4 5`
and `/proc/storage_perf` snapshots before claiming a speedup.
