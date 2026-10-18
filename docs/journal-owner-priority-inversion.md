# Journal owner scheduling inversion (October 17)

The supplied Epiphany lock report shows a scheduling inversion rather than a
CPU spinning on the journal lock or a device failing to complete a request:

* The mount is active in `commit-record`, with owner tid 719, depth 1 and
  `granted=0`.
* That owner is READY, `on_cpu=0`, `on_rq=1`, and has waited 5091 ms to run on
  CPU 7. Its saved stack is in the NVMe completion wait path.
* CPU 7 is running plasmashell with five queued tasks; six other CPUs are idle.
  Runtime migration of already-started continuations is deliberately disabled.
* The owner's virtual deadline minus runtime is 409599999 ns for a 6000000 ns
  physical slice. That is the nice-19 weight of 15 (6 ms * 1024 / 15), while
  the foreground task has a substantially earlier deadline. The report does
  not give plasmashell's nice value.

Making the journal waiters poll faster cannot dispatch this owner. Raising the
lock-report threshold would hide the symptom. Cross-CPU migration is a separate
scheduler lifecycle problem and is not enabled by this change.

## Native priority protection

The scheduler now supports nested priority-ceiling scopes for kernel critical
sections that can sleep. Within a scope, EEVFD uses the highest normal fair-class
weight, equivalent to nice -20. The task's configured nice, scheduling policy
and requested physical time slice remain unchanged. The final exit restores
its configured weight, including nice changes made while the scope was active.

Reweighting accounts execution at the old weight under the CPU queue lock,
rebases virtual runtime into the current runqueue time domain, and renews the
virtual deadline. It does not introduce a second scheduling queue or dispatch
an ineligible task outside EEVFD. The depth occupies existing sched_entity
padding; its size and existing field offsets stay unchanged.

The journal enters protection once per outer acquisition, before joining the
wait queue, and leaves after final transaction cleanup and ownership handoff.
Protecting acquisition is necessary because FIFO handoff reserves the mount
for a woken recipient before it resumes to consume the grant. Nested helpers
on the same transaction do not add another scope. Interrupted acquisition drops
its scope after forwarding any unused grant. All normal commit success and
failure paths converge on the same protected finish helper.

This is priority protection of acquisition/ownership, rather than dynamic
waiter-to-owner priority inheritance. It allows a background task to complete
shared kernel work promptly; it does not generally promote unrelated userspace
work or fix the separate limitation on runtime CPU migration. A dead task's
scope has no runnable entity and is discarded with the task; the existing
journal reaper still aborts or forwards its ownership.

Lockdiag continues to report stalls. Its journal-owner snapshot now also shows
effective `weight` and `critical_depth` to distinguish a protected owner from
its configured user priority.

## Validation

Run `make test-journal-priority` after the normal kernel/ISO setup. Tests use
private images and do not modify disk.img:

* Actual-source EEVFD and scheduler ASan/UBSan simulation uses the captured
  owner/runtime values against a modeled nice--20 competitor: unprotected
  dispatch takes 30984 ms; protected dispatch takes 123 ms. This is a controlled
  simulation, not a measured improvement to the user's live desktop.
* The same test checks nested entry/exit, a nice change during protection,
  preservation of a custom slice, and restoration of normal weight.
* Actual-source journal sanitizer tests cover commit ordering, allocation and
  I/O failures, interrupted acquisition, FIFO handoff, owner death, recovery,
  concurrent writers and balanced outer scope entry/exit through nested helpers.
* A private one-CPU ext3/NVMe guest runs nice-19 and nice-0 writers against a
  nice--20 CPU load with periodic sleeps. Each writer creates, writes, fsyncs,
  renames and unlinks 32 files and checks its configured nice after each cycle.
  The final run completed the writers in 769 ms and 573 ms respectively, with
  `JOURNAL_PRIORITY_BOOT: END failures=0`. This exercises real journal I/O and
  priority restoration; the scheduler simulation supplies the deterministic
  starvation regression.
* Existing deadline-tree and wake-publication regressions pass. Scheduler
  attribute syscall boot validation also passes (`SCHED_ATTR_BOOT: END failures=0`).

Evidence is under `build/journal-priority/`, especially `final-tests.log`,
`scheduler-test.log`, `deadline-test.log`, `wake-test.log`, and
`sched-attr-boot.log`. The journal guest itself is under
`build/journal-priority-boot/`.

The rebuilt boot ISO retains its prior configuration and contains the verified
stripped kernel. The prior ISO is saved as
`build/journal-priority/avoryos-before.iso`. Reboot to load the new kernel; no
application binaries, desktop settings or journal durability rules are changed.
The original eight-CPU Plasma/Epiphany workload has not been rerun end to end.
