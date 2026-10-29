# Network worker sleep publication fix

`kernel/src/net/core.c` previously marked the RX worker BLOCKED before linking
its wait entry or installing its fallback deadline. A local timer interrupt
could schedule the worker out at that point, preventing it from finishing the
registration needed to wake it. The dedicated TCP timer similarly published
SLEEPING before setting its deadline, allowing an indefinitely stranded timer.

The RX worker now masks local interrupts while publishing the deadline, state,
wait entry and queue recheck. Its wake cleanup removes the entry and clears the
deadline under the same protection. The TCP timer installs its deadline before
publishing SLEEPING, with local interrupts masked throughout publication.
Packet processing and voluntary scheduling run with the original interrupt
state restored; the patch does not hold an RX lock while scheduling.

## Validation

- `test_net_worker_sleep.c` compiles the real network core with interrupt and
  scheduler mocks. It covers a normal RX park, a wake during registration,
  packet arrival during registration, cleanup/re-registration, and TCP timer
  sleep publication. ASan/UBSan pass. The saved original core fails the RX
  interrupt-mask assertion and, in a separate timer-only run, the assertion
  detecting a sleeping thread without a deadline.
- Existing RX queue test passes 12,000-packet bursts, allocation-pressure
  recovery, generation rejection and 100,000 concurrent packets without leaks.
- Updated the TCP timer fairness test for the current deadline tree (it still
  referenced the removed `timer_cursor`). Expired ACKs across 72 connections,
  SYN retry fairness and idle timer handling pass under ASan/UBSan.
- KVM RTL8139 tests pass with 8 CPUs / 30 seconds and 1 CPU / 15 seconds of four
  simultaneous TCP streams, plus UDP, IPv6, ICMP, carrier events and recovery.
- Kernel build succeeds; the existing RWX LOAD-segment linker warning remains.

Artifacts and backups are in `build/firefox-network-fix/`. The ISO candidate
preserves the existing boot files/configuration and replaces `/boot/kernel`.
The packaged kernel is extracted and compared byte-for-byte before installation.

This fixes two concrete networking races. It does **not** establish that the
reported sustained CPU spike after reopening YouTube has been resolved.
The separate browser probe uses a clean profile; the user's saved profile was
not run against YouTube. A test-only Firefox archive changes Marionette's
listener binding because this guest lacks localhost routing; its host forward
binds only 127.0.0.1. That archive is confined to disposable VM filesystems.

The production ISO was rebuilt independently during testing. Its extracted
`/boot/kernel` matches the tested, patched kernel byte-for-byte, so it was
verified in place rather than overwritten. Current ISO SHA-256: `229a036e71a5206183d49564307f88364c961ff2a67e68da23947ddf5768bbd3`.
The earlier ISO backup is `build/firefox-network-fix/iso-before.iso`.
The main `disk.img` was not modified by this fix.

The real-site probe loaded YouTube and played 1080p before the tab cycle;
the reopened video remained paused in the final run. This is partial browser
coverage, not proof that post-reopen playback or the CPU spike is fixed.

## Subsequent exact-sequence investigation

The Plasma 6 / RTL8139 tab-cycle investigation subsequently isolated an
interrupt-completion race and allocator pressure-query contention. See
[the reproduction, fixes and final installed ISO](firefox-rtl8139-reopen-2026-10-28.md)
for the later three-cycle playback results and current image identification.
