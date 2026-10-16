# Scheduler attribute syscalls

The native x86-64 syscall table now implements `sched_setattr` (314) and
`sched_getattr` (315). This supplies the interface missing from Epiphany's
PressureMonitor log; it is separate from the WebKit PulseAudio launch assertion.

The implementation accepts the original 48-byte and extended 56-byte Linux
structures, including the zero-size compatibility convention. Larger inputs
up to one page are accepted if their extension bytes are zero; otherwise the
call returns E2BIG and writes back the supported size. Getters copy only the
supported prefix, preserving the caller's extra bytes. All user accesses use
the fault-safe user-copy routines.

OTHER uses the requested clamped nice value. BATCH uses that value with the
existing batch slice; IDLE maps to nice 19 with the longest slice. FIFO and RR
use AvoryOS's existing EEVDF high-priority mapping and do not supply Linux
real-time scheduling guarantees. Their requested policy and RT priority now
round-trip correctly through both the new and legacy scheduler interfaces.
Policies are tracked independently from nice: OTHER with nice 19 remains OTHER.
Policy, weights, and slices change while holding the CPU runqueue lock, with
queued entities removed and reinserted to retain deadline-tree ordering.

Supported attribute flags are RESET_ON_FORK, KEEP_POLICY, and KEEP_PARAMS.
Fork, clone, and vfork inherit policy state; RESET_ON_FORK clears negative nice
and FIFO/RR in the child and is itself cleared in the child. Unprivileged
callers cannot change another user's threads, raise priority, select RT policy,
or remove an existing reset flag. Full capability and RLIMIT_RTPRIO/RLIMIT_NICE
delegation are outside the existing scheduler's permission model.

DEADLINE, its reclaim/overrun flags, and utilization clamp requests return
EOPNOTSUPP. AvoryOS has no deadline admission controller or CPU-utilization
governor. Unknown policies and flags return EINVAL. Unused deadline timing and
utilization fields are ignored for ordinary policies when their flags are clear.

The ABI follows the primary
[Linux scheduler syscall implementation](https://github.com/torvalds/linux/blob/master/kernel/sched/syscalls.c).
In particular, get reports the size of the returned prefix and set validates
zero-filled future extensions.

Validation:

```
make -C kernel -j4
python3 scripts/tests/test_sched_attr_boot.py
python3 scripts/tests/test_scheduler_wake_publication.py
python3 scripts/tests/test_sched_deadline_tree.py
```

The scheduler attribute regression boots the current kernel in a private
four-CPU KVM guest. It checks policy/priority/nice round trips, both legacy
scheduler interfaces, structure sizes and future extensions, bad pointers and
page boundaries, per-thread changes, fork/clone inheritance, reset-on-fork,
ownership/priority permissions, and unsupported features. It powers off through
the normal shutdown path. It does not access the user's disk image, host GPU,
or audio devices. Logs are in `build/sched-attr-boot`.
