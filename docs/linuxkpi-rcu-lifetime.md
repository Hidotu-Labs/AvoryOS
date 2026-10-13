# Native RCU lifetime correction

The remaining RadeonSI corruption is unverified. The real AMD asynchronous
constant diagnostic passed all sixteen batches with the flight recorder
disabled; llvmpipe is reported to work. Neither result establishes that the
complete hardware texture/rendering path is correct.

## Confirmed native defects

The callback worker previously waited for a grace period **before** detaching
the pending callbacks. A reader could enter after that grace period, acquire
an object, and have its removal callback queued before the worker detached
the list. That callback would then run without waiting for its reader.
The deterministic regression against the captured old implementation reports
`late callback freed while its reader remained active`.

This mechanism is used by the unmodified imported `dma-resv.c` for fence-list
replacement and by `dma-fence.c`, AMDGPU's fence code and BO-list code for
deferred destruction. A lifetime violation here is a plausible source of
rendering failures; we have not captured it causing a particular Minecraft
pixel. Linux requires a callback's removal/publication to precede the grace
period covering it: see [RCU memory ordering](https://www.kernel.org/doc/html/v6.6/RCU/Design/Memory-Ordering/Tree-RCU-Memory-Ordering.html)
and [RCU requirements](https://www.kernel.org/doc/html/v6.6/RCU/Design/Requirements/Requirements.html).

Two related native issues were corrected:

- A single global "someone is waiting" flag let one concurrent grace-period
  waiter disable another's wakeup. Waiters are now counted.
- A zero-reader transition could be missed when a later reader entered before
  a woken waiter ran. The old implementation also fails the deterministic
  regression with `completed grace period lost when new reader entered`.

The overlay's `rcu_assign_pointer()` now publishes with release ordering
rather than only `WRITE_ONCE`, preserving initialization before publication.

## Implementation

`kernel/linuxkpi/src/rcu.c` detaches a FIFO callback batch before starting its
grace period. New callbacks belong to a later batch. The reader count and
drain generation occupy one atomic 64-bit word so a zero transition cannot
be published late, after a newer reader has entered. Grace periods observe
either a currently empty reader set or a changed drain generation. The shared
reader count remains conservative: a continuously overlapping set of readers
can postpone a drain. This is not Linux's per-CPU TREE_RCU implementation.

`rcu_barrier()` snapshots the number of queued callbacks under the callback
lock and waits for the corresponding completed batch count. That also covers
a detached batch still waiting for readers, without requiring unrelated
callbacks submitted after the barrier to complete.

No Mesa or AMDGPU vendor source is patched, no frame-wide synchronization is
forced, and no Minecraft preferences are written. The boot log identifies the
new implementation with `[KPI-RCU] isolated callback batches, drain generations
and release publication active`.

## Validation

`scripts/tests/test_kpi_rcu_lifetime.py` compiles the actual implementation
with mock scheduling gates and reproduces the two old defects deterministically.
The fixed code also exercises eight simultaneous grace-period waiters, a
callback barrier with an active reader, 8,000 callback frees and 120,000 reader
sections per run. Twenty runs passed AddressSanitizer/UndefinedBehaviorSanitizer:
160,000 callback frees and 2,400,000 reader sections. The scheduling mocks do
not stand in for validation of the native scheduler.

The native boot self-test now publishes and retires 512 objects while two real
kernel threads inspect them under RCU. Storage is retained until the test ends
so an early callback produces a failed check rather than a test-induced kernel
use-after-free. The eight-CPU KVM boot reported 16,384 reader inspections,
512 retired objects and zero premature callbacks. Reader work is bounded to
8,192 iterations per thread to avoid extending boot under scheduler delays.
The one-CPU fixture also passed its native lifetime check and presentation
test (110 active host frames). The same eight-CPU private VM passed
400 concurrent flips and 400 damage commits; 72 active host frames and the
final 1,024,000 pixels matched. Its complete recorder export was also checked.

These fixtures disable AMDGPU initialization, attach no VFIO device and use a
private disk. They do not prove the remaining physical AMD corruption fixed.
The minimal boot still reports missing ATA devices and occasionally a one-page
vkms PMM drift; the same vkms drift appears in the pre-fix
`build/logs/linuxkpi-ww-requeue-kvm8-boot.log`. Those are not passing checks.

Logs: `build/logs/rcu-before-batch-regression.log`,
`build/logs/rcu-before-drain-regression.log`,
`build/logs/linuxkpi-rcu-lifetime-regression.log`,
`build/logs/linuxkpi-rcu-lifetime-build.log` and
`build/logs/linuxkpi-rcu-lifetime-kvm{1,8}-{result,boot,host}.log`.

Boot the rebuilt ISO and test the affected Minecraft/Mesa scene normally.
Physical AMD and affected-workload validation remain required.
