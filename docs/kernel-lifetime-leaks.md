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
`wait4`, `sched`, `clone`, `accept4`, `unix-stream`, `process-attrs`. All readings
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
