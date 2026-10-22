# C7 SMP close-path shootdown stall

The supplied eight-vCPU report resolves against the C4 6.18 kernel. CPU 1 is
closing a Plasma DRM file through Nouveau client/VMM teardown and holds
`shootdown_lock`. The shootdown watchdog subsequently identifies CPU 3 as
the target which never acknowledges a kernel-address invalidation. CPU 0 is
also waiting for a VFS-RCU grace period. The IRQ probes cannot sample either
CPU 0 or CPU 3. These stack scans are not complete unwinds and do not prove
which lock CPU 3 was spinning on.

The audit found that LinuxKPI spinlocks poll published local TLB requests
while waiting, but both rwlock contention loops only executed PAUSE.
Nouveau uses IRQ-masked rwlocks for event notification and removal. An
IRQ-masked reader or writer waiting behind a holder that needs a remote
shootdown can therefore prevent the invalidation required for that holder
to release its lock.

Both rwlock wait paths now invoke the existing local TLB poller. This does
not enable interrupts, acquire another lock, schedule, acknowledge VFS-RCU
read sections prematurely, or bypass remote invalidation. The existing
poller completes the real local invalidation before publishing its TLB ack.

`python3 scripts/tests/test_tlb_lock_poll.py` exercises the actual lock loops
and shootdown handler, substituting only privileged instructions. It covers
reader versus writer, writer versus reader, and writer versus writer,
alongside the existing native and LinuxKPI spinlocks. Removing the new poll
calls recreates the acknowledgement deadlock in bounded negative controls.
PCID/fallback invalidation, flush-before-ack, delayed IPIs and IRQ-state
preservation are also checked.

This establishes a reproducible kernel defect and its correction, not yet
proof that it explains every freeze in the supplied report. The user chose
to perform the NVIDIA desktop retest. Keep the watchdog and request a new
report if it recurs; do not increase the timeout or drop required acks.
