# Corrections after the supplied PSP initialization failure

These corrections address a proven teardown defect and harden failure reporting.
**They do not establish that PSP initialization, the observed 0x60 teardown crash,
rendering corruption, or physical host reboots are fixed.** No passthrough run,
GPU reset, host power/bridge operation, or firmware-state change was performed.

## Changes

- CONFIRMED BUG: the pinned imported `ttm_resource_manager_evict_all()` declares
  `ret` without initializing it, then tests it after traversing the manager's
  LRU lists. An empty manager can return an arbitrary error instead of waiting
  on its move fence and reporting the actual result. Manager finalizers use this
  result to decide whether cleanup may proceed. The LinuxKPI entry point in
  [ttm_eviction.c:14](/home/offihito/AvoryOS/kernel/linuxkpi/src/ttm_eviction.c:14)
  follows the existing eviction algorithm with `ret = 0`. A compile-time symbol
  rename selects it while leaving the imported source unchanged. Eviction
  errors, move-fence waits, lock ordering and reference ownership are preserved.
  This is a real defect in an affected teardown path; a causal link to the
  supplied null-manager crash has not been demonstrated.
- Failure containment: [initcalls.c:61](/home/offihito/AvoryOS/kernel/linuxkpi/src/initcalls.c:61)
  requires an acquire-observed finished flag published only after the walker
  returns. A stray completion cannot release the boot dependency. If the
  worker's native pointer disappears before the walker finishes, boot logs an
  explicit error and stays blocked. Slow initialization still waits and joins
  normally; no thread is forcibly terminated and no reset is attempted. The
  existing logged inline fallback uses the same finished publication. Its idle
  context limitation remains. This is hardening motivated by the supplied
  trace, not proof that a stray completion caused that trace's continued boot.
- Diagnostics: [amdgpu_trace.c:28](/home/offihito/AvoryOS/kernel/linuxkpi/src/amdgpu_trace.c:28)
  logs failed PSP wait register, expected value, mask, a status read taken after
  failure, and the original result. The family-specific PSP callbacks reach
  this entry point through a compile-time rename of the imported implementation.
  Successful waits add no status read; failures add one read and preserve the
  original return value. There are no command retries or PSP register writes.
  Upstream `psp_hw_init()` can still convert its failure into -22, but the earlier
  original wait error is now visible for these callbacks. This does not change
  the PSP firmware protocol or make a failed handshake succeed.

## Validation

[Actual CPU regression output](linuxkpi-visible-failure-tests-2026-10-14.txt):
**41/41 scripts passed**, including ASan/UBSan resource-wrapper tests.

- New TTM test covers empty and occupied managers, pending move-fence drain,
  eviction/fence errors, and balanced locks/references. The imported implementation
  fails the same empty-manager assertion with deterministic automatic-variable
  poison; the correction passes. Poison is used only in the test harness.
- Initcall test injects an early completion and separately an aborted worker.
  The boot gate remains closed; normal completion after 620 simulated seconds
  and the existing creation-failure fallback still pass. The previous
  notification-only implementation fails the early-completion control.
- PSP wrapper test verifies original argument/result forwarding, no added read
  on success and exactly one status read on failure. Existing trace-wrapper
  lifetime tests still pass. Its initial harness needed the new PSP mock added;
  the final harness and all regressions pass.
- `make -C kernel -j4` passed and linked the new entry points. Existing compiler
  and linker RWX-segment warnings remain. `git diff --check` passed.
- The ISO is rebuilt with the same `run-c7-prime` command line; building the ISO
  does not launch QEMU. The previous ISO was copied to
  `build/gpu-corruption-audit/host-reset-2026-10-14/iso-before-visible-fixes.iso`
  before replacing it. The original failure log is preserved separately.

## What remains

The supplied log first fails PSP ring creation, then reports IRQ reference
warnings and unclean TTM managers, and finally faults at 0x60. The instruction
at the fault address in the preserved pre-change ISO matches the previously
symbolized TTM LRU-manager access. Missing manager/late BO cleanup is a strong
location hypothesis; the caller/ownership chain remains unproven without the
faulting registers/stack and a reproducer. No null-pointer guard was added to
TTM to skip cleanup, and no warning or firmware error was suppressed.

The user-reported improvement after orderly guest shutdown is still a
correlation. The guest's shutdown callback suspends AMDGPU IP blocks; closing
QEMU skips that guest path, while host VFIO also has close/open reset and power
paths. These changes do not turn forced QEMU close into orderly GPU shutdown
or establish that subsequent initialization is safe. Further physical attempts
are not part of this validation.
