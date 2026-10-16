# Interrupted journal ownership handoff

Date: 2026-10-16. Host regressions and ISO build pass; guest confirmation is
pending.

The reported waiters all observe `active=1 depth=0 blocks=0 phase=idle`,
with unchanged cumulative metadata counters and a hold time growing to 23.8
seconds. This is the state between finishing one transaction and starting its
successor. The recorded command, start time and caller address still belong
to the previous transaction in this state, so they do not reliably identify
the granted waiter. The current pre-fix ELF maps the reported caller address
to `ext2_unlink_impl`.

`ext3_journal_start` removed its wait entry after waking, then returned EINTR
if a signal was pending. If the finishing owner had granted it the journal,
the return left that grant unconsumed, with the transaction lock held and no
operation in progress. A handled signal need not kill the thread, so the
thread reaper could not be relied on to reclaim ownership. All subsequent
writers could wait indefinitely.

An interrupted waiter now checks for an unused grant after withdrawing its
entry and forwards ownership to the next waiter (or releases the lock if the
queue is empty) before returning EINTR. Grant selection and publication both
hold `sched_lock`: this also covers a waiter interrupted after its entry was
selected but before its grant was published. Once the withdrawn waiter has
acquired that lock, no delayed publisher can still give it ownership.

The actual-source `test_ext3_empty_commit.c` regression now covers interrupted
handoff with an empty queue, forwarding to a queued successor, and interruption
before any grant. Each case checks that ownership is released and later
transactions can proceed. The expanded suite passes under ASan/UBSan, including
the existing FIFO, concurrent writer, commit failure and recovery cases. The
same expanded test against the pre-fix implementation fails because the
interrupted waiter's ownership remains recorded.

The journal user-copy regressions also pass. `make -j8 avoryos-x86_64.iso`
succeeds with the existing linker RWX LOAD-segment warning.

Guest confirmation requires booting the rebuilt ISO and repeating the desktop
workload. No elapsed guest performance improvement has been measured.

## Follow-up: protected handoff and accurate owner diagnostics

A further guest report still showed `active=1 depth=0 phase=idle`, at
`hold_ms=5194`. That observation alone does not establish a stranded grant:
the old handoff preserved the previous owner's command, caller address and
start time, so the reported hold included the preceding transaction. A waiter
can cross its warning threshold just as that transaction finishes.

There was also a separate unsafe window in `wait_queue_take_head`: it cleared
the selected entry's back-pointer and deadline and returned a thread pointer
before the journal published ownership or woke it. Local preemption could
delay the wake after disabling the safety deadline; remote thread cleanup
could bypass the queue lock via the cleared back-pointer, miss the unpublished
owner, and free the selected thread.

`wait_queue_handoff` now publishes the grant and wakes the oldest live waiter
under the queue lock, with interrupts masked. It clears the back-pointer only
after the last use of the thread pointer, so cleanup must wait for the wake to
finish. The journal also masks interrupts between resetting the finished
transaction and waking its successor, and while forwarding an interrupted
grant. Allocation cleanup remains outside this short critical section. FIFO
ordering is unchanged.

Publication now sets `phase=handoff`, copies the recipient's command, clears
the old caller address, and starts a fresh timestamp. Warnings include
`granted=`. In the handoff phase `hold_ms` measures time since the grant;
once the recipient enters its operation, the normal transaction clock and
caller address are recorded. A continuing handoff stall can therefore be
distinguished from a long operation/commit without attributing the previous
transaction's duration or command to its successor.

`test_wait_queue_handoff.py` compiles the actual wait-queue handoff under
ASan/UBSan and starts concurrent cleanup during publication. It verifies
cleanup cannot bypass the queue lock, the deadline survives until publication,
publication precedes wake, live waiters remain FIFO, dead entries are skipped,
and IRQ state is restored. Moving back-pointer clearing before publication
makes the regression fail. The actual journal suite also checks recipient
identity, fresh timestamps and phase/caller reset on handoffs. Journal,
scheduler wake-publication and user-copy regressions pass; the ISO build
succeeds. Confirmation of the reported guest latency remains pending.
