# Software vblank wait publication

Reported symptom: the entire guest display sometimes stops updating in Plasma
on Wayland and X11, while audio continues. The report spans multiple renderers.
There is no captured stack trace of the reported freeze, so its exact cause
remains unconfirmed.

Inspection found an independently reproducible indefinite wait in
`kernel/src/fb/vblank.c`. `vblank_wait_next()` set `THREAD_BLOCKED` with local
interrupts enabled before registering the wait entry and setting
`wakeup_ticks`. Timer preemption in that interval lets the scheduler dequeue
the thread without a deadline. The software vblank queue has no periodic
producer to wake it: the thread itself must publish the scheduler deadline.
This can strand a display client or the VirtIO presentation worker while
other threads continue running.

The fix masks local interrupts across wait publication and cleanup, rounds
the scheduler deadline up to milliseconds, rechecks pending signals after
registration, and registers a fresh entry on each sleep after an early wake.
It preserves the caller's interrupt state.

Validation:

- `bash scripts/tests/test_vblank_wait.sh` executes the actual vblank source
  with simulated timer checkpoints, early wake detachment and signal races.
- The same harness against the saved original source aborts at
  `vblank waiter stranded before deadline`; the fixed source passes.
- `make -C kernel -j4` compiles and links successfully. The linker emits the
  existing RWX LOAD segment warning.
- An isolated four-vCPU KVM guest completes 960 `DRM_IOCTL_WAIT_VBLANK`
  requests across four concurrent clients. Each client verifies event
  cookies, advancing sequences, and nonblocking event queue draining.
  `build/drm-vblank-fix/serial-stress.log` records the successful result.

The rebuilt kernel is installed in `avoryos-x86_64.iso`. Its extracted kernel
was compared with the intended stripped kernel before publication. The
previous ISO is saved at `build/drm-vblank-fix/iso-before.iso`.
The guest stress test uses a private filesystem and does not attach `disk.img`.

This validates the software vblank repair. It does not establish that all
Plasma freezes or physical AMD/Nouveau display paths share this cause; the
original desktop workload still needs a retest.
