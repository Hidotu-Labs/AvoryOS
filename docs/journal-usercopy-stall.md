# Journal stalls and faulting write buffers

Date: 2026-10-09. Host regressions pass; reproduction in the guest is pending.
Guest RAM remains 4 GB. The kernel and boot ISO were rebuilt; the guest disk
was not modified.

## Report and interpretation

The supplied waits identify owner 1746 (`Main`), depth 1, one submitted block,
and a hold increasing from 22.8 to 29.6 seconds in phase `operation`. The user
reports that this never recovers and prevents opening a terminal or progressing
in Roblox. The owner call site `ffffffff8006d31e` in the reported build resolves
to the initial journal start in `ext4_write_impl`. It identifies the operation
that acquired the transaction, not the owner's current blocked instruction.
The two lines alone do not establish the exact blocked stack.

## Concrete faults corrected

- Both ext2 and ext4 copied user bytes after acquiring the mount-wide journal.
  Fault-time file I/O and memory reclaim could therefore run while all other
  writers waited. A shared staging wrapper now copies at most 32 filesystem
  blocks into reusable kernel scratch space before each transaction. Unaligned
  chunks touch at most 32 blocks. The inode snapshot still occurs under journal
  ownership, and every transaction finishes before the next user copy.
- The subsequent VFS cache update copied user bytes under `pages_lock`.
  A demand-page allocation failure invokes `mm_file_cache_reclaim`, whose
  `vfs_cache_reclaim` acquires that same lock for the process's mapped files.
  If the updated file is among those mappings, this is a recursive-lock
  deadlock. Another writer can then hold the journal while its allocation
  waits in reclaim behind that lock. Cache updates now stage one page outside
  the lock, then copy kernel memory under the lock. A copy or scratch allocation
  failure invalidates the affected cache range rather than installing a partial
  image.
- `vfs_write` interpreted unsigned negative errors such as `-EIO`, `-ENOMEM`,
  and `-EFAULT` as successful lengths for cache updates. It now updates the
  cache only for a nonzero result no larger than the requested size.

Zero-byte user faults leave mappings untouched; partial faults write only the
copied prefix and preserve existing block suffixes. Scratch buffers are released
on success and failures. I/O/commit errors remain errors even after an earlier
batch wrote a prefix. Kernel-buffer writes keep the existing batching path.
The mount's journal lock, metadata coherence, commit sequence, and live-owner
exclusion are preserved. A timeout does not steal a live owner's transaction.

Write phases now distinguish inode reads, mapping/allocation, data reads, data
writes, and inode updates. A future wait should report one of these instead of
the generic `operation`, making any remaining stall easier to locate.

## Verification

- `python3 scripts/tests/test_journal_usercopy.py`: extracts the actual cache
  update and VFS write functions. A mock user fault performs same-file reclaim
  lock acquisition; the old cache code fails the recursive-lock assertion and
  the new code passes. Old VFS code also fails the error-length assertion.
  Checks cover unaligned page updates, preserved suffixes, copy/allocation
  failures, and in-flight fills.
- `test_ext2_write_batch.c`, using the compiler command in
  `journal-benchmark.md`: actual filesystem code, fault-time neighboring writes,
  bounded unaligned user batches, partial faults in a later batch, scratch
  allocation accounting, concurrent inode writers, and I/O failure cleanup.
- `python3 scripts/tests/test_ext4_sparse_write.py`: actual extent/write code,
  kernel and staged user batches, competing inode snapshots, sparse boundaries,
  zero/partial user faults, commit errors, and mapping failures.
- `test_ext3_empty_commit.c`: existing actual journal tests still pass, including
  contention/wake registration, ownership, nesting, and failed commit cleanup.
- Host tests use AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer
  is disabled for the sandbox; ext2 scratch allocation accounting is explicit.
- `make -j8 avoryos-x86_64.iso` succeeds. Booting the rebuilt ISO and repeating
  the original guest workload remains necessary to confirm this reported hang.

These changes remove demonstrated deadlock/error paths. They do not establish
that every journal wait has the same cause or resolve the planned broader
memory-leak audit.
