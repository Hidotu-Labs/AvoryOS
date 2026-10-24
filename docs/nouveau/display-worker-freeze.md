# Display worker wait-publication race

The native VirtIO completion and asynchronous presentation workers published
THREAD_BLOCKED with interrupts enabled, before linking their wait entries.
A timer could switch them out in that gap. In interrupt mode there was no
fallback sleep deadline, and device wakeups had no entry to find. The worker
could therefore stay blocked indefinitely while unrelated kernel threads ran.
The presentation worker also left itself BLOCKED during ready-path cleanup
until after unlinking its entry, creating another unprotected window.

Both waits now mask local interrupts across deadline/state publication,
registration, readiness recheck and ready-path cleanup. Interrupts are restored
before yielding; by that point the wake entry is installed. The producer state
recheck retains work that arrived before registration. The polling-mode worker
publishes its fallback deadline before becoming blocked. The command-response
wait already uses an IRQ-masked publication window and was retained.

This applies when Nouveau renders buffers presented through the native VirtIO
scanout backend. Local desktop logs show that arrangement (NVIDIA renderD129,
VirtIO card0 output). They do not establish that this race caused the user's
reported random freeze. The older mode-fix-frozen serial capture includes a
separate TLB shootdown failure and is not a reproduction of this fix.

Validation:

- test_gpu_worker_wait_publication.py compiles the actual worker wait blocks
  and injects timer checks at statement boundaries. It covers ready-at-entry,
  wake during registration, wake before yield, wake during yield, both workers,
  and IRQ/polling modes. Removing IRQ protection deterministically reproduces
  an unregistered blocked worker.
- The changed VirtIO GPU object builds.
- PRIME scanout lifetime and frame publication regressions pass.

The change has not been boot-tested on the NVIDIA desktop. No GPU reset,
forced flip-complete event, firmware change or installed image update is part
of this fix.

## Subsequent interrupted-bind report

The user reported another freeze next to the messages `Nouveau synchronous
VM_BIND interrupted; waiting for completion tgid=405`. Those exact messages
are present in `build/logs/nouveau-c4.log`. That saved log continues with
framebuffer creation and later glxgears activity after the messages; adjacency
does not establish an indefinite VM_BIND stall. The VM was closed before a
live thread dump could be captured. The earlier worker-race fix is therefore
not evidence that the reported desktop freeze is resolved.

The interrupted-bind recovery now waits in one-second slices, retaining its
requirement that the submitted mapping complete before it reports success.
This periodically observes completion even if a wake notification is missed.
A five-second stall prints the fence context, sequence, flags and elapsed
wait, and requests one native thread dump per boot. Further warnings are
limited to one per five seconds per stalled wait. A later completion is
explicitly logged. A GPU job that truly never completes still waits; this
change neither cancels the mapping nor returns false success.

The focused test injects six timed-out slices before completion and checks
that the mapping is not reported complete early and that stall diagnostics
fire. Native and LinuxKPI objects compile. No live GPU reproduction has
validated this recovery or established the remaining freeze's cause.
