# Native completion token and publication lifetime correction

The remaining RadeonSI pixel corruption has not been reproduced by the
headless pipeline oracle: the two physical diagnostic boots failed during
GPU startup before registering an AMD render node. The user requested a
native audit without another physical GPU boot. This change fixes proven
LinuxKPI completion defects; it is not a verified pixel-corruption fix.

## Defects and correction

The old blocking wait APIs checked `done` without holding the queue lock,
then decremented it in a separate locked operation. Two waiters could both
observe one token and consume it. The second decrement changed zero to
`UINT_MAX`, falsely marking the completion permanently ready. Subsequent
waits could return without their work finishing.

`complete()` and `complete_all()` also published readiness, released the
lock, then accessed the object's queue again to wake waiters. A waiter could
observe that readiness, return and release the object during that gap.
The new implementation keeps publication and wakeup within one locked
critical section; consumers check and decrement within the same queue lock.
Sleeping waiters join the queue at its tail and release the lock and restored
IRQ state only while invoking the native scheduler. Removal, state cleanup,
and token consumption finish under the queue lock.

The previous timeout fast path returned zero without consuming a ready token
when called with timeout=0. Such a call now returns 1 and consumes one token,
matching the [upstream completion implementation](https://github.com/torvalds/linux/blob/v6.6/kernel/sched/completion.c).
Interruptible and killable APIs retain interruption returns; a ready token
takes precedence over interruption. `complete_all()` remains ready until
`reinit_completion()`, which still requires callers to finish existing waits
before reinitializing.

Native `kthread_stop()` waits on the control object's `exited` completion,
then immediately frees that object. Its producer calls `complete()` as its
last action. The old publication-to-wakeup gap therefore affects a real
native allocation lifetime, not merely an abstract completion contract.
DRM scheduler entity teardown waits on `entity_idle`, which the scheduler
publishes with `complete_all()`. This makes the publication lifetime rule
relevant to actual GPU context teardown. It does not establish that this race
occurred in the affected Minecraft/Mesa workload. No Mesa, AMDGPU vendor source
or game preferences were changed.

## Regression evidence

`scripts/tests/test_kpi_completion.py` compiles the actual completion source,
waitqueue source and wait macros with pthread-backed native scheduling. The
old implementation is preserved at `build/logs/linuxkpi-completion-before.c`
(also captured at `/tmp/avory-completion-before-claim-fix.c`).
Each targeted negative control fails:

- `build/logs/completion-before-zero-regression.log`: zero-time ready wait.
- `build/logs/completion-before-race-regression.log`: two consumers, one token.
- `build/logs/completion-before-wake-regression.log`: lock released before
  the queued waiter's wake callback completes.

The old unlocked-read race is forced by parking both reads after they observe
the same token; the corrected implementation claims under the lock and does
not take that instrumented read path. Lock instrumentation verifies that
notification completes before the producer releases its publication lock.
The corrected source passes ASan/UBSan, every implemented wait API, pending
signal cases, timeouts, permanent completion/reinitialization, counted tokens,
and eight competing waiters consuming 32,000 signals. Results are saved in
`build/logs/linuxkpi-completion-regression.log`.

The native boot self-test now checks zero-time waits and two kernel threads
sharing one completion signal. It logs `[KPI-COMPLETION] waiters=2 signals=1
consumed=1 remaining=0`. Private guest validation attaches no physical AMD GPU
and never opens the user's `disk.img`. Hardware artifact validation remains
outstanding.

The rebuilt ISO passes the new completion contracts on one-CPU and eight-CPU
KVM fixtures. Each fixture also completes 400 flips and 400 damage updates;
116 and 72 active host frames respectively, and the final 1,024,000 pixels,
match the CPU oracle. These fixtures deliberately lack AMD firmware and
attach no physical GPU. Their missing-firmware tests and the pre-existing
vkms PMM drift are not represented as passing hardware validation.

An eight-CPU private AvoryOS boot with the original Mesa/llvmpipe passes the
texture/mip/fog and UBO/storage software oracles, prints the completion-credit
marker, and powers off normally. The software overlap cases remain labelled
inconclusive, as expected. Evidence:

- `build/logs/linuxkpi-completion-kvm{1,8}-{boot,host,result}.log`
- `build/logs/gpu-pipeline-completion-software.log`
- `build/gpu-pipeline-boot/software-serial.log`

`avoryos-x86_64.iso` is rebuilt with the original PRIME/AMDGPU command line.
The user's installed disk and Minecraft settings are unchanged. An affected
RadeonSI workload must still be checked before claiming its corruption fixed.
