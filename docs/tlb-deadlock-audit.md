# Firefox TLB shootdown hang

The supplied report resolves `0xffffffff816d1d00` to `shootdown_lock`.
CPUs 0, 4 and 7 wait on it; CPU 6 cannot answer a probe or take timer ticks.
`vmm_lock` is free and no shootdown was initiated under that lock. Stack
samples place the affected Firefox workers in mmap/mprotect/madvise and the
VMM deferred-drain path. The stack samples are scans, not unwind backtraces.
Without shootdown ownership or pending-ack snapshots, this report alone cannot
prove exactly where CPU 6 stopped.

The xAPIC sender had two concrete protocol defects: no delivery-status idle
wait before replacing the ICR, and no IRQ guard around the high/low register
pair. Bursts could overwrite pending sends, and interrupt handlers could
replace the destination between writes. A lost shootdown request leaves an
initiator waiting for an acknowledgement while holding `shootdown_lock` with
interrupts disabled. This is a plausible explanation for the report, pending
verification with the original Firefox workload on the affected hardware.

The sender now masks IRQs across each complete send and waits for xAPIC
ICR idle before programming another request. Broadcasts use the same path.
x2APIC retains its single MSR write without delivery-status polling. Panic
NMI sends retain their existing bounded wait. Linux uses the same xAPIC
idle-wait and IRQ-exclusion principles in
[its IPI implementation](https://code.googlesource.com/linux/torvalds/linux/+/90f055df112162fd9e093c16be1c21f38c35b907/arch/x86/kernel/apic/ipi.c).

Shootdown target selection now happens after acquiring the serialization lock,
with interrupts masked. This avoids keeping CPU identity and remote CR3
observations across the interruptible acquisition wait. The shootdown lock
also has an always-on lock spotlight, naming its owner, call site and hold
length without requiring LOCKDIAG_OWNER=1.

Validation:

- `python3 scripts/tests/test_lapic_icr.py`: exercises the actual sender function
  bodies with a simulated busy APIC, IRQ assertions, consecutive destinations,
  broadcasts, disabled-IRQ callers, panic NMI and x2APIC. The old implementation
  fails the IRQ-exclusion assertion; the corrected implementation passes.
- `make userland/test_tlb_stress.elf` and native execution: pass.
- An eight-CPU QEMU TCG guest passes 2,400 concurrent mapping cycles, including
  page faults, read/write permission transitions, discard/zero verification and
  unmaps. No hang report occurs. This test uses a temporary kernel with the
  unrelated LinuxKPI boot-test suite omitted to reach userland; the production
  source and ISO retain that suite.
- Normal kernel and ISO builds pass. The existing RWX load-segment linker
  warning remains.

On a rebuilt disk image, `/bin/test_tlb_stress` runs the same workload. Full
Firefox reproduction on the affected machine is still required to establish
that this particular hang is eliminated; QEMU cannot reproduce every physical
APIC timing condition.
