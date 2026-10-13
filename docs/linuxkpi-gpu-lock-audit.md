# Native GPU buffer-lock audit

The graphics corruption still has no verified root cause. The compositor,
PRIME and VirtIO tests do not establish that LinuxKPI is the only remaining
possibility. Small offscreen tests with a CPU readback after every draw also
do not reproduce overlapping rendering submissions.

An upstream RadeonSI cache fix was found during the audit:
[Mesa commit 57fd11256bdf0ec1ff1908277e4963180252491f](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/57fd11256bdf0ec1ff1908277e4963180252491f).
It concerns stale shader constants/descriptors on gfx1036 with overlapping IBs.
The user requested strictly native fixes, so the experimental Mesa backport,
build helper, setup hooks and private build were removed. No patched Mesa
library was staged or installed in disk.img. No vendor AMDGPU source was changed.
This finding is evidence to consider, not permission to move a GPU-specific
cache workaround into a generic LinuxKPI barrier or CPU cache operation.

## Confirmed native defect

LinuxKPI's `ww_mutex` protects DRM/TTM buffer reservation transactions. The
old implementation:

- Could reject a transaction's **first** contested lock with `-EDEADLK`, even
  when it held no locks and could safely wait.
- Incremented `ctx->acquired` but never decremented it on unlock.
- Read an owner's stack context without protecting it against unlock/release.
- Used a try/unlock probe before a separate sleeping acquire. Another owner
  could appear in between, invalidating the ticket/deadlock decision.
- Mixed wait-die and wound-wait and checked a wound only after acquiring the
  next mutex. A holder already asleep on another mutex could remain blocked.

The native replacement serializes ownership/context publication and waiter
ordering under `base.wait_lock`, balances acquired counts, distinguishes
`DEFINE_WD_CLASS` and `DEFINE_WW_CLASS`, and wakes a wounded holder to let it
back off while blocked. Sleeping callers recheck ownership and tickets.
The first lock waits; slow retries retain the same ticket after dropping all
held locks. NULL-context users, duplicate acquisitions, trylock and interruptible
acquisitions retain their API conventions. Priority inheritance and lockdep
remain unsupported.

These semantics follow Linux's
[transaction-lock design](https://docs.kernel.org/locking/ww-mutex-design.html).
This fixes a demonstrated native synchronization defect. It does **not**
prove that the remaining Minecraft pixels came from that defect.

## Validation

`python3 scripts/tests/test_kpi_ww_mutex.py` compiles the actual LinuxKPI
mutex/ww_mutex implementations with pthread-backed native scheduling primitives.
It tests first-lock contention, held-lock accounting, 2,000 opposite-order
transactions across both algorithms, wounding a blocked holder, slow retries,
same-context duplicate acquisition, trylock and signal behavior. The captured
old implementation fails the first-lock regression. Native boot tests also
check counts across two held BO locks and the duplicate/trylock contract.

Existing cache-alias, shmem lifetime and plain-mutex contention tests pass.
The ISO is rebuilt with the original PRIME/VFIO command line. The running
user VM and its disk image are not modified or restarted.

The rebuilt ISO's private two-CPU TCG boot passes native WW contention,
context accounting and ordered multi-BO tests. Its presentation test completes
400 concurrent flips and 400 damage commits, with 89 active host frames and
the final 1,024,000 pixels verified. That fixture intentionally has no GPU
firmware and no physical AMD device; its known missing-firmware/ATA messages
and pre-existing sysfs PMM drift do not count as successful hardware testing.

## Offscreen diagnostic

`userland/test_gpu_constants.c` is a diagnostic, not a Mesa patch. Its default
mode requires the AMD render node and RadeonSI. It queues 1,024 draws with
changing shader constants, calling `glFlush` but performing no CPU readback
until the whole batch completes. Sixteen batches check every rendered pixel
against a CPU oracle before any display/PRIME path. Submission overlap is
allowed, not guaranteed by a userspace timing assertion.

`--software` explicitly runs a harness smoke test; it cannot establish GPU
correctness. The harness passed with the original unmodified Mesa in a private
Linux chroot. Hardware validation and the affected Minecraft scene remain
outstanding. Minecraft's options and launcher defaults are unchanged.

## Free-owner requeue correction (2026-10-13)

The user's eight-CPU KVM boot exposed a race missed by the earlier tests.
The thread dump showed `kpi/ww1` and `kpi/ww2` both blocked on the same mutex
with `owner=0`, while the test runner waited in `kthread_stop`. This was a
deadlock in the replacement, not normal self-test duration. The captured
log is `build/logs/linuxkpi-ww-free-owner-before-fix.log`.

An older transaction can release the mutex, then reacquire before a younger
queued waiter consumes its wake. Stamp ordering puts the older transaction
at the queue head. The previous code immediately slept after insertion,
although the mutex was free and that transaction was now eligible to acquire
it; the younger waiter also slept behind it. No future owner existed to
unlock the mutex and wake either thread.

After insertion the loop now rechecks ownership/queue position under the
same lock before sleeping. The deterministic regression parks the younger
waiter across its wake, forces the older transaction to requeue on the free
mutex, and rejects the captured pre-fix implementation with
`first waiter slept with owner=0`. It also runs 24,000 rapid reacquisitions
by eight pthread workers per run, across both transaction algorithms.
Twelve runs passed 288,000 reacquisitions plus 24,000 opposite-order pairs.

The native boot contention test now uses eight workers and logs the current
test name. The rebuilt ISO passed an eight-CPU KVM private boot, including
all three WW tests and the rest of chunk 6. Its PRIME test completed 400 flips
and 400 damage commits; 47 active host frames and the final 1,024,000 pixels
matched. The VM used a private root image, disabled AMDGPU initialization,
and never attached VFIO or opened the user's disk image. This validates the
native locking correction, not the unresolved hardware pixel corruption.
