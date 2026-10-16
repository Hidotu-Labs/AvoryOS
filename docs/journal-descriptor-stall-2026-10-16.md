# Transient journal descriptor stall

The post-rebuild guest report has one owner, tid 871 (`systems:disk$2`),
`active=1 depth=1 blocks=1 granted=0`, with `hold_ms` rising from 5317 to
10495 in `phase=commit-descriptor`. The command is truncated to the kernel's
16-byte comm field. The caller address `ffffffff800691d2` resolves to
`ext2_rename_impl` in the ELF present when the report was received.

This differs from the previous idle/grant stalls: a transaction is executing
its commit, rather than waiting for a granted owner to start. Submitted and
unchanged counts are cumulative metadata counters; they are not NVMe progress
counters. A one-block write-set fits in one descriptor. The old phase covers
descriptor construction, journal-block mapping, optional cache allocation,
cache publication, device submission/completion and cache completion. It does
not prove an outstanding device command.

The current NVMe wait has a five-second timeout and a ten-millisecond safety
wake. No NVMe timeout/failure message accompanies these lines in the captured
serial log. That is relevant but inconclusive: the owner may be before that
wait, delayed in a lock acquisition, or waiting to be scheduled. The stall
cleared before the user could request a manual stack report. No live owner
stack was captured, and no root-cause or elapsed-time fix is claimed.

## Automatic evidence capture

The existing five-second journal warning now requests the existing LOCKDIAG
report immediately, outside the waiter's journal/cache/wait-queue locks.
Reports are atomically limited to one per 30 seconds across all mounts and
waiters, so a crowd behind one owner does not each print a full report. The
warning itself retains its existing once-per-wait behavior.

The root-journal section of LOCKDIAG now identifies the owner's state even
when READY, with CPU, on-CPU and run-queue flags, ready timestamp, wake deadline
and deadline-queue membership. Previously the thread listing skipped READY
owners; that could hide a scheduling delay after an I/O completion. A saved
kernel stack is printed for an off-CPU READY/BLOCKED/SLEEPING owner. The
existing all-CPU snapshots cover a running owner. These are diagnostic
samples, not an atomic snapshot of the scheduler.

Descriptor phases now identify `commit-descriptor-build`,
`commit-descriptor-map`, `commit-descriptor-cache-alloc`,
`commit-descriptor-cache-publish`, `commit-descriptor-io` and
`commit-descriptor-cache-finish`. The shared traced-write helper uses the same
cache and device operations as before, including versioning, invalidation on
failure and error returns. Metadata home writes retain their existing labels.
Journal ordering, ownership and NVMe polling/timeout policy are unchanged.

## Verification

- Actual-source journal ASan/UBSan tests pass, including the report throttle,
  time-sampling race, descriptor labels, FIFO handoff, concurrent writers,
  commit errors and recovery.
- Actual-source block-cache ASan/UBSan tests pass, including named descriptor
  allocation/I/O/completion phases on success and failure. Existing metadata
  writeback, allocation and batched/user-copy write regressions pass.
- NVMe wait-registration/poll/timeout and metadata DMA/PRP regressions pass.
  Scheduler wake-publication regression passes.
- The updated ISO builds successfully; the pre-existing RWX LOAD-segment
  warning remains. The working disk image was not modified or rebuilt.

Boot the diagnostic ISO and use the normal workload. On another five-second
journal wait the serial capture should contain a `[LOCKDIAG]` report whose
reason is `JBD2 transaction wait exceeded 5 seconds`. Resolve its kernel
addresses against the matching `kernel/bin-x86_64/kernel` ELF, then address the
observed wait path rather than inferring device latency from the broad phase.
