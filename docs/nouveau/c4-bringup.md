# C4 firmware and first AD106 probe

Started 2026-10-19. **C4 is in progress: AvoryOS now initializes AD106 GSP
and registers Nouveau DRM with the isolated 6.18.42 / 570.144 candidate.**
Two consecutive hardware launches passed. Explicit upstream driver removal
also completes, including GSP booter-unload with mailbox status `0 / ff`.
Missing-firmware software unwind tests now pass; physical failed-probe
resource accounting is prepared and remains required before closing C4.

**2026-10-21 reset update:** GPU-only assignment behind a dedicated root port
survived two ordinary Ascension shutdowns, active-GSP QEMU closure and a fresh
Ascension launch afterward, without a host reboot. This configuration is now
installed in `make run-nouveau`. Physical HDMI audio is not assigned;
emulated desktop audio remains. The page free-count diagnostic was subsequently
corrected and passed two strict hardware boots, as recorded below. These
results do not replace the other outstanding C4 qualifications.

Earlier paired-function evidence: the first ordinary guest power-off preserved host PCI access. After the
second test explicitly unloaded Nouveau before power-off, VFIO's exit reset
lost the downstream PCIe link again. Native Linux reproduces that unload/reset
failure too. Guest initialization/removal and host reset reliability are
separate validation results. No host device was rebound or explicitly reset
by the agent.

The active 6.12 import and ordinary Limine launcher remain in place pending
SMP and physical AMD regression validation. Strict one-vCPU readiness passes;
the eight-vCPU run retains the previously observed VKMS PMM drift. The earlier
535.113.01 failure and investigation below are retained as historical evidence.

## LinuxKPI-only investigation

Investigation covers LinuxKPI and the native kernel PCI implementation; the
existing imported-driver patches remain unchanged. No LinuxKPI defect
explaining GSP status `0x59` has been established yet.

### Removed 570 KVM metadata experiment

The explicit `hypervisorType=3 / bIsPassthru=1` experiment did not fix the
reset failure. Its metadata was confirmed in the RPC log, Nouveau initialized
and completed GSP booter-unload, but QEMU exit still lost both PCI identities.
Host boot: `1613ded9-06c6-408a-af8b-04992477e327`. The full strict guest
result also failed the existing page free-count invariant diagnostic.

At the user's request, the 6.18 patch, opt-in build/runner switches and its
dedicated regression were removed. Generated patched source, objects, kernels
and boot disks were discarded; logs and assessment remain under
`build/nouveau-618-kvm/`. The earlier 6.12 experiment is historical and was
not changed by this removal.

### Proper PCIe topology investigation

Read-only source/evidence review found an untested topology difference:
assignment directly on Q35's root bus does not model a discrete PCIe endpoint
behind a root port. Native Linux control evidence previously described the GPU
as conventional PCI. This observation alone did not establish a fix.

The same native Linux kernel and four 570.144 firmware blobs were tested behind
a dedicated root port, using OVMF and VirtIO VGA. Two consecutive hardware
launches on host boot `7872e7d7-c7ec-493a-b4fc-dc8b5db3b831` initialized
Nouveau, completed module removal and powered off normally. Both host PCI
identities remained accessible after each QEMU exit. Linux identified the GPU
as a PCIe Legacy Endpoint at `0000:01:00.0`. Evidence:
`build/nouveau-c4/linux-control-570-rootport/hardware-ovmf-{first,second}/`.
The initial SeaBIOS/no-VGA fixture never reached Linux, timed out, and left host
PCI healthy; it is inconclusive, not a Nouveau failure.

AvoryOS's PCI shim assigned every wrapper to bus zero, making `pci_dev_id()`
and BDF lookup incorrect off the root bus. The fix preserves native bus
numbers, populates per-bus device lists, and links bridge/parent relationships
after enumeration without depending on enumeration order. Cycles and failed
bridge-config reads are rejected. No device reset or driver firmware is changed.
`test_kpi_pci_bus_topology.py` covers nested/unordered bridges, bus identity,
same-slot devices on different buses, allocation failure and malformed cycles.

The diagnostic runner's `--pcie-root-port` uses guest `0000:01:00.0/1`,
reserves a 32 GiB prefetched window and requires software readiness for the
same topology. The AvoryOS hardware result follows.

AvoryOS candidate SHA256
`0dd07a6e8585c5f7250293dec842509bd864aba08ec4645ebc1d505903c0dff5`
passes strict one-vCPU root-port software readiness and the PCI topology,
capability, config-access/lifecycle, state, power and shutdown regressions.
The shared change also builds in the existing 6.12 profile. All 11,137 imported
6.18 source blobs remain unmodified. The normal ISO/launcher were not promoted.

However, its physical root-port test **still loses the link on QEMU exit**.
GSP initialization, the DRM_VERSION oracle and ordered booter-unload succeed;
the guest completes S5, then host link retraining fails and both functions
become inaccessible. The strict guest result also retains the page free-count
failure. Evidence: `build/nouveau-618/rootport-hardware-first/`. No second
AvoryOS launch was attempted. The bus fix is a real compatibility correction,
**not an established fix for the reset failure**.

A remaining control mismatch was identified before making another driver
change: the original native Linux control checks DRM node presence but never
opens it. AvoryOS runs `test_nouveau_probe.c`, which opens, issues DRM_VERSION,
and closes the card. Nouveau's open/postclose callbacks create and destroy
per-file GPU clients, so these are not equivalent workloads.

The next private control image embeds that exact oracle and toggles only
`control_open=0/1` in the same image and QEMU layout. Its guarded comparison
runs no-open, open, then open-repeat, stopping immediately if PCI health is
lost. It uses OVMF, VirtIO VGA, a private snapshot NVMe disk, root port `03.0`
and GPU `01:00.0/1`, matching the AvoryOS diagnostic layout. Full GSP RPC
payload tracing is enabled for subsequent comparison, not as a fix.
The comparison ran on fresh host boot
`958b8741-a532-4939-8e34-3bd0fff1a85c`, entirely with the installed native
Linux kernel, not AvoryOS or LinuxKPI:

- No-open initialized/unloaded Nouveau and powered off normally; both PCI
  functions remained readable. Link width nevertheless changed from x8 to x1.
  The original result file says fail solely because verbose GSP tracing
  displaced the early PCIe-description message from the kernel log ring.
  The validator now checks the separately printed enumerated PCI addresses;
  the original result is preserved with this qualification.
- Exact DRM open/version/close initialized/unloaded Nouveau, passed the oracle
  and powered off normally; both PCI functions remained readable at x1.
- On the next identical launch, host VFIO's startup reset lost the link
  (`retraining failed`, `Data Link Layer Link Active not set`). The guest saw
  `unknown header type 7f` and no GPU, before Nouveau could probe the card.
  Both host functions became unreadable. No subsequent VM was launched.

Thus two successful native guest exits do not establish reset repeatability.
The link loss reproduces without executing any AvoryOS code and can occur at
the next VM's startup reset, not only at QEMU exit. This does not prove that
DRM open is the trigger, identify which reset primitive is defective, or rule
out additional AvoryOS bugs. It does rule out treating a LinuxKPI-only change
as sufficient to resolve this native reproduction.

Evidence and the qualified baseline assessment are under
`build/nouveau-c4/linux-control-570-rootport-open/`. No host reset policy,
bindings or persistent configuration were changed. The matching AvoryOS trace
fixture under `build/nouveau-618/rootport-trace/` was not run because PCI access
was lost. The reset problem remains unresolved.

### GPU-only reset-path validation

Parallel audits identified a concrete reason that assigning unused HDMI audio
changes the reset path. Audio has no individual reset method; QEMU's reset
selection therefore requests a whole-device-set hot reset for the assigned
pair. The failed native repeat shows successful GPU-only FLR immediately
before the paired reset loses the link. Omitting audio from the guest does
not disable GPU FLR or change the host's reset policy. Both physical functions
must remain bound to VFIO in the reviewed isolated group. VFIO permits opening
one device while retaining the other group member's safe driver binding.
If function reset fails, VFIO can still require a whole-set reset; avoiding
every bus reset is a hypothesis to verify with traces, not a guarantee.

The independent guest audit also found that Ascension's native HDA fallback
claims the NVIDIA audio function in this diagnostic VM, enables bus mastering
and initializes its codec after Nouveau. HDA has no native shutdown hook and
is not owned by the LinuxKPI PCI driver list. The native Linux control never
loads an HDA driver. This is another workload difference, not proof of active
audio DMA or the complete reset root cause: the VFIO-only guest also initializes
HDA and survives. No speculative HDA/GSP kernel change was added to this test.

`scripts/nouveau-boot.py --gpu-only` now omits only the guest audio function;
default paired-function behavior and all host group/ownership checks remain
unchanged. Software readiness records distinguish these configurations.
The candidate kernel and four firmware blobs are unchanged. Focused runner
regressions and strict one-vCPU GPU-only software readiness pass. Hardware
validation on fresh host boot `11111e22-4b8b-4441-b53c-a972fd644fe1` ran
Ascension first, without priming the device with native Linux:

1. `rootport-gpu-only-first`: GSP init, exact DRM oracle, full Nouveau removal
   and ACPI S5 succeeded; both host functions remained readable.
2. `rootport-gpu-only-second`: the same complete lifecycle succeeded again
   without rebooting the host; both functions remained readable.
3. `rootport-gpu-only-abrupt`: a private oracle wrapper held after the exact
   DRM open/version/close test. QMP confirmed the VM running with GSP active;
   QMP quit exited normally without any guest Nouveau removal or S5 request.
   Both physical functions remained readable. QEMU's GTK window-close handler
   directly calls the same `qmp_quit()` path. This is not a kill-9 test.
4. `rootport-gpu-only-after-close`: assignment, GSP initialization, the DRM
   oracle, verified booter-unload and S5 all succeeded after that active-GSP
   close; both functions remained readable.

All four cases opened only display function `.0`, retained normal GPU FLR,
and produced no QEMU hot-reset request or host link-retraining failure.
Physical link width stayed x8. Runtime/reset policy and bindings were unchanged.
The three ordinary runs retain strict failure for the known LinuxKPI page
free-count invariant. The abrupt test observed that failure too. Reset reuse
passes separately; do not relabel those strict results or close all of C4.

`make run-nouveau` now uses the tested PCIe GPU-only layout with guest
`01:00.0`, retaining emulated Intel HDA, networking, keyboard/mouse and the
installed writable desktop disk. Its matching normal Limine ISO was rebuilt
from the exact tested kernel and verified by extracting its kernel/command
line. An exact command-line stamp prevents reuse of an old-BDF ISO. The previous
ISO is retained under `build/nouveau-618/pre-gpu-only/`; the installed disk was
not rewritten. The legacy AMD and earlier typo-named C4 targets are unchanged.
The full installed desktop session and rendering stress remain user validation,
not claims from the isolated probe fixture.

Evidence and assessment are under `build/nouveau-618/rootport-gpu-only-*` and
`build/nouveau-618/gpu-only-assessment.json`. The retained execution plan is
`build/nouveau-618/gpu-only-test-plan.json`.

Source references:
[QEMU reset selection](https://raw.githubusercontent.com/qemu/qemu/v11.1.0/hw/vfio/pci.c),
[VFIO reset ownership](https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/vfio/pci/vfio_pci_core.c),
[VFIO group/device API](https://raw.githubusercontent.com/torvalds/linux/v6.18/Documentation/driver-api/vfio.rst).

### Page free-count diagnostic correction

The previous invariant sampled global free memory before 256 allocation/free
cycles. `alloc_pages()` can materialize permanent shared `struct page` storage
and page-table backing on first use. Returning the data block does not and must
not free that metadata. Consequently, a lower final global count did not by
itself demonstrate leaked data pages. Background allocation on the boot-test
CPU was also outside the old test's ownership boundary.

The test now allocates each pair first, then requires the free operation to
return exactly `(1 << order) + 2` physical pages. It retains all 256 iterations
and uses no tolerance. IRQ state is saved only around the short measurement
and non-sleeping free sequence, restored before reporting, and never disabled
around `GFP_KERNEL` allocations. Either successful allocation is released if
its partner fails. Local IRQ isolation does not make a global PMM snapshot
concurrency-safe across other CPUs; validation remains the qualified one-CPU
configuration. The production page allocator and upstream Nouveau are unchanged.

`scripts/tests/test_kpi_page_free_invariant.py` reproduces the old false failure
with retained lazy metadata and extracts the corrected kernel test. It checks
all 512 allocation-failure positions and all 512 under/over-free positions,
requiring real miscounts to fail and partial allocations to be cleaned up.
The existing descriptor arena/physical-bound regressions pass as well.

Candidate SHA256
`5ff5b4a3d877364ef5ddb0ca38438962ae2f6e8cad516d77965cd8dfd7fe2f85`
passes strict software readiness and two consecutive strict physical GPU-only
boots, including GSP initialization, the DRM oracle, booter-unload and S5.
Both logs report `page free-count invariant correct`; both host PCI functions
remain accessible afterward. Evidence is under
`build/nouveau-618/page-invariant-{readiness,hardware,hardware-repeat}/`.
The normal C4 ISO was rebuilt and its extracted kernel verified against this
candidate. The previous ISO is retained under `pre-page-invariant/`, and the
installed desktop disk was not rewritten. Assessment:
`build/nouveau-618/page-invariant-assessment.json`.

### PCI capability and ECAM audit

After the failed host reset-policy experiment, the user requested removal of
that helper and an audit of LinuxKPI/native PCI. The helper and its test are
removed. The audit found and corrected these independently reproducible bugs:

- Native `asc_pci_find_capability()` could loop indefinitely on a cyclic list
  or inaccessible config space. It now bounds traversal, aligns capability
  pointers, rejects the `ff` absent-device response, handles CardBus's list
  pointer and accepts a null device safely.
- LinuxKPI `pci_find_next_capability()` returned the supplied capability again
  instead of following its next pointer. It now advances before matching and
  bounds malformed lists.
- LinuxKPI's extended-capability walker decoded the next pointer from bits
  16..27 instead of 20..31 and could return its starting entry again. It now
  decodes and aligns the pointer correctly, skips the starting match, rejects
  short config spaces, and uses the extended list's traversal bound.
- Native ECAM translation incorrectly subtracted MCFG's start bus from the
  physical address. MCFG bases correspond to bus zero; split/nonzero-start
  allocations now use the absolute bus offset. Availability and translation
  also now agree about segment zero, reject truncated tables, and reject
  misaligned or cross-function word/dword accesses.

The new `test_pci_capability_walks.py` and `test_pcie_ecam.py` regressions run
the production function bodies with simulated PCI configuration. Both failed
before their fixes and pass afterwards; the old native walker independently
fails the bounded cyclic-list check. Existing PCI config transaction,
enable/disable lifecycle, power-state, state-restore and shutdown checks pass.
The shutdown harnesses were updated to supply the PCI identity accessors used
by the existing pre-handoff diagnostic. This repairs their stale fixtures;
it does not change production shutdown behavior.
The broader `test_pci_driver_identity.py` check still fails to compile because
its fixture lacks the pre-existing Nouveau probe diagnostics and fields; that
unrelated harness was left unchanged.

The isolated 6.18 candidate builds with SHA256
`52c6b651c229437e790c9ca4385cc81987b0afa6dc4301d5ae81a84692c449fb`.
Its one-vCPU software readiness VM passes the strict LinuxKPI checks, all four
570.144 firmware CRC/DMA checks, the process oracle and ACPI power-off.
Evidence is under `build/nouveau-618/pci-audit-readiness/`. The standard
`make` path now imports and builds this kernel directly into the main ISO.

The user rebooted for a hardware retest on host boot
`b7593727-e2f7-4680-9305-3f9a7b275f66`. Both reviewed NVIDIA functions were
accessible, VFIO-bound and in D0, with the original `flr bus` reset selection
and no host policy changes. The exact qualified candidate initialized GSP,
returned probe result 0 and registered Nouveau DRM 1.4.0 on card2. Orderly
power-off completed the verified GSP removal sequence, booter unload returned
`0 / ff`, and the guest PCI handoff retained identity `280310de` and pin 1.
NVMe shutdown and ACPI S5 then completed and QEMU exited.

The host's exit reset nevertheless failed downstream link retraining and
DLLLA recovery; both NVIDIA config identities read `ffffffff`. No second
launch was attempted. Thus these PCI fixes do not prevent the observed
repeat-boot failure. The strict hardware result also remains **fail** because
of the page free-count invariant and a one-page VKMS warm-loop PMM drift.
Evidence and separate guest/host result attribution are under
`build/nouveau-618/pci-audit-hardware-first/`. No host reset-policy writes,
standalone host resets or driver rebindings were performed.

These fixes do **not** establish the cause of the AD106 exit/reset failure.
The saved C4 runs use a bus-zero MCFG window, so the split-window address
correction does not change that configuration. GSP unload followed by host
link loss remains a distinct hardware validation requirement. No host reset,
driver rebind or reset-policy change is part of these fixes.

The opt-in firmware suite now also copies each verified NVIDIA blob into a
page-aligned VM buffer, maps its scatterlist through the real guest DMA backend,
and checks every DMA page against the CPU physical alias. It checks coherent
buffers of 4,096, 65,536 and 528,384 bytes for zero initialization, alignment and
physical contents, freeing each allocation afterwards. This exercises the
guest VM/page allocator and DMA implementation without accessing NVIDIA hardware.

The eight-CPU KVM run under `build/nouveau-c4/kpi-dma-readiness/` passed all four
firmware CRCs, all DMA page walks (including 9,293 pages for the 38,061,600-byte
GSP image), and all three coherent buffers. Its candidate SHA256 is
`79300405444f111020ae39d5e794811ecd16d9eedc507e0702abf9dd3e91b3e1`.
The full strict readiness result is **fail**: the existing VKMS GEM warm-loop
test reported 30 pages of PMM drift. The userspace oracle exited zero and the
guest powered down normally; neither fact overrides that failure.

These results reduce suspicion of firmware-copy and identity-DMA address bugs.
They do not establish that hardware DMA, MMIO, GSP initialization or failed-init
cleanup works. After the user's guest shutdown, both NVIDIA functions again
returned invalid PCI configuration despite reporting D0; hardware retesting
requires restoring the host PCIe link first. The normal Limine launch targets
and their serial-log destination remain unchanged.

### Physical retest after host power cycle

The next controlled test used the normal `make run-nouveau` Limine/installed
desktop path with a one-run command-line override `nouveau.debug=info,gsp=debug`
instead of `gsp=trace`. No driver code was changed. Both NVIDIA functions had
valid PCI configuration before launch. GSP again returned `res:0x59 resp:0x59`;
failed-init cleanup again logged `reset=0 fwsec=0 unload=-110`. Lower trace
volume therefore did not resolve this failure.

Both functions still had valid vendor/device bytes and interrupt pins 1/2
while QEMU remained running after the failed probe. After the user's orderly
`shutdown -h now` and QEMU exit, both read invalid configuration with pin 255.
The host journal records VFIO resetting both functions at exit, followed by
the upstream bridge reporting a non-functional downstream link, failed
retraining, and missing Data Link Layer Link Active. This locates the observed
PCI link loss at VM teardown; it does not establish its underlying cause.

`vfio_pci.disable_idle_d3` was already `Y`. The bridge and both functions had
`power/control=auto` and `d3cold_allowed=1` after reboot, so the previous manual
power policy was not persistent. No host power-policy or reset writes were
performed during this retest. Evidence, the exact normal kernel hash, and the
separate serial excerpt are under `build/nouveau-c4/kpi-low-log/`.

### LinuxKPI PCI disable correction

`pci_disable_device()` incorrectly cleared PCI command MEMORY and IO decoding
as well as bus mastering. Linux v6.12's `do_pci_disable_device()` only clears
MASTER; the x86 architecture hook disables IRQ routing, not BAR decoding:
<https://github.com/torvalds/linux/blob/v6.12/drivers/pci/pci.c#L2095-L2138>.
Nouveau's failed-probe `fail_pci` label disables before calling
`nouveau_drm_device_del()` and `nvkm_device_del()`, whose destructors can still
access device registers. The shim therefore cut off MMIO too early.

LinuxKPI now preserves decode bits and clears `is_busmaster` at the final
disable. Nested enables and redundant-disable protection are preserved. The
PCI lifecycle regression models command-register state and proves decode
survives while bus mastering stops; it fails against the original shim and
passes with the fix under ASan/UBSan/LSan. PCI state/power and the C3 helper
regressions also pass. No imported driver or driver patch was modified.

The candidate kernel and normal Limine ISO have been rebuilt; hashes and
negative regression evidence are under `build/nouveau-c4/pci-disable-fix/`.
An eight-CPU software VM again passes the four firmware DMA walks and coherent
buffer checks and exits normally, but strict readiness still fails on the
existing VKMS GEM PMM drift (54 pages in this run). Hardware behavior remains
unverified because the previous VM exit left both NVIDIA functions inaccessible.
This correction occurs after GSP's init failure and does **not** establish a
fix for `0x59` or the earlier booter-unload timeout.

The older driver's diagnostic log dump also interprets the LibOS put count as
bytes; NVIDIA's public `liblogdecode.c` shows it counts `NvU64` entries modulo
`logBufferSize / sizeof(NvU64) - 1`. Consequently that existing dump omits most
records. It was left unchanged to honor the LinuxKPI-only constraint; no
firmware-specific length adjustment was hidden in generic hex-dump helpers.

The physical retest on 2026-10-20 confirms the correction is active: after the
failed Nouveau probe, a QMP physical read of guest ECAM command register
`0xe0018004` returned `0x0003` (IO/MEM decoding enabled, bus mastering disabled).
The normal kernel hash matches the rebuilt image recorded above. GSP still
returned `0x59`, and the init-abort unload still timed out at `-110`. Both host
PCI identities remained readable while the VM ran.

AvoryOS's native `shutdown` command does not accept the Linux-style `now`
argument. Use **`shutdown -P`** to power off; `shutdown -h` intentionally halts
without powering off. In this run the user selected halt, the serial log
confirmed `NVME-SHUTDOWN: nvme0 SHST=complete`, and QMP confirmed CPU0 HLT=1.
The agent then closed the halted VM through QMP. Host VFIO's exit reset again
logged a non-functional link and failed retraining, leaving both NVIDIA
functions with invalid PCI configuration. This is a halt-plus-QEMU-exit test,
not a guest ACPI power-off test. Preserving PCI decoding did not resolve GSP
initialization or exit reset recovery. Full evidence is under
`build/nouveau-c4/pci-disable-hardware/`.

### Linux control comparison prepared

Further blind shim changes are paused pending a Linux control comparison of
the same physical card and pinned 535.113.01 firmware. This is diagnostic work;
no additional LinuxKPI or imported-driver fix is claimed.

`python3 scripts/nouveau-linux-control.py` prepares a private initramfs under
`build/nouveau-c4/linux-control/`, refusing to overwrite existing evidence.
It copies the installed kernel and its Nouveau dependency modules, verifies
each pinned firmware SHA256, and builds a static PID 1. It does not load host
modules, launch a VM, bind/reset devices, open `disk.img`, or change the normal
AvoryOS Limine targets. Hashes of the kernel, initramfs, modules and firmware
are recorded in `inputs.json`.

In the control guest, PID 1 loads those modules, checks the NVIDIA device at
guest `00:03.0` for a Nouveau binding and DRM card, attempts Nouveau module
removal and powers off. A guest-only `control_software=1` boot permits absence
of the physical card to validate the fixture; a hardware run must omit that
option. The control currently uses installed Linux `6.18.42-1-cachyos-lts`, not
the imported v6.12 baseline. A successful result establishes a useful known
working firmware/VFIO path but does not alone prove a specific LinuxKPI defect.

Both software preparation boots passed: one using QEMU's default firmware and
one using the existing AvoryOS OVMF image, q35, eight CPUs and the VirtIO VGA
device at `06.0`. All twelve guest modules loaded and the guest powered off
normally. No host GPU was assigned. Serial logs and `result.json` are in the
control directory. Hardware comparison has not run: fresh preflight still
reports unreadable NVIDIA PCI configuration after the previous VM exit, so a
host power cycle is required before assigning the device again.

The first physical Linux control attempt on 2026-10-20 is **inconclusive**.
Preflight read valid identities/pins and both VFIO bindings, and the staged
kernel/initramfs hashes matched `inputs.json`. QEMU then reported both
`Unable to power on device, stuck in D3` and a zero-size option ROM. Linux's
PID 1 loaded the twelve modules but found no device at the reviewed guest
slot and emitted `END status=fail`; it did not reach a Nouveau GSP probe.
The control VM powered off and exited. The host journal shows failed link
retraining during assignment resets, invalid ROM signature `0xffff`, and
pending-transaction timeouts; both NVIDIA functions were inaccessible after
exit. Thus a valid preflight configuration read alone does not establish
that the GPU will survive VFIO's assignment reset. This attempt cannot
establish whether native Linux would succeed where LinuxKPI fails.
Evidence is in `hardware-{serial,host,kernel,kernel-full}.log`,
`hardware-{before,after}.json`, and `result.json` in the control directory.

### Host power-policy observations

`scripts/nouveau-host-power.py --status` now collects read-only evidence for
the reviewed bridge `0000:00:01.1` and NVIDIA functions `0000:01:00.0/1`:
boot ID/uptime, live PCI identity, D0/runtime state, current link information,
reset-method ordering, `power/control`, `d3cold_allowed`, and the VFIO idle-D3
parameter. It reuses the existing exact-device, IOMMU-group, host-iGPU and VM
ownership guards and additionally requires both functions to be directly below
the reviewed bridge with no other descendants. D0 and valid config alone are
explicitly not treated as proof of a usable reset.

The helper's `--status` mode is read-only. Its optional `--apply` mode writes
only `power/control=on` and `d3cold_allowed=0` on the dedicated bridge and the
two NVIDIA functions, after validating PCI identity, D0, VFIO ownership and
topology. These are host runtime-power/D3cold policy controls:
[power/control](https://github.com/torvalds/linux/blob/v6.12/Documentation/ABI/testing/sysfs-devices-power),
[d3cold_allowed](https://github.com/torvalds/linux/blob/v6.12/Documentation/ABI/testing/sysfs-bus-pci).
They may prevent automatic host power transitions, but the policy is not a
`run-nouveau` prerequisite and is not a recovery procedure. The pinned-policy
control test still lost PCI config access after QEMU exit, so routine C4 runs
must not require `sudo ... --apply`. The helper remains available for a
specifically planned power-policy experiment; it never resets or rebinds the
GPU. Original values are saved under `/run` for same-boot restoration.

`scripts/tests/test_nouveau_host_power.py` passes against temporary sysfs/proc
trees: exact topology/ownership/identity guards, read-only status, six allowed
attribute writes, apply/restore, injected partial-write rollback, and stale or
tampered state/symlink refusal. Existing preflight and interactive-target
regressions pass. The live read-only snapshot is
`build/nouveau-c4/host-power-before.json`: NVIDIA config still reads all `ff`
while sysfs reports D0. No host power-policy write has been performed.

### Native Linux reproduces the 535 GSP failure

The user then cold-booted and applied the helper. Fresh evidence records boot
ID `01d30518-b8c5-4385-80bb-4392b91b0f6e`, valid live identities, active D0,
16.0 GT/s x8 link, `power/control=on` and `d3cold_allowed=0` on the bridge and
both functions, with VFIO idle D3 disabled. The native Linux control was rerun
under this verified policy. Its PID 1 now records the actual kernel release,
PCI inventory and the kernel ring buffer, even if console verbosity suppresses
the initial kernel output.

Linux `6.18.42-1-cachyos-lts` enumerated `0000:00:03.0`, tried its preferred
570.144 image (absent in this deliberately 535-only fixture), and loaded all
four 535.113.01 blobs with the same sizes used by AvoryOS. FWSEC and booter-load
succeeded, and GSP executed a `00003fe2/00000628` sequencer. It then returned
`fn:4097 ... res:0x59 resp:0x59`; Nouveau failed with `-22` and remained unbound.
The control guest powered off normally. Both NVIDIA PCI functions became
inaccessible again after QEMU exit even though the power policy remained
pinned. Evidence is under `build/nouveau-c4/linux-control-pinned/`.

This establishes that the observed 535 init failure and exit reset problem
can occur without LinuxKPI. It does not prove LinuxKPI has no remaining
defects, or that the two generic `0x59` reports have precisely the same cause.
Further compatibility edits should be driven by a working control baseline.

The next comparison image was prepared with:

```sh
python3 scripts/nouveau-linux-control.py --firmware-version 570.144 \
    --output-dir build/nouveau-c4/linux-control-570-ready
```

This mode checks that the installed Nouveau module declares each of the four
AD106 570.144 firmware paths, then copies/decompresses the installed files into
the private image and records full sizes/SHA256s and resolved source paths.
No 535 firmware is present in this image, so it cannot silently fall back.
Its OVMF software-only boot passed. The original C0 bundle and its hashes are
unchanged. AvoryOS's imported AD10x GSP implementation supports 535.113.01
only; 570 was **not** substituted into AvoryOS or mixed with the older RM
interface. Its subsequent native hardware run passed; see the coherent
6.18 upgrade section below for the result and remaining VFIO teardown issue.

## Implemented preparation

- Firmware loader accepts files up to 64 MiB, using VM-backed CPU buffers above
  1 MiB and `kvfree` on release/error. Reads are bounded to 1 MiB and handle partial
  reads; premature EOF fails with `-EIO`. A 64-bit native VFS size query rejects
  files above the bound before narrowing their length, including files >4 GiB.
- Host sanitizer regression runs the actual loader: full 38,061,600-byte GSP
  contents/release, partial reads, truncation, empty/missing files, oversize,
  allocation failures, path rejection and resource cleanup.
- `scripts/nouveau-firmware-stage.py` materializes the four AD106 aliases from
  the pinned C0 lock. Every size/SHA256 must match; the NVIDIA redistribution
  notice is included. A bad input leaves the previous complete bundle intact.
  AMD's manifest/hash enforcement is unchanged. Output is separate under
  `build/nouveau/firmware`, with a generated CRC manifest.
- `kpi_nouveau_fw_test=1` opts the private C4 fixture into guest verification of
  all four blobs through the real loader. Normal AMD images do not acquire a
  requirement to ship NVIDIA firmware merely because the manifest is staged.
- Nouveau thermal shutdown now reaches native emergency guest ACPI power-off;
  emergency restart reaches native reboot. There is no userspace shutdown
  helper. Emergency paths skip GPU callback teardown to avoid a thermal worker
  waiting for itself. These are not an orderly filesystem/driver shutdown.
  Actual thermal protection on the physical GPU remains untested.
- The launcher requires host CLFLUSH support and exposes the host CPU to KVM.
  The target uses the real CLFLUSH path; the inherited local-only all-CPU
  WBINVD fallback is not presented as implemented.
- PCI diagnostics record the selected GPU, BARs, IRQ and probe return. Upstream
  GSP trace output records RPC functions/sequence activity. Firmware loads log
  exact names and sizes. Serial/QEMU logs persist through errors and timeouts.

## Dedicated launcher

`scripts/nouveau-boot.py` defaults to preparation. It uses a copy of the boot
ISO and a disposable snapshot of a private root disk; it never opens `disk.img`,
changes host bindings, invokes a host reset helper or uses the AMD VFCT ROM.

Hardware use requires the exact reviewed `10de:2803` display function and
`10de:22bd` audio function, both already bound to VFIO, reviewed group members,
no visible existing VM/group owner, and no NVIDIA host DRM node. It requires
explicit confirmation that the host display has moved to the iGPU. Both PCI
functions are assigned at guest `00:03.0/1`; only the display function is allowed
through the exact-BDF Nouveau probe gate. Optional explicit ROMs must have a
matching NVIDIA PCIR identity. There is no default ROM file.

A diagnostic hardware launch additionally requires a **passing strict software readiness
result for the same kernel hash, firmware manifest and CPU count**. A software
oracle exit of zero alone does not qualify. The guest must log all four firmware
CRC successes. The process oracle on hardware must discover a real Nouveau DRM
card and report its driver/version; it does not submit rendering work.

Current entry point:

```sh
make
make run-nouveau
```

The standard kernel import and build now use the qualified 6.18.42 source;
`make` builds the main ISO and installs the verified 570.144 firmware. No
separate preparation target is required:

```sh
make
```

Then `make run-nouveau` uses `avoryos-x86_64.iso` and checks the prepared files, PCI identities, VFIO
bindings and iGPU primary display. It launches QEMU
directly through the regular Limine path with one vCPU, the NVIDIA pair at
guest `00:03.0/1`, USB keyboard and relative mouse, audio, networking and
VirtIO output. The run target invokes no Python scripts, diagnostic init or
automatic oracle shutdown. Serial appends to `build/logs/nouveau-c4.log`.

The main kernel uses the 6.18.42 import. The Nouveau Limine entry uses
`init=/sbin/init`, enables only Nouveau's reviewed guest BDF gate, requests GSP
modesetting, disables runtime PM, and enables native LinuxKPI removal on
orderly shutdown.

The former phase-specific and candidate Make targets have been retired. Use
`make run-nouveau` for the verified 6.18 desktop boot.

The builder writes `kernel-c4`, `build/nouveau-c4/kernel` and its separate
`kernel/obj-nouveau-c4-x86_64/` object/profile/dependency tree. A relocated Kbuild
fragment preserves target-specific includes/FPU flags. It holds a shared import
lock, checks the selected Linux pin and links both drivers. Hardware commands keep AMD probing
off. C4 does not use `make run-c7-prime`, which assigns Raphael by default.

## Interactive boot and failed-init follow-up

The 23:17 physical log ran the address-limit correction and still received
GSP_INIT_DONE status `0x59`. No D3/pin failure occurred on that attempt; GSP
logs were captured. Therefore TASK_SIZE is not presented as the root-cause fix.

Two subsequent source changes are now built into the standard kernel/ISO:

- `nouveau.gsp_kvm_passthrough=1` explicitly sets GspSystemInfo's
  `hypervisorType=3` (OS_HYPERVISOR_KVM in the matching NVIDIA enum) and
  `bIsPassthru=true`. The matching OpenRM `rpcSetSystemInfo_v17_00` supplies
  these fields for assigned GPUs; upstream r535 Nouveau leaves them zero.
  The option defaults off and is enabled by this host's launch target.
  This is an evidence-based candidate fix, not a verified explanation of `0x59`.
- After successful booter load followed by INIT_DONE failure, stop/reset GSP,
  run FWSEC-SB and booter unload before the destructor releases DMA buffers.
  Previously `running=false` caused ordinary fini to return without this
  teardown. The abort path preserves logs and the original failure code, does
  not send the initialized-RM unload RPC, and logs each cleanup result. Hardware
  confirmation is required before claiming it fixes repeated boots.

Tracked v6.12 patches `0005` and `0006` reproduce both changes. Host tests
execute the actual system-info and init/unwind bodies, including default-off,
64-bit BARs, allocation/booter/RPC failures, cleanup failures, success, postinit
and resume paths. Firmware installation is tested on a real GPT/ext4 fixture
with an existing file, byte verification, cached rerun and bad-bundle rejection.
Dry runs of both user launch aliases confirm plain QEMU and the normal desktop
boot path. The normal kernel and ISO builds succeed.

An intermediate software-only boot completes the oracle, firmware CRCs and
shutdown but fails the existing VKMS GEM accounting test (48 pages); it is
recorded as a strict failure. The subsequent physical diagnostic sees VFIO D3
and zero-ROM errors, then boots without a usable NVIDIA function. It never
executes the new GSP payload. No repeated retry/reset/rebind was performed.
Evidence is under `build/nouveau-c4/kvm-info-{readiness,hardware}/`.

Current standard kernel SHA256:
`152ae2ee38035bb91d90e7218e8a3eb3447aaab6ce31be2fbf0e15ed89964e49`.
ISO SHA256:
`55c73e2b0de0457ddf1182fbe17c15044c3b26d63fdb7f886a195e5a9e9d9ee2`.
A full host power-off/power-on is needed to test this build on an accessible
GPU. The interactive command remains `make run-nouveau`.

References: matching NVIDIA [system-info producer](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/535.113.01/src/nvidia/kernel/vgpu/nv/rpc.c),
[hypervisor enum](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/535.113.01/src/common/sdk/nvidia/inc/nv-hypervisor.h),
and [GPU passthrough detection](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/535.113.01/src/nvidia/src/kernel/gpu/gpu.c).
Encoded firmware log decoding needs unavailable NVIDIA data, as described in
the [Nouveau GSP logging documentation](https://nouveau.freedesktop.org/Bugs.html#getting-gsp-rm-logs).

## Observed hardware failures (2026-10-19)

The user completed the host handoff: NVIDIA `0000:01:00.0/1` are bound to
`vfio-pci`, isolated in group 12. Raphael `0000:0e:00.0` drives the host through
`amdgpu`, and NVIDIA has `boot_vga=0`. This host uses Limine; rebuilding its
initramfs uses `sudo limine-mkinitcpio`. The user's module override loads VFIO
before AMDGPU and removes the earlier forced NVIDIA module list.

The first supplied serial log discovers AD106, reads VBIOS 95.06.34.00.63,
loads all four pinned firmware files, exchanges RPCs, executes the CPU
sequencer, and receives GSP_INIT_DONE (4097) with status `0x59`. Nouveau maps
that response to `-EINVAL`; no Nouveau DRM card is registered. The generic RM
status alone does not identify the underlying failure.

Subsequent QEMU launches can fail before guest boot with `invalid PCI interrupt
pin 255`. A bounded diagnostic probe also logged `Unable to power on device,
stuck in D3` and a zero-sized option ROM. That attempt eventually booted AvoryOS
without a usable NVIDIA function, so it cannot validate GSP changes. It also
failed the existing strict VKMS accounting test; this failure remains recorded.
The VFIO dma-buf fallback warning is separate from these errors.

Read-only preflight can see healthy configuration immediately before QEMU
starts (pins 1/2, D0/active) despite the subsequent VFIO startup failure. It now
checks the first 64 configuration bytes, identity and interrupt pins, and records
power/reset state. This catches already inaccessible configuration; it cannot
prove a subsequent VFIO reset will succeed. No automatic retry, host reset or
rebind is performed. Stop repeated launches after these errors and fully power
off the host before the next physical test.

The current candidate corrects the LinuxKPI four-level x86 TASK_SIZE value to
`(1UL << 47) - PAGE_SIZE`, matching the imported Linux contract rather than
passing the noncanonical boundary to GSP's maxUserVa. This is a contract fix,
**not yet a verified fix for the GSP failure**. The v6.12-specific tracked patch
adds bounded GSP log-buffer tails on initialization failure. Keeping this patch
under `scripts/linux/patches/v6.12/` preserves the retained v6.6 import, which
predates r535.c. The importer applies common patches followed by selected-series
patches; unrelated series are skipped.

Candidate SHA256:
`77d1c868fd787d2a237253d7628682b7ec437b3c296f6fec0cd68e3a8b531483`.
Strict software readiness passed with all four CRCs and normal shutdown:
`build/nouveau-c4/readiness-handoff-1cpu/result.json`. The physical diagnostic
failure is retained under `build/nouveau-c4/hardware-diagnostics/`.

The launcher now prints QEMU stderr on diagnostic failure (including timeout),
and gives cold-boot guidance for inaccessible PCI/D3 errors. Before reusing a
fixture, it copies prior inputs, result and serial/host logs into its `history/`
subdirectory; large disk and ISO fixtures are not duplicated. The public serial
history stays at `build/logs/nouveau-c4.log`.

After a full host power-off and power-on, with the monitor still on the iGPU:

```sh
make run-nouveau
```

This consumes the newly qualified candidate. A successful physical GSP probe,
its failure cleanup and repeated guest boots still need verification. Do not
mark C4 complete from software readiness alone.

## Validation and open work

Host tests cover exact identities/functions, group isolation, driver/DRM/visible
VM ownership, CLFLUSH and rejection of AMD ROMs. The shared bounded runner is
tested to preserve the exact two NVIDIA functions, snapshot disk and absence of
an inherited AMD ROM. Firmware staging/loader failures are exercised separately.

Initial eight-CPU and one-CPU gate-off guests loaded and CRC-verified every
NVIDIA blob, completed the oracle and shut down. Their strict results failed
on the previously observed VKMS accounting drift; they are not qualifying
readiness passes. The eight-CPU run also reported the native PCP drain test.

The GEM measurement sleeps 200 ms before sampling global free pages. RCU
callback draining was investigated at the sample boundaries and per iteration,
because radix-node reclamation is deferred and slab caches retain peak pages.
Neither experiment removed the drift, so both experimental test changes were
reverted. The original check remains intact and the root cause is unresolved.
An unexplained normal-kernel update also motivated isolating all C4 objects;
the user confirmed no other build was running. No concurrent build is assumed.

A build-output mistake in the first C4 invocation wrote the normal kernel
filename. The unchanged ISO's known boot kernel was extracted and restored to
`kernel/bin-x86_64/kernel` (SHA256
`50b817f1e4a4abe55508013f867c6872f44a9738ac5c50ddbc49073833794984` at that point).
The ISO was untouched. The corrected builder replaces the existing OUTPUT
assignment instead of prepending an assignment that gets overridden.

Earlier pre-handoff isolated candidate (`28590f13a439c8bf96a4b4d03fc6421a48e31912c6aa05d22722d821170dbaa3`)
passes strict one-CPU readiness with the original VKMS test unchanged, all four
NVIDIA firmware CRCs, closed-gate Nouveau registration and normal guest shutdown.
The same kernel also passes a strict eight-CPU boot using the ordinary AMD
firmware fixture without NVIDIA firmware-test opt-in. The earlier intermittent
accounting issue is not declared fixed by these passes. Host tests pass and the
normal kernel/ISO hash check passes for the isolated build. Machine-readable
results are in `build/nouveau-c4/evidence.json`.

Remaining C4 work: resolve GSP initialization, verify DRM registration,
probe-failure leak/IRQ checks, bounded shutdown and reset/cold-boot characterization. C4 is complete only with hardware evidence
for its original exit criteria; staging/build success is not GSP bring-up.


### Native 570 result and coherent 6.18 upgrade

The immutable native control in `build/nouveau-c4/linux-control-570-ready/`
passed hardware initialization with Linux 6.18.42 and firmware 570.144. The
NVIDIA function bound to Nouveau and exposed card1 and renderD128. Module
removal completed; booter-unload booted and returned mailboxes 0 / ff, then
the guest powered off. `result.json` records pass for the guest and
`host_after_healthy: false`: both host NVIDIA PCI identities became invalid
after QEMU exited. Initialization and the host VFIO reset failure are separate
observations. No host resets or rebindings were performed by the agent.

The old pinned 535.113.01 bundle remains unchanged. The 570 bundle is private,
with the exact native control SHA256/size records and generated CRC32 values.
The standard importer pins unmodified v6.18.42 sources at commit
`856a9b51680cbbed1ace1207b29e424a616cff48` in `kernel/linux`. It applies the
version-specific source relocations and generated-header/config setup needed
for the coherent DRM, Nouveau and LinuxKPI build. The ordinary `make` target
compiles this as the main kernel and embeds it in the normal ISO. The old
isolated candidate builder is removed. The first import requires network
access; later builds reuse the pinned tree.

The candidate firmware is staged from the normal pinned
`build/firmware/lib/firmware` bundle (running the standard firmware installer
if files are missing). All four AD106 570.144 files and the NVIDIA license are
checked against the checked-in SHA256 list before publication; the obsolete
`build/nouveau-r570` assessment output is not required. Preparation preserves
the existing desktop disk and installs only the verified firmware closure.
The launch target continues to reject a changed or unstamped ISO: rerun
preparation rather than bypassing its checksum check.

The compatibility work includes per-CPU cache-hot annotations and native
pointer qualifiers; the 6.18 descriptor flag wrapper without changing native
page size; atomic rate-limit counters; timer setup/renamed deletion helpers;
file_ref lifetime and terminal/saturation semantics; explicit maple-tree slab
reservations; const binary sysfs attributes; managed I2C actions; and native
faux-device lifecycle. Existing scheduler/GPUVA/TTM/bochs tests are adapted to
the newer public APIs while retaining their behavioral checks.

Upstream still contains older chip-family paths, including TU106, TU117,
GA106 and GP107. Presence in the import is not proof that any particular card
works in AvoryOS. Firmware packaging, build configuration, desktop rendering,
and physical AMD/NVIDIA testing remain validation requirements. The current
private bundle intentionally contains only the four AD106 570 files.

Promotion requires a clean link, gate-off boot/firmware/DMA tests, review of
strict boot failures, AMD regression testing, and AD106 initialization and
cleanup testing. A successful native Linux control is not AvoryOS GSP success.

### Orderly shutdown cleanup

The 6.18 C4 command line enables `kpi_nouveau_remove_on_shutdown=1`. On an
orderly guest reboot, power-off or halt, native LinuxKPI unregisters the
selected Nouveau PCI driver, invoking its normal remove path before native
power-off. That path drains DRM/NVKM state and reaches the GSP fini sequence
(guest-driver unload RPC, Falcon stop/reset, FWSEC-SB and booter unload) before
releasing device resources. The guest logs a `Nouveau shutdown PCI pre-handoff`
check with PCI identity and interrupt pin before QEMU returns the device to
host VFIO. A `WARN` there means guest-visible PCI config was already lost or
became inaccessible during cleanup. Forced QEMU termination cannot run guest
cleanup and is not an equivalent shutdown test.

Upstream Nouveau supplies a PCI remove callback but no shutdown callback;
system power-off does not ordinarily unregister the driver. A comparison
therefore omitted `--remove-on-shutdown` on fresh host boot
`78769a2d-6035-4b20-80d5-db3dfba57d45`, using the exact qualified PCI-audit kernel
and firmware. GSP initialization and the Nouveau DRM oracle passed. The guest
powered off without a Nouveau removal or GSP booter-unload sequence, but the
host still lost the PCIe link during VFIO exit reset and both NVIDIA identities
became `ffffffff`. A second launch was not attempted. The strict hardware
result also fails the LinuxKPI page free-count invariant. Evidence is under
`build/nouveau-618/upstream-shutdown-first/`.

Forced removal is therefore not necessary for the failure. The normal C4
cleanup setting was restored; disabling removal is not offered as a fix.
The next baseline should first open/close a paused VFIO VM without executing
guest code, then, if host PCI access survives, boot with the same pair assigned
but Nouveau probing disabled. `nouveau-boot.py --vfio-only` prepares the latter:
it retains exact-device/ownership/readiness checks, sets `kpi_nouveau=0`, and
uses an oracle that rejects Nouveau DRM registration. Its result is separately
labelled `nouveau-vfio-only`; success cannot count as GSP initialization.
No host reset settings or vendor-specific MMIO reset sequence are introduced.

#### Same-boot isolation and handoff boundary

On fresh host boot `ab7f1202-f5e4-4d78-b0fc-c22553688f4b`, three sequential
launches used the same reviewed NVIDIA pair and unchanged host reset/power
settings:

1. QEMU started with `-S`. QMP confirmed `prelaunch, running=false`; no `cont`
   command was sent and the guest serial file stayed empty. QMP quit exited
   zero and both host PCI functions remained accessible. Evidence:
   `build/nouveau-618/paused-vfio-first/`.
2. The qualified PCI-audit kernel booted with the GPU pair assigned and
   `kpi_nouveau=0`. The oracle confirmed no Nouveau DRM card, strict boot
   checks passed and ACPI power-off completed. QEMU exited zero and both
   identities remained accessible. Evidence:
   `build/nouveau-618/vfio-only-first/`.
3. The same kernel initialized Nouveau/GSP and completed verified full driver
   removal. QEMU used `-no-shutdown`, so the guest could finish ACPI S5 while
   QEMU retained VFIO ownership. QMP confirmed `shutdown, running=false`.
   Live host config still showed GPU identity `280310de`, command `0003`
   (bus mastering disabled), and audio identity `22bd10de`, command `0006`.
   Only after QMP quit did VFIO's reset lose the downstream link; both
   identities became `ffffffff`. Evidence:
   `build/nouveau-618/gsp-handoff-boundary/`, including the exact 64-byte
   pre-exit headers in `handoff-boundary.json`.

The third run's strict result remains fail due to the LinuxKPI page free-count
invariant; successful GSP initialization/removal is recorded separately. No
further launch was attempted after PCI access was lost.

This sequence narrows the investigation to state associated with Nouveau/GSP
activity and its subsequent VFIO reset. A paused assignment and a full AvoryOS
boot without the Nouveau probe both survive. It does not identify the exact
retained GPU state or exclude a LinuxKPI defect on the Nouveau-specific path.

### Host reset-method experiment

**Result: the experiment failed to preserve GPU reuse. Do not treat this
helper as a C4 launch requirement or a fix.** On host boot
`c9f2378e-93ef-4dc2-b688-79a8d7c939d3`, the user disabled individual function
resets and the agent ran one qualified, one-vCPU 570.144 hardware fixture.
GSP initialization registered Nouveau DRM 1.4.0 on card2. The serial log shows
full GSP removal completing, booter unload returned `0 / ff`,
guest PCI handoff retained identity/pin, and ACPI S5 completed. Nevertheless,
VFIO's separate whole-device-set reset failed link retraining and both host
NVIDIA PCI identities became `ffffffff`. There was no second launch. This
demonstrates that FLR is not necessary for the observed exit failure.

The exact kernel passed software readiness; the hardware run's strict result
remains **fail** due to page free-count invariant and VKMS warm-loop PMM drift
(14 pages). Successful GSP init/removal does not override those failures.
Evidence and the separate result attribution are under
`build/nouveau-618/reset-bus-first/`, with software qualification under
`build/nouveau-618/reset-readiness/`. No persistent rule, driver rebind or
standalone host reset was installed or performed. Host reboot restores the
kernel's default reset policy. Do not repeat the experiment on the currently
inaccessible functions or disable VFIO's whole-device-set reset as a workaround.

The successful GSP initialization and unload results do not establish reliable
GPU reuse. The saved host logs show pending-transaction/FLR timeouts followed
by loss of the downstream PCIe link. The host advertises `flr bus` for the
display function and no independent reset method for audio. On this host,
selecting `bus` returns EINVAL and logs `Unsupported reset method 'bus'`.
The PCI individual-function bus-reset probe rejects other devices on the bus;
the advertised list is not proof that selecting it will still succeed after
the audio function has been enumerated. Replaying `flr bus` also fails that
probe. The existing method list remained unchanged and PCI access survived.

The revised experiment cleared the display function's individual reset list
to skip FLR. VFIO's separate whole-device-set hot-reset path remains available
for the dedicated bridge and both functions. The failed result above does not
establish an FLR defect or a recovery fix.

The failed experiment's host reset helper and its regression were removed at
the user's request. No host reset-policy prerequisite is part of C4. Investigation
continues in LinuxKPI and the native kernel PCI implementation. The retained
logs document the failed experiment, not a supported recovery procedure.

Linux documents `reset_method` selection in its
[PCI sysfs ABI](https://github.com/torvalds/linux/blob/v6.18/Documentation/ABI/testing/sysfs-bus-pci).
VFIO invokes function reset during enable/disable and can additionally reset
the device set through the slot/bus path; see
[vfio_pci_core.c](https://github.com/torvalds/linux/blob/v6.18/drivers/vfio/pci/vfio_pci_core.c).


#### Coherent candidate software results

The AMD+Nouveau 6.18 candidate compiled and linked. Its SHA256 is
`273431db36a573267a24aa3472040ed26e8b0b4094545efc8ad64e2f721fb44d`.
All 11,137 archived source blobs match the selected upstream Git commit.
The isolated 6.12 AMD+Nouveau regression candidate also compiled and linked
with the shared LinuxKPI changes. The normal kernel, ISO and active import
pin match their pre-upgrade hashes.

The eight-CPU gate-off boot reached the diagnostic, exited zero and powered
off. All four 570 CRC checks and DMA page walks passed, including all 15,521
pages of the 63,571,696-byte GSP image. Coherent DMA buffers of 4,096, 65,536
and 528,384 bytes passed. Maple ranges, GPUVA lifetime and scheduler tests
passed. Strict readiness remains **fail** because VKMS retained 29 PMM pages
in its warm loop. This is similar to the existing baseline failure; it was
not waived.

A one-CPU run of the exact same kernel/firmware pair passed strict readiness,
with VKMS first/second pass PMM deltas 158/0. This provides an explicitly
one-CPU-qualified configuration for the next GSP experiment. It does not
qualify eight-CPU hardware operation. Evidence is in
`build/nouveau-618/readiness/`, `readiness-1cpu/`, and `assessment.json`.
The private hardware fixture is prepared separately with one vCPU, 570
firmware and the Nouveau probe gate enabled. At this software checkpoint, no hardware launch had occurred; the results follow below.

Managed-resource lifecycle (including release-action matching and 8,000
concurrent releases), slab reservation rollback, and Linux file export tests
passed with ASan, UBSan and leak detection outside the ptrace-restricted
sandbox. The file-export test fixture was updated to mirror the current
shared native descriptor table; production descriptor code was not changed.
Per-CPU storage and normal Limine target regressions also passed.


### Physical 6.18 / 570 initialization and removal results

Host boot ID: `56f7f6bf-5262-4ffb-9293-5789520646ee`. Both NVIDIA functions
were VFIO-bound with valid PCI identities, D0, dedicated bridge descendants,
and the pinned power policy before the first launch. Each hardware run used
one vCPU and required strict readiness for its exact kernel/firmware pair.

1. Kernel `273431db36a573267a24aa3472040ed26e8b0b4094545efc8ad64e2f721fb44d`
   initialized GSP 570.144, returned probe result 0, registered Nouveau 1.4.0
   on DRM card2, and passed the userspace DRM_VERSION oracle. Guest power-off
   completed and both host NVIDIA identities remained valid. The upstream
   driver has no PCI shutdown callback, so this run did not prove removal.
   Evidence: `build/nouveau-618/hardware-init-pass/`.
2. Kernel `9c1f868d694061668053a0e1d4c9fed9502453bce5bf1f50ef2e94362775218f`
   passed its strict one-vCPU software suite, then initialized GSP and exposed
   card2 successfully on the same host boot. The opt-in LinuxKPI diagnostic
   `kpi_nouveau_remove_on_shutdown=1` called the real `pci_unregister_driver()`
   path before native power-off. Nouveau removal reached booter-unload,
   returned mailboxes `00000000 000000ff`, logged `booted`, and returned from
   unregister. The guest then completed NVMe shutdown and ACPI S5.
   Evidence: `build/nouveau-618/hardware-cleanup/`.

After the explicit unload test, both host NVIDIA identities became invalid.
The host journal records VFIO resetting the pair, followed by the bridge's
non-functional downstream link, failed retraining and missing DLLLA. This is
consistent with the native Linux unload/control result. It is not evidence
that GSP initialization failed. A full host power cycle is required before
another GPU launch; no retry, host reset or driver rebind was attempted.

The removal diagnostic defaults off and does not change ordinary PCI shutdown
or the standard Limine targets. `scripts/nouveau-boot.py --remove-on-shutdown`
enables it for an explicit hardware experiment and requires ordered unregister,
successful booter-unload and completion evidence before reporting cleanup
success. The dispatch regression checks default behavior, gate/failed-probe
exclusion, reverse shutdown order and idempotence with sanitizers.

GSP initialization and upstream removal are now demonstrated in AvoryOS.
Rendering, KMS output, eight-vCPU qualification, physical AMD regression and
repeatability after explicit unload remain separate outstanding gates.

### Failed-probe and missing-firmware cleanup

The LinuxKPI constructor adapter now reclaims a partially constructed NVKM
device when `nvkm_device_pci_new()` returns an error. The upstream caller
otherwise returns immediately while the partial device can remain registered.
The adapter uses the real NVKM destructor, balances PCI enable ownership,
handles failures before interrupt-list initialization, and preserves an older
device when a duplicate-handle constructor fails. Imported driver source files
remain unmodified; only the constructor symbol is renamed at compilation.

Candidate SHA256:
`8748cbabc355dfc7f8fc3046a34f1a3b7fcc7d573bde18482d1737af6a2ec248`.
Its strict one-vCPU software VM passed the real linked upstream GSP loader's
four missing-file stages, eight rounds each. Each case verifies that the
expected earlier files were actually loaded, all firmware pointers are cleared,
firmware buffer/header/byte counts and VM mappings return to baseline, and a
second destructor is harmless. The guest completed NVMe shutdown and ACPI S5.
Evidence: `build/nouveau-618/failure-readiness-1cpu/result.json` and its serial
log. These tests exercise CPU firmware ownership, without GPU MMIO or firmware
execution; they do not substitute for a physical PCI probe failure.

Sanitizer regressions additionally exercise each missing/truncated file,
allocation failures, success after failure, and duplicate/partial constructor
ownership. Negative controls deliberately omit cleanup and must fail. The
native software fixture uses a 2 MiB GSP prefix to exercise VM-backed buffers;
the host loader regression uses the full 63,571,696-byte allocation size.

The private hardware runner can omit exactly one verified 570.144 component
with `--missing-firmware gsp`, `bootloader`, `booter_load`, or `booter_unload`.
It removes the private image's AD106 GSP directory before installing the chosen
fixture, so an older base-image file cannot mask the intended failure. It
requires the exact candidate's passing software readiness before launch.
Its negative oracle requires the selected missing-file request, probe `-ENOENT`,
no Nouveau DRM node, and a clean accounting marker after failed-probe teardown.
The opt-in accounting checks firmware objects, VM mappings, IRQ actions,
workqueues, PCI enable count, managed resources, MSI state and NVKM registry
ownership. It does not claim a general heap-leak proof for every driver path.

The first physical fixture is ready under `build/nouveau-618/missing-gsp/`.
It has not been launched: the previous successful unload test left host PCI
configuration inaccessible, requiring a host power cycle first. C4 remains
open until physical negative probes and subsequent full-firmware recovery pass.
