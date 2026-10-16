# Firefox journal freeze, 2026-10-16

The user confirmed a full desktop freeze. The supplied capture is also present
in `build/logs/p6-c7-prime.log` from the 11:31 build. It reports a mount journal
held for more than five seconds by Firefox tid 1080, in
`commit-descriptor-io`, with four staged blocks. Other filesystem mutations
wait behind this transaction.

The owner's saved scheduler stack reaches NVMe command wait through the
journal descriptor write. At the snapshot it is READY, on CPU 5's runqueue,
not executing, and has no pending sleep deadline. This does **not** prove that
the device command completed: either completion or the 10 ms fallback timeout
can wake the NVMe waiter. Nor does the snapshot prove five seconds of runnable
waiting. Its `ready_since_ms=0` was not a valid wake-age measurement.

CPU 5's sampled RIP resolves to the retry following `sched_yield()` in
`ext2_read_block`; its last recorded syscall is mprotect. Retry can mean a
colliding in-flight cache write or a changed cache generation. A last-syscall
label alone does not identify the current instruction. Other CPUs acknowledge
the diagnostic probe and most are idle. No NVMe command timeout or controller
fatal status appears in this capture. The final ~511 ms TLB delay samples the
CPU printing the large diagnostic report, so it is not independent proof of a
TLB fault. The bwrap namespace EPERM is separate from the journal stall.

## Changes

`sched_wakeup` now records the timestamp when a blocked thread becomes runnable
and preserves an existing timestamp if deadline expiry already linked it.
Previously the common wake path omitted the timestamp and the already-linked
path erased it. This changes diagnostic bookkeeping, not scheduling policy.
The automatic journal hang report now includes the owner's runnable wait age,
virtual runtime/deadline and slice, and its CPU's queued count, virtual time,
timer/quantum deadlines and current task's scheduling state. These are unlocked
snapshots, deliberately avoiding potentially wedged runqueue locks.

The full desktop freeze is **not yet fixed or reproduced**. No change to the
journal's durability, cache coherence, NVMe timeout policy or audio buffering
is justified by this snapshot alone.

## Validation

- `test_scheduler_wake_publication.py` passes the existing publication-race
  scenarios plus wake-age initialization, preservation after deadline
  enqueue, and repeated READY wakeups.
- `test_sched_deadline_tree.py` passes 4,785 operations over 256 threads.
- The kernel builds with the existing RWX segment linker warning.
- `build/journal-freeze-probe/` contains an isolated eight-vCPU KVM probe using
  its own ext3 image and NVMe device, without the desktop disk or VFIO.
  Eight writers and eight readers perform create/truncate/write/fsync and
  read/mmap/mprotect/munmap. All workers inherit a CPU 5 affinity mask;
  the monitoring main task remains on its existing CPU. The baseline completes
  160 writes and about 24 million read attempts with zero reported I/O errors,
  without reproducing the freeze. Read attempts include files not yet created.
  This covers a narrow storage/scheduler workload, not Firefox or AMDGPU.

On the next desktop reproduction, retain the automatic journal report including
`ready_wait_ms` and `journal owner cpu`. It can distinguish an aged runnable
owner from a recently woken command waiter; current virtual deadlines and timer
state make the former actionable without guessing from a single saved stack.

The diagnostic candidate also completed the 160-write probe with zero errors
and powered off normally. It was installed as `avoryos-x86_64.iso` only after
checking that the previous ISO was not open. `disk.img` was not modified.
Rollback ISO: `build/journal-freeze-probe/iso-before-diagnostics.iso`.
The matching unstripped kernel and installation hashes are saved beside it.
