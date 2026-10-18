# Kernel lifetime leaks investigated from syscall and Minecraft captures

The supplied stress output reported 52 kB after WAIT4, PROCESS ATTRS and
ACCEPT4, 56 kB after UNIX STREAM, 12 kB after SCHED and 4 kB after CLONE/FUTEX.
The original harness compared a single immediate `MemFree` reading before
and after each test. Those values establish retention at that instant, not
which allocator owns the pages or whether the loss repeats indefinitely.
WAIT4 and the socket/process-attribute workloads all fork; native address
spaces are deliberately freed on a deferred queue after the PID is reaped.
Generic slab caches also retain empty pages. These effects require separate
measurement from unreachable allocations.

Two independently verifiable leaks were fixed:

- `sys_brk` shrink unmapped every page of the shrinking range while still
  holding `mm->lock`, so each per-page safe_unmap_and_free() ran with
  interrupts masked. In that state tlb_flush_deferred_drain() can only
  perform local invalidation and must return false, parking (or, once the
  64-entry per-CPU queues overflow, silently dropping) every faulted-in
  frame. The focused brk repeat retained exactly the full allocated volume
  per round (3200 kB = 200 iterations x 16 kB, linear to 32000 kB over ten
  rounds). The shrink now follows the sys_munmap pattern: reserve the range
  PROT_NONE against reinstalling faults, unmap in 32-page bounded batches
  with mm->lock dropped, and call vmm_drain_deferred_work() between batches.
  Frame freeing goes through teardown_range()'s anonymous-private or
  page-cache ownership check instead of an unconditional free.
- Linux task shadows had an initial usage count but no native reference
  release, and `__put_task_struct()` was a no-op. Internal service, workqueue
  and kthread-worker retire/respawn cycles abandoned their task/control
  allocations. The native thread now owns one reference; reaping clears its
  back-pointer and releases that reference after execution is off-CPU.
  Creation adds a separate creator reference. Joined kthreads release it in
  `kthread_stop`; internal self-retiring workers detach it before starting.
  Worker destruction pins a borrowed task under the publication lock before
  dropping that lock and joining. External get/put holders can outlive native
  reaping. The final put frees the task and its control block, protecting the
  completion through the entire exit trampoline. Failed task-shadow creation
  cancels the suspended native thread and releases its boot/control records.
  Stopping before first execution now wakes the suspended thread as well.
- AF_UNIX `listen()` unconditionally reset the pending accept queue. Calling
  it again to change backlog orphaned those sockets; closing the listener
  could no longer destroy them. Re-listening now preserves the queue and
  changes only backlog. Connected sockets are rejected.

These fixes do not establish that either defect produced all the exact
reported syscall deltas, the Minecraft ENOMEM, or its corrupted saved pixels.
The socket stress opens a new listener each iteration and did not explicitly
re-listen. The simple fork tests need not allocate Linux task shadows at all.
Do not claim those observations are resolved until the guest repeat verifies
which retention survives cleanup and whether it accumulates.

`test_mem_stress --focus NAME --rounds 6` now warms that exact workload and
prints immediate and post-200-ms readings for every round. Supported names:
`wait4`, `sched`, `clone`, `accept4`, `unix-stream`, `process-attrs`, `mmap`,
`epoll`, `brk`, `pipe2`. All readings
are preserved; there is no cache allowance or best-of selection that could
hide growth. It prints positive/negative deltas since the previous round and
baseline. Background allocations can still influence global readings. Normal
suite output labels single-sample drops RETAINED, with no automatic leak
verdict. SCHED stress restores its original CPU affinity after testing instead
of pinning subsequent workloads to CPU 0.

Validation runs the actual extracted lifecycle functions under ASan/UBSan
with counted allocations: 1,000 plain task exits, 4,000 joined/detached worker
lifecycles, retained external references, stop-before-start and OOM cancellation
leave zero live allocations. The AF_UNIX test preserves pending sockets over
1,000 repeated backlog changes; restoring the original queue reset fails the
regression. Existing IRQ-lock, workqueue-requeue, completion, RCU-lifetime,
socket-fd ownership and vfork rendezvous tests also pass. Hardware/guest
validation of the rebuilt kernel is still required.

The kernel and `avoryos-x86_64.iso` were rebuilt; the ISO was assembled from
its previous boot tree with the existing VFIO/PRIME command line preserved.
The embedded stripped kernel checksum was verified against the new build.
Default ISO replacement was atomic, so the currently running QEMU retains its
old open ISO. Close and relaunch QEMU to load the patch. The previous ISO is
saved in `build/gpu-diagnostics/avoryos-before-memory-fix.iso`; the candidate
is `build/gpu-diagnostics/avoryos-memory-fix-x86_64.iso`. The active disk image
was not edited. The updated static `test_mem_stress` is staged on the existing
host port 9007 server for download into the guest.

Launch with `make -o disk.img -o avoryos-x86_64.iso run-c7-prime` to use the
prepared ISO and existing disk without triggering a full disk rebuild from
the changed diagnostic dependency. Then download `/test_mem_stress` into
`/tmp/test_mem_stress` and run the focused repeat cases with Minecraft closed.

## Guest repeat results, 2026-10-13

`kernel-leaks-guest-second.log` contains all five remaining focused cases,
each exiting zero. WAIT4, SCHED, CLONE/FUTEX, ACCEPT4 and UNIX STREAM each
completed six measured rounds with exactly zero retention in every settled
sample relative to its warmed baseline. WAIT4/ACCEPT4/UNIX STREAM sometimes
showed immediate losses up to 152 kB, all returned by the 200-ms sample.
This directly demonstrates delayed return in those observations; it does not
establish that the kernel patches caused all differences from the original
single-sample test.

The first uploaded log retained only PROCESS ATTRS because guest O_APPEND
was ineffective: the combined-file wrapper overwrote its earlier cases.
That retained six rounds: five settled deltas of zero and a final 4 kB delta.
The final loss occurred as the diagnostic output crossed the log's second
4 KiB page (final PASSED at byte 4215, ROUND 6 at byte 4248), consistent with
file-cache growth rather than demonstrated cumulative process-attribute
leakage. It has not been attributed by an allocation ownership trace.
The wrapper now writes each case to a separate file and concatenates once
with a single writer; no O_APPEND is required. Logs remain in
`build/gpu-diagnostics/kernel-leaks-guest.log` and
`build/gpu-diagnostics/kernel-leaks-guest-second.log`.

These guest runs show no accumulating leak in the five settled repeat cases.
They do not certify absence of other kernel leaks or resolution of the
long-running Minecraft ENOMEM and saved-image corruption. A real-workload
repeat on the updated kernel remains the check for those symptoms.

## Guest repeat results, 2026-10-17 (mmap, epoll, brk, pipe2)

A full-suite single-sample run flagged RETAINED 376 kB after PIPE2, 3200 kB
after BRK, 100 kB after EPOLL and 4 kB after MMAP/MUNMAP. Focused repeats
(`--focus NAME --rounds 10`) classified them. MMAP and EPOLL showed exactly
zero retention in every settled sample. PIPE2 lost 536 kB once before the
baseline and stayed flat for all ten rounds: bounded cache growth, not a
leak, and it needs no fix. BRK retained exactly 3200 kB in every round
(200 iterations x 16 kB, the full allocated volume), reaching 32000 kB after
ten rounds; this was the third fixed leak above.

After the batched-drain `sys_brk` fix, the guest brk repeat shows zero
retention in every settled sample across ten rounds, with rounds 9 and 10
reading 128 kB and 224 kB FREE rather than retained. The kernel and
`avoryos-x86_64.iso` were rebuilt with the `run-c7-prime` command line
preserved (verified inside the ISO's limine.conf), and the disk's
`/bin/test_mem_stress` was replaced in place via debugfs with the
focus-mmap/epoll/brk/pipe2 build. Not yet covered: real-workload runs on
the rebuilt kernel remain the check for the long-standing symptoms above.
