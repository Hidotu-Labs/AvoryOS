# AMDGPU partial-init cleanup KPF

Date: 2026-10-15. CPU regression reproduces the reported fault with the old
macros and passes with the correction. Guest confirmation is pending.

The user's trace first reports `ring vcn_dec_0 test failed (-110)` and failure
of `vcn_v3_0` hardware initialization. During the resulting cleanup, the
`kpi/initcalls` thread faults at CR2 `0x148` in `prepare_to_wait_event`, called
by `drm_sched_entity_flush` through `drm_sched_entity_destroy` and
`amdgpu_ttm_set_buffer_funcs_status`.

## Demonstrated fault

SDMA initialization creates the TTM move entities before AMDGPU initializes
the ring schedulers. VCN hardware initialization occurs before
`amdgpu_device_init_schedulers`; when it fails, those entities exist but their
run queues still have a null `rq->sched` pointer. They are idle and have no
queued jobs.

The imported `drm_sched_entity_flush` passes `sched->job_scheduled` to
`wait_event_killable` with `drm_sched_entity_is_idle(entity)` as its condition.
The pinned Linux wait macro checks that condition first and returns immediately
when it is true. The AvoryOS LinuxKPI macro instead always called
`prepare_to_wait_event`, before testing the condition. That computes and touches
the wait queue at null scheduler plus its `job_scheduled` offset, matching
the trace's `0x148` write fault.

The fix restores the condition-first behavior for ordinary, interruptible,
and killable waits in `kernel/linuxkpi/include/linux/wait.h`. It also restores
the timeout fast-path contract: an already-satisfied zero-timeout wait returns
one; an unsatisfied expired wait returns zero without registering a waiter.
The actual wait loop still rechecks the condition after registration, preserving
wakeup-race handling and signal results.

This corrects LinuxKPI behavior rather than hiding low-address faults or
changing the imported GPU scheduler. The VCN timeout remains a separate
unresolved hardware-initialization failure. This change does not disable
VCN/JPEG, alter reset/firmware protocols, extend GPU timeouts, or claim that
AMDGPU now initializes successfully.

## Verification

`python3 scripts/tests/test_kpi_wait_fastpath.py` compiles the actual wait
macro family and actual imported DRM entity-flush function with ASan/UBSan.
The fixture reproduces an idle entity created before scheduler initialization,
then exercises normal queued work on a valid scheduler. It also tests satisfied
waits with a deliberately unusable `0x148` queue, timeout boundaries, raced
completion during registration, and interruptible/killable signal results.

The old header produces a null-scheduler member access and attempts to register
the invalid wait queue. The corrected header never evaluates that wait-queue
expression when the entity is idle. Existing GPU FPU/free correctness, MODE2,
and doorbell teardown regressions pass.

All fcntl experiment files remain restored to their pre-change versions.
Audio and journal sources/tests remain byte-identical to their working
versions. No hardware initialization trial or host power operation was run.
