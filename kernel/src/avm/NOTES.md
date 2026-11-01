# AVM SVM bring-up: verification notes

Status: the SVM constants are not yet checked against the AMD APM text. The
boot probes exercise most of the paths, so the values below are confirmed
where a probe runs them.

## Attempts

- AMD64 APM Vol 2 (publication 24593): fetch timed out several times, so the
  VMCB table (Appendix B, Table B-1) has not been read.
- A web search for the VMCB offsets returned no offset table.

## Values in use (src/avm/svm.h)

| Constant | Value | Evidence |
| --- | --- | --- |
| CPUID 8000_0001h ECX[2] (SVM) | bit 2 | Unverified. Boot probe reports SVM present on the nested host. |
| CPUID 8000_000Ah EDX[3] (next RIP) | bit 3 | Unverified. Probes depend on it for every intercept skip. |
| VM_CR MSR | C001_0114h | Unverified. Boot probe read it without fault. |
| VM_CR.SVMDIS / LOCK | bits 4 / 3 | Unverified. |
| VM_HSAVE_PA MSR | C001_0117h | Boot probe wrote it on the BSP without a #GP. |
| EFER.SVME | bit 12 | Boot probe set it on the BSP without a #GP. |

A #GP on an unknown MSR would have stopped the boot, so the two MSR numbers
exist. That does not confirm their meaning or the bit positions.

## Checked by the boot probes (KVM accelerator, nested on the Ryzen host)

- VMRUN, IOIO, HLT, CPUID, MSR, SHUTDOWN and INTR intercepts.
- IOPM and MSRPM base addresses, GUEST_ASID, TLB_CONTROL, NP_ENABLE, N_CR3.
- State save area: segments (including the unreal DS limit), CR0, CR2, CR3,
  CR4, EFER, RAX/RSP/RIP/RFLAGS.
- EXITCODE, EXITINFO1 (port, direction, size, MSR direction), nRIP, and the
  nested-fault exit (0x400) with EXITINFO2 as the faulting address.
- EVENTINJ for external interrupts (vector delivery) and for #GP.
- VM_HSAVE_PA, EFER.SVME, VMSAVE/VMLOAD around VMRUN.

Not yet exercised: a #GP injected for an unknown MSR, SHUTDOWN, exception exits
(0x40 + vector), and VMEXIT_INVALID.

## Still to verify against the manual

- Confirm the offsets and bit positions above against APM Vol 2 Appendix B.
  The boot probes prove they work for the paths they run; the manual is still
  the reference for the rest.
- GIF behaviour around VMRUN. The run loop does not use CLGI, so GIF stays set
  during the guest and the INTR and NMI intercepts fire. The probes confirm
  this on the nested host, where an INTR intercept only fires while the guest
  has IF set; NMI kicks cover guests with IF clear.
- NPT entry bits (the standard x86-64 PTE format is assumed).

## Phase 2: interrupts (done, gate passes)

Gate: a real-mode guest gets timer ticks from the in-kernel LAPIC. The probes
below check this and the rest of the phase 2 plan.

- In-kernel LAPIC (`avm_lapic.c`), in x2APIC MSRs and xAPIC MMIO. Covers ID,
  TPR, PPR, SVR, LVT entries, EOI, ISR/IRR/TMR, the timer, and IRQ raising from
  the IOAPIC. ICR is not supported (phase 3).
- LAPIC timer: periodic and one-shot, on a 1 MHz clock divided by the divide
  register. TSC-deadline mode (MSR 0x6E0) is rejected.
- Timer and IOAPIC interrupts are delivered through EVENTINJ, gated on guest
  IF, TPR and the in-service set. The LAPIC has priority over the PIC.
- Host timer callbacks: before each VMRUN the next guest deadline (LAPIC timer
  or PIT) is armed on the host timer, and INTR is intercepted, so the host
  interrupt forces an exit and the timer is delivered on time. A halted guest
  with IF set sleeps on `sti; hlt` until a host timer or interrupt wakes it.
- 8259 PIC pair (`avm_irq.c`): ICW1-4, OCW1-3, cascade through IRQ 2, fixed
  priority, specific and non-specific EOI. Its state is guarded by the VM's
  `irq_lock`.
- PIT channel 0: modes 0, 2 and 3, lo/hi access, latch, and KVM_CREATE_PIT2.
  Modes 1, 4 and 5 act as mode 0. Channels 1 and 2 are not modelled, so ports
  0x41, 0x42 and 0x61 still go to the VMM.
- IOAPIC (`avm_ioapic.c`): IOREGSEL/IOWIN, the redirection table with 24 pins,
  fixed delivery to a physical destination. KVM_IRQ_LINE drives pins 16-23.
- MMIO decoding (`avm_mmio.c`): mov forms 88, 89, 8A, 8B, A0-A3, C6, C7 with
  16- or 32-bit operand and address sizes and the usual prefixes. Accesses to
  the APIC and IOAPIC pages are done in the kernel; other accesses go to the
  VMM as KVM_EXIT_MMIO, and the VMM's answer is written back on re-entry.

Probes: `test_avm_lapic_boot.py`, `test_avm_pic_pit_boot.py`,
`test_avm_ioapic_boot.py` (xAPIC MMIO, the IOAPIC via a VMM IRQ, and
KVM_EXIT_MMIO), and `test_avm_busy_timer_boot.py` (a guest that never halts
still gets its timer ticks).

Limits:

- Level-triggered IOAPIC pins are treated as edge-triggered. There is no remote
  IRR, so no EOI handling for level interrupts.
- Only fixed delivery to physical destinations is modelled. NMI, SMI and logical
  destinations are ignored.
- The decoder assumes guest paging is off, so the linear address equals the
  guest-physical address. It does not decode REX (64-bit code) or string and
  REP forms.
- ICR (MSR 0x830, MMIO 0x300/0x310) is not supported. Writes are ignored.
- The host timer has millisecond granularity, so guest deadlines round up to
  the next millisecond.

## Deviations from the design

- KVM_RUN does not check for pending signals, so there is no KVM_EXIT_INTR.
- TLB_CONTROL flushes the guest TLB on every entry. Correct but slow.
- Exception exits and VMEXIT_INVALID still return KVM_EXIT_INTERNAL_ERROR.

## Phase 3: SMP (gate passes)

Gate: two guest vCPUs both reach HLT. Checked by `test_avm_smp_boot.py`: the
BSP sends INIT and SIPI to an AP, the AP sends a fixed IPI back while the BSP
is in guest mode, and both halt. Each vCPU runs in its own host thread.

- MP state per vCPU (`avm_smp.c`): an AP waits in KVM_RUN from creation until
  INIT, then until SIPI. INIT resets the APIC and SIPI sets CS to vector*4 KB
  with IP 0. Only INIT and SIPI from the BSP are tested.
- ICR writes, both x2APIC (MSR 0x830) and xAPIC (MMIO 0x300 with 0x310 staged
  first). Fixed IPIs set the target's IRR. Physical destinations and the
  shorthands are supported; logical destinations are ignored.
- Kicks: a vCPU records its host CPU and an in-guest flag around VMRUN. A kick
  sends an NMI to that CPU, which the NMI intercept turns into an exit. The
  host NMI handler accepts an NMI as a kick only when the kicker set a flag for
  that CPU; any other NMI is still fatal, as before. An NMI is used rather
  than a reschedule IPI because a guest with IF clear does not take
  interrupts, and a physical interrupt is only intercepted when IF is set on
  this nested host.
- Memslot removal (`size == 0` in KVM_SET_USER_MEMORY_REGION): drops the NPT
  entries, releases the pin references, clears MLOCKED on the VMM's pages, and
  kicks every vCPU. Tested by `test_avm_slot_delete_boot.py`, and with a
  second VM running side by side by `test_avm_multivm_slot_boot.py`: a guest with IF
  clear spins on a word while the VMM removes its page from another thread, and
  the guest's next read reaches the VMM.
- Per-VM ASID, cycled through the ASIDs the CPU supports (CPUID 8000_000Ah EBX).
  Tested by `test_avm_asid_boot.py`: two VMs run side by side, each keeps its
  own memory at the same guest address, and the kernel logs distinct ASIDs.
- XSAVE: each vCPU has a guest XSAVE area, and each CPU has a host area. The
  host state is saved and the guest state restored just before VMRUN, and the
  reverse happens after the exit. Interrupts are off for this whole stretch.
- XSETBV is intercepted (misc2 bit 13, exit 0x8D). Without OSXSAVE in CR4 it
  raises #UD. A guest may only set XCR0 to the host's own value (ECX must be 0);
  anything else raises #GP. This is needed because SVM cannot intercept XGETBV,
  so a guest that picked a different XCR0 would read back the host's value.
  The host's XCR0 is never written. Tested by `test_avm_xsetbv_boot.py`.
- Migration: the VMCB and guest XSAVE area belong to the vCPU, and the host
  save areas are per CPU and selected at each entry. Each entry logs when the
  vCPU's host CPU changed. Tested by `test_avm_migrate_boot.py`: the guest keeps
  a register value across a move, and at least one move is seen by the probe
  (`sched_getcpu`) and logged by the kernel. The scheduler does not reliably move
  a running thread when its affinity changes (`sched_enforce_affinity` only
  marks it, and the move happens at a later reschedule), so the number of moves
  varies from run to run; the test asks for one, not a fixed count.

Limits:

- A guest that sets XCR0 to a subset of the host's value gets #GP, so guests
  that want fewer state components are not supported yet.
- Logical destinations, SMI, STARTUP from the guest's own shorthand
  interactions, and INIT to a running BSP are not modelled.
- ASIDs are cycled through the ASIDs the CPU has. The TLB is flushed on every
  entry, which is what makes sharing an ASID safe.
- Nested virtualization here gates the INTR intercept on the guest's IF; the
  NMI kick is used so that guests with IF clear still exit. Bare-metal AMD
  behaviour has not been checked.

## Phase 4: QEMU startup ioctls (in progress)

Goal: QEMU (`-enable-kvm -cpu host`) gets past its KVM setup on AVM. Added so
far, each checked by a probe:

- Capabilities: DESTROY_MEMORY_REGION_WORKS (21) and EXT_CPUID (7) report 1.
- CPUID (`avm_cpuid.c`): KVM_GET_SUPPORTED_CPUID returns the default table
  (-E2BIG if the buffer is too small). KVM_SET_CPUID2 replaces the vCPU's
  table. A guest's CPUID reads only from that table, never from the host.
  The default table is the host's leaves 0, 1, 7, 0xD, 0x80000000-4 and 8,
  with bits AVM does not back cleared (MONITOR, VMX, TSC-deadline, MCA/MTRR/PAT,
  TSC_ADJUST, SGX, RTM, speculation-control MSRs, AMX, XSAVES). Leaf 1 ECX.OSXSAVE
  follows the guest's CR4. Hypervisor leaves 0x40000000/1 carry the "KVMKVMKVM"
  signature with no paravirtual features.
- MSRs (`avm_msr.c`): KVM_GET_MSR_INDEX_LIST reports APIC base (0x1B) and EFER.
  KVM_GET_MSRS and KVM_SET_MSRS handle those two and stop at the first other MSR.
  The feature MSR list is empty. Guest RDMSR/WRMSR of EFER or other MSRs still
  raises #GP.
- Probes: `test_avm_cpuid_boot.py` (table and MSR ioctls, and a guest that reads
  the VMM's table, expected "TZK") and `test_avm_ioctl_boot.py` (now also checks
  caps 21, 30 and 40).
- Internal errors use the ABI layout (`internal.suberror`, `ndata`, `data[]`) and
  report cap 40 (INTERNAL_ERROR_DATA).
- Ioeventfd (`avm_ioeventfd.c`, cap 36): KVM_IOEVENTFD registers a PIO port or an
  MMIO address, optionally with datamatch and a length. A matching write signals
  the eventfd (via `asc_eventfd_signal` in `syscalls/sys_aio.c`) and resumes the
  guest without an exit to the VMM. Only writes match. The table has 16 entries
  per VM and holds one eventfd reference each, dropped on deassign or VM teardown.
  KVM_CHECK_EXTENSION also works on the VM fd. Cap 118 (multiple address spaces)
  reports 0, since AVM has one address space. Probe: `test_avm_ioeventfd_boot.py`.

Startup surface added after that (`avm_state.c`, `avm_irqroute.c`,
`avm_vm_misc.c`), each checked by `test_avm_state_boot.py` or
`test_avm_irqfd_boot.py`:

- Registers and state: KVM_GET_REGS, KVM_GET_SREGS (read from the VMCB, or the
  last VMM value before the first entry), FPU (the XSAVE legacy area, in FXSAVE
  layout), XSAVE (the first 4 KB), XCRS (XCR0 only).
- LAPIC: KVM_GET_LAPIC and KVM_SET_LAPIC over the xAPIC registers. The ICR and EOI
  are left out.
- MP state, and vCPU events and debug registers at reset: pending events and
  breakpoints are refused, and everything else is accepted and ignored.
- Clock: KVM_GET_CLOCK reads host monotonic time; KVM_SET_CLOCK is accepted and
  has no effect. TSC frequency: GET returns the host rate, and SET accepts only
  that rate.
- GSI routing (KVM_SET_GSI_ROUTING): entries route a GSI to a PIC or IOAPIC pin,
  or to an MSI message. With no table, GSIs 0-23 keep the legacy numbering.
  KVM_IRQ_LINE goes through the table too.
- MSI: KVM_SIGNAL_MSI, and MSI route entries. Fixed delivery to a physical
  destination only.
- IRQFD (cap 32): a signal on the eventfd pulses its GSI, and the vCPU is kicked.
  Resample is refused. Each eventfd has one hook, which the kernel's eventfd
  (`syscalls/sys_aio.c`) now calls after each signal or write.
- Signals and exits: KVM_SET_SIGNAL_MASK sets the mask that applies during
  KVM_RUN. A pending signal that the run does not block returns EINTR with
  KVM_EXIT_INTR, and so does immediate_exit.
- Caps: 4, 14, 25, 32, 36, 37, 39, 41, 50, 55, 56, 61, 77, 122 and 136 report 1.
  Cap 118 (multiple address spaces) reports 0. MCE (31) and robust singlestep
  (51) report 0.

Limits of this surface:

- The signal check runs at the top of each run loop iteration. A vCPU halted with
  no timer deadline stays in the halt until an interrupt wakes it, so a signal
  does not interrupt that halt.
- XSAVE carries only the first 4 KB, so state components past 4 KB are not
  carried. XCR0 changes are stored but do not change what the guest's XSETBV
  allows.
- Debug registers are not kept. Events and the MP state are not migrated.
- The TSS address and identity map calls are accepted and do nothing, because SVM
  uses neither.

Debugging, machine checks, state tables and read-only slots (added after that):

- Single-step: KVM_SET_GUEST_DEBUG with KVM_GUESTDBG_SINGLESTEP sets TF in the
  guest and intercepts #DB. Each instruction then exits to the VMM as
  KVM_EXIT_DEBUG with the next RIP. An instruction the kernel completes (CPUID,
  MSR, XSETBV, I/O, MMIO, HLT) also reports one, after its completion. TF is
  hidden from the registers the VMM reads. BLOCKIRQ holds back interrupt
  delivery while stepping. Breakpoints (software or hardware) and injected
  debug exceptions are refused. Probe: `test_avm_debug_boot.py`.
- Machine checks: KVM_CAP_MCE reports the bank count, and
  KVM_X86_GET_MCE_CAP_SUPPORTED reports MCG_CTL_P plus the banks.
  KVM_X86_SETUP_MCE follows KVM: the low byte is the bank count (1 to 32), and
  MCG_CTL_P is the only other bit accepted. QEMU masks its MCG_CAP to what we
  report but keeps its bank count (10 by default), so the bank byte must pass.
  QEMU still warns "Unsupported MCG_CAP bits: 0x1000000" because MCG_SER_P is
  not offered; the warning is harmless, and offering SER_P would make QEMU
  route memory errors through SIGBUS handling we do not implement.
  KVM_X86_SET_MCE (injection) is refused, because the MCA MSRs are not
  emulated and the guest CPUID hides MCE and MCA.
- VMM-side MSRs (avm_vmm_msrs in avm_internal.h): QEMU writes TSC, SYSENTER_*,
  PAT, the MTRR MSRs and the MCG and bank MSRs on every vCPU reset, and reads
  them back. QEMU treats a short KVM_SET_MSRS or KVM_GET_MSRS count as a fatal
  error. QEMU adds MTRR, PAT, MCE and MCA to its own CPUID view whatever the
  kernel reports (kvm.c kvm_arch_get_supported_cpuid), so those writes happen
  even though our CPUID hides MTRR. These MSRs are stored per vCPU and round-trip,
  but the guest does not see them: a guest RDMSR or WRMSR of them still raises
  #GP, as before. Only APIC_BASE and EFER are in the index list, so QEMU's
  optional MSR writes (STAR, MISC_ENABLE, FEATURE_CONTROL, and the rest) are
  skipped. Probe: `test_avm_msr_boot.py`.
- In-kernel PIC, IOAPIC and PIT state: KVM_GET_IRQCHIP and KVM_SET_IRQCHIP
  (the PIC pair and the IOAPIC redirection table, in the hardware format), and
  KVM_GET_PIT2 and KVM_SET_PIT2 (channel 0 only). QEMU calls these at reset.
  A PIT that was never loaded does not arm IRQ 0. Probe: `test_avm_debug_boot.py`.
- Read-only memory slots (KVM_MEM_READONLY, cap 81): the NPT entry has no write
  bit, so a write exits to the VMM as MMIO and the VMM's copy is not touched.
  The page is still pinned with a write fault first, so its contents survive.
  QEMU's default BIOS sits in such a slot. Probe: `test_avm_readonly_boot.py`.
- Routing: KVM_CAP_IRQ_ROUTING reports the GSI count plus one (1025), as QEMU
  expects. The routing table holds up to 1024 entries.
- Caps newly reporting 1: 23, 35, 51 and 81 (and 31 with the bank count).

Left at 0, because QEMU only uses them when it asks for the feature, or AVM does
not implement the feature: KVM_CAP_XSAVE2, SREGS2, GET_MSR_FEATURES,
X86_SMM, X86_USER_SPACE_MSR, IRQFD_RESAMPLE, KVMCLOCK_CTRL,
IRQ_INJECT_STATUS, NESTED_STATE, COALESCED_MMIO, SYS_ATTRIBUTES, the dirty-log
and guest-memfd caps, and the TSC-control and hyperv caps. KVM_GET_STATS_FD is
not implemented: QEMU tolerates the failure at startup, and only the stats query
(`info kvm`) needs it.

How this set was chosen: from QEMU 11.1.2's own source (accel/kvm/kvm-all.c,
target/i386/kvm/kvm.c, hw/i386/kvm/*.c). The required-capability tables are
`kvm_required_capabilites` and `kvm_arch_required_capabilities`. The ioctls are
the ones the startup functions issue unguarded, plus the guarded ones that AVM
supports. The next blocker, if any, shows up only when QEMU runs on this kernel.

Still open: kvmclock (4.2) and the paravirtual features that depend on it, and
KVM_GET_STATS_FD.

Limits of the CPUID table:

- XCR0 is fixed to the host value, so leaf 0xD EBX reflects the host, not the
  guest's current XCR0. Guests use the sizes from the host's XCR0 only.
- Leaves 2, 4, 5, 6, 0xA and 0xB are not in the table and read as zero. The
  VMM can supply them with KVM_SET_CPUID2.

## Pinning (phase 1, task 6)

- Pinned pages: each memslot page is made private and writable through the
  normal fault path, then gets a frame reference and the PTE `MLOCKED` bit.
- zram swap skips `MLOCKED` PTEs (zram.c), so swap cannot move a pinned page.
- KSM only accepts frames with refcount 1 as candidates (ksm.c), and a pinned
  frame has refcount 2, so KSM leaves it alone.
- NUMA migration did not check `MLOCKED`, so `numa_migrate_page` now returns
  `-EBUSY` for locked pages (src/mm/numa_migrate.c). This changes behaviour for
  every `mlock`ed page, not just KVM ones.
- Not yet covered: `munmap` of a pinned range (the frame stays alive through its
  reference, but the guest keeps a stale mapping), and a `fork` of the VMM,
  which makes the pinned pages copy-on-write. Both need handling before the
  VMM is used for real.
- Each memslot translation is checked against the VMM's page table at
  `KVM_SET_USER_MEMORY_REGION`; the boot probe checks the log line.

## Guest-visible MSRs and SYSCALL (the VMCB as the home)

- The guest's EFER, PAT, STAR, LSTAR, CSTAR, SFMASK, KernelGSBase, the SYSENTER
  MSRs, and the FS and GS bases live in the VMCB save area. The VMM's
  KVM_GET_MSRS / KVM_SET_MSRS and the guest's RDMSR / WRMSR use the same fields
  (svm_msr_get / svm_msr_set in svm.c). They cannot disagree.
- VMLOAD and VMSAVE (svm_run.asm) move these fields around each VMRUN, along
  with FS, GS, TR, LDTR, and the host's copies. VMRUN itself does not load them.
  A WRMSR-based swap was considered and not built: it cannot load TR or LDTR, so
  it could only repeat what VMLOAD already does.
- Validation (the guest sees #GP, the VMM sees EINVAL): EFER may only set SCE,
  LME, and NXE (LMA is read-only; SVME is forced on); LME cannot change while
  CR0.PG is set; PAT entries must be valid memory types; LSTAR, CSTAR,
  KernelGSBase, and the FS and GS bases must be canonical.
- VMM-only MSRs (avm_vmm_msrs in avm_internal.h): TSC, MTRR, MCE, and the K7
  performance counters. The guest cannot see them.
- AMD PMU: QEMU always writes the K7 counters for an AMD vCPU, so they are
  stored. The F15h core-counter feature (PERFCORE, CPUID 8000_0001h ECX bit 23)
  is cleared, so QEMU does not write the F15h counters, which we do not provide.
- Probes: test_avm_msr_boot.py (round trip of the VMM set, 88 entries),
  test_avm_longmode_boot.py (a 64-bit guest that sets SCE through WRMSR, swaps
  GS with swapgs, and reaches its own handler with SYSCALL; expects PPPPPS).
