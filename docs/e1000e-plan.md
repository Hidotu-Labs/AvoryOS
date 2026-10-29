# QEMU e1000e implementation plan

Status: Phases 0–6 completed. The QEMU 82574L e1000e model now has one-queue
packet I/O with MSI preferred and INTx fallback, recovery/lifecycle parity,
and `RUN_NIC=e1000e` launcher support.
Date: 2026-10-27.

## Target and completion criteria

Add native support for QEMU `-device e1000e`, which models the Intel
82574L Ethernet controller, PCI `8086:10d3`. The installed QEMU reports
version 11.1.2. Record the actual QEMU binary/version and model properties
in validation results; upstream sources are references, not proof that a
particular installed binary behaves identically.

The completed first version must provide:

- A distinct `e1000e` PCI driver identity and `/sys/bus/pci/drivers/e1000e`
  bind/unbind interface, with automatic `ethN` naming.
- One RX and one TX queue, MTU 1500, and software protocol checksums.
- Working DHCP, ARP, IPv4/IPv6, TCP, UDP and ICMP through the existing stack.
- MSI delivery with INTx fallback; a diagnostic selector to test both paths.
- Link notifications, bounded interrupt work, TX watchdog recovery, safe
  stop/rebind/shutdown, and the current network statistics.
- Multiple Intel controllers coexisting with RTL8139 and explicit default
  interface selection through the existing `net_default=` option.
- A public launcher option: `make run RUN_NIC=e1000e`.

This establishes support for the QEMU 82574L model. Physical 82574L boards
need separate PHY/NVM/errata validation before being described as supported.
Other controllers covered by Linux's broad `e1000e` family are outside this
device-ID allowlist. MSI-X, RSS, multiple queues, offloads, jumbo frames,
power management and physical hot-remove are follow-up work.

## Architecture and existing code

Extend the native implementation in `kernel/src/drivers/net/e1000.c` with
a small, immutable controller profile selected by PCI identity. Keep the
descriptor handling, DMA allocation, network callbacks, service worker and
lifecycle synchronization shared. Add only the family-specific fields or
helpers required by the two supported controllers; avoid a general Intel
driver framework or a duplicate full driver.

Register separate driver objects for `e1000` and `e1000e`, with exact PCI
ID tables. The existing `e1000_init()` entry point can register both, and
`e1000_shutdown()` must stop both. Preserve the shared stable controller
slots and installed callback lifetime rules. Their combined capacity is
currently `NET_DEVICE_MAX` (8), rather than eight slots per driver.

The current implementation has several explicit 82540EM assumptions:

- Probe accepts only `8086:100e` and a particular BAR0 encoding.
- EEPROM fallback uses DONE bit 4 and address shift 8.
- Ring initialization assumes the old reset defaults and register controls.
- Interrupt registration requests INTx only, and startup clears
  `PCI_COMMAND_INTX_DISABLE` unconditionally.
- Logs hard-code `8086:100e`; the boot harness hard-codes the `e1000` sysfs
  path, PCI addresses and expected model.

QEMU's e1000e core supports legacy descriptors, so the initial data path can
reuse the current 16-byte ring formats. Its EEPROM command format and
interrupt behavior require explicit device-specific handling. MSI-X needs
controller cause routing in addition to programming a PCI MSI-X table;
therefore merely requesting every PCI interrupt mode is insufficient.

## Phase 0 — confirm the device contract (complete)

1. Inspect the installed model's `-device e1000e,help`, PCI identity, BARs,
   interrupt capabilities and reset state in a disposable QEMU instance.
2. Compare the matching QEMU source/revision, where available, against the
   Intel 82574 documentation. Record a short register difference table for
   reset/NVM readiness, EERD, RX/TX queue controls, descriptor selection,
   receive filtering/checksum controls, and interrupt masking/acknowledgment.
3. Confirm the 82574 EERD encoding (DONE bit 1, address shift 2, data shift
   16), supported BAR0 layout, and requirements for queue enable and link
   initialization. Resolve uncertain behavior before coding it.
4. Record baseline results from the existing e1000 host and boot tests.
   Reuse their private disks and existing boot infrastructure.

Confirmed against installed QEMU 11.1.2 using `-device e1000e,help` and
QMP `query-pci` with the device at `00:01.0`:

| Property | Observed |
| --- | --- |
| PCI identity and interrupts | `8086:10d3`, Ethernet class `02:00`, INTx pin 1; QEMU source initializes one MSI vector and a five-entry MSI-X table |
| BAR0 | 128 KiB, non-prefetchable 32-bit MMIO; this is the controller register map |
| BAR1 | 128 KiB flash region |
| BAR2 | 32-byte I/O region |
| BAR3 | 16 KiB MSI-X region |
| BAR6 | 256 KiB expansion ROM region |

The QEMU v11.1.2 e1000e model identifies itself as an Intel 82574, and its
core handles reset through `CTRL.RST`. The register source defines the 82574
EERD command with START bit 0, DONE bit 1, address shift 2 and data shift 16.
QEMU's EERD model returns the EEPROM word for the requested address. RAR0 is
the first MAC source; bounded EERD reads are the fallback. Legacy descriptor
structures are supported by the QEMU core, but e1000e defaults also enable
newer features, so Phase 3 must explicitly select and validate a one-queue
legacy RX/TX configuration.

The baseline `test_e1000_probe.py` initially failed compilation because its
MMIO mapping fixture had stale PMM/VMM stubs but no `pci_map_mmio` stub. The
fixture now models that mapper directly. The existing e1000 host and QEMU
regressions pass after the refactor.

Exit gate met: the compatibility profile covers the model-specific ID, BAR
size and EERD layout; descriptor and interrupt differences remain explicit
work for later phases. No other device IDs are enabled.

## Phase 1 — introduce profiles while preserving e1000 behavior (complete)

1. Introduce a controller profile containing the device/model identity,
   BAR requirements, EEPROM encoding, and necessary reset/configuration
   differences. Store its pointer in each controller slot.
2. Convert the 82540EM-specific probe, MAC fallback and log formatting to
   use that profile, initially registering only the existing controller.
3. Prepare registration helpers for separate driver objects without
   duplicating the service worker or shutdown traversal.
4. Parameterize relevant host-test fixtures so register side effects are
   modeled independently for each family. Assertions must distinguish the
   encodings rather than accept either encoding indiscriminately.

Exit gate met: the kernel builds; `python3 scripts/tests/test_e1000_probe.py`
passes for the 82540EM path, and
`python3 scripts/tests/test_e1000_probe_boot.py --case e1000 --seconds 0
--smp 4` passes traffic, recovery, rebind and shutdown coverage. QEMU still
binds the 82540EM as `e1000`.

## Phase 2 — probe, reset and identify the 82574L (complete)

1. Add the exact `8086:10d3` profile and `e1000e` driver registration.
2. Validate BAR0 against the audited layout, including size and address
   arithmetic. Use the existing uncached PCI MMIO mapper.
3. Establish a quiescent probe sequence: mask controller interrupts, disable
   RX/TX and bus mastering, reset with a deadline, then wait for the
   documented/model-specific initialization state.
4. Read a valid unicast MAC from the initialized receive address registers,
   falling back to the 82574 EERD format with bounded waits. Keep the
   82540EM fallback independent.
5. Register the persistent netdev and expose the correct PCI driver link.
   Probe alone leaves packet DMA and interrupts disabled.
6. Cover wrong IDs/classes, ownership conflicts, invalid BARs, all-ones
   MMIO, mapping failure, reset/NVM timeout, invalid MAC and slot exhaustion.

Exit gate met: the host probe covers both RAR0 MAC discovery and the 82574
EERD fallback, exact driver/device identity, undersized and out-of-range BARs,
MMIO mapping/reset/NVM failures, an all-ones register read, invalid EEPROM
MAC data, PCI command state, slot exhaustion and device/profile separation.
A QEMU boot test verifies `8086:10d3`, `/sys` binding to `e1000e`, the
configured MAC and probe-only PCI command state. The profile keeps e1000e
activation disabled until Phase 3 validates RX/TX setup, so the probe-only
guest leaves DMA and INTx disabled. Packet traffic is not claimed.

## Phase 3 — one-queue traffic with INTx

1. Configure the 82574 explicitly for legacy RX/TX descriptors and one
   queue. Program the audited RX/TX controls and disable RSS, extended or
   packet-split RX modes, VLAN transformations and unimplemented offloads.
2. Reuse the 256-entry rings, 2 KiB buffers and DMA publication rules.
   Retain the initial below-4-GiB allocation policy to reduce bring-up
   variables. Validate padding, FCS handling, packet lengths and ring wrap.
3. Implement the audited INTx cause/mask/acknowledgment behavior. Keep the
   existing bounded RX drain and verify that continuation requests actually
   produce subsequent interrupts on this model.
4. Accept broadcast and multicast traffic needed by DHCP and IPv6 NDP.
   Ensure receive checksum metadata cannot be mistaken for packet errors
   when the stack calculates its own checksums.
5. Exercise DHCP, ARP, ICMP, short and MTU-size UDP, at least 1 MiB of
   byte-verified TCP echo, and IPv6 NDP/SLAAC/UDP.

Exit gate: e1000e-only traffic passes through INTx; ring wrap and RX bursts
continue making progress without packet corruption or an interrupt storm.
Host tests cover saturation, malformed RX descriptors, fragment chains,
allocation failures, RX queue exhaustion and IRQ registration failure.

Completed on the installed QEMU 11.1.2 model. The 82574L setup now clears
`RFCTL.EXTEN`, disables `MRQC`/RSS, RX checksum offload, and VLAN transforms,
leaves RCTL in legacy-descriptor mode, disables TARC queue 1, and applies the
82574 TARC0 erratum bit after every ring setup. The common bounded INTx handler retains
read-to-clear ICR handling and ICS continuation. e1000e host probes cover TX
saturation, RX burst continuation, reset/recovery, and profile reapplication.
QEMU KVM boots passed DHCP, 1 MiB byte-verified TCP/ring wrap, short and MTU
UDP, ICMP, and IPv6 UDP through NDP/SLAAC. The QEMU v11.1.2 model routes
legacy RX/TX causes through INTx when MSI is disabled.

Exit gate met: e1000e-only traffic and the shared profile-specific host probes
pass with INTx; no MSI/MSI-X claim is made in this phase.

## Phase 4 — recovery and lifecycle parity

1. Apply the device-specific reset/configuration sequence consistently to
   initial start, watchdog recovery, stop and rebind.
2. Preserve lifecycle serialization, packet generations, stable callbacks,
   DMA quarantine on reset failure, and the shutdown worker guard.
3. Verify the watchdog recovers an injected TX stall while carrier is up,
   skips recovery while carrier is down, and still reclaims completions
   after a lost TX interrupt.
4. Toggle link through QMP during concurrent traffic. Check carrier,
   operstate, transition counters and route-netlink notifications.
5. Run repeated bind/unbind cycles, including a mixed e1000/e1000e guest.
   Confirm that operations on one controller do not reset or reconfigure
   another and that selected-default restoration is correct.

Exit gate: post-recovery traffic passes; successful stops release DMA;
failed resets retain DMA safely; repeated rebinds have a constant active
DMA footprint and bounded memory usage. Shutdown disables packet engines,
bus mastering and interrupt delivery for both Intel models.

Completed. e1000e host probes verify watchdog suppression while link is down,
recovery after link returns, completion reclaim when a TX interrupt is lost,
DMA quarantine after a simulated reset failure, successful stop cleanup, and
shutdown with RX/TX and PCI bus mastering disabled. The QEMU recovery case
passes post-recovery traffic and three bind/unbind cycles. A four-stream mixed
e1000/e1000e QEMU run toggled the selected e1000e link through QMP while traffic
was active, checked carrier/operstate/netlink events, rebound both controllers,
verified the non-target controller stayed active during each rebind, and
confirmed that the selected default and stable DMA footprint were restored.

Exit gate met: post-recovery traffic, stop/rebind/shutdown, reset-failure
quarantine, mixed-controller isolation and selected-default restoration pass
in the recorded QEMU KVM SMP4 runs. The 82540EM path also passed its boot
regression after these changes.

Recorded validation on QEMU 11.1.2 with KVM and four vCPUs:

```sh
make -C kernel -j4
python3 scripts/tests/test_e1000_probe.py
python3 scripts/tests/test_e1000_probe_boot.py --case e1000 --seconds 0 --smp 4
python3 scripts/tests/test_e1000_probe_boot.py --case e1000e --seconds 0 --smp 4
python3 scripts/tests/test_e1000_probe_boot.py --case mixed-intel --seconds 1 --smp 4
python3 scripts/tests/test_e1000_probe_boot.py --case recovery-e1000e --seconds 1 --smp 4
```

All commands passed. The mixed case ran four concurrent integrity-checked TCP
streams while QMP took the selected e1000e link down and back up, then checked
both devices through three rebind cycles. `git diff --check` and Python syntax
compilation also passed. The build retains the existing linker warning about
an RWX load segment.

## Phase 5 — MSI with a tested INTx fallback

1. Request one MSI vector with INTx fallback through `pci_irq_request_modes`.
   Keep 82540EM on its existing INTx path during this expansion.
2. Make startup/quiesce explicitly mode-aware: INTx remains disabled while
   MSI is used; do not undo the PCI IRQ layer's command-register settings.
3. Audit the 82574 interrupt acknowledgment and automatic masking controls
   for MSI. Configure a simple, explicit policy and restore it after reset.
4. Define a safe lifetime for retained MSI vectors/callbacks across stop,
   recovery and rebind. Keep delivery masked while inactive and restore
   the required PCI/controller state before resuming. Do not reclaim a
   callback/vector while an interrupt can still be in flight.
5. Add `e1000e_irq=auto|intx|msi`: auto prefers MSI and falls back to INTx;
   forced MSI fails activation if unavailable, making its tests meaningful.
   Log the actual selected mode and vector/line.
6. Test successful MSI allocation, allocation/programming failure with
   fallback, forced-mode failure, and traffic/recovery/rebind in both modes.

Completed. The e1000e uses one MSI vector by default and falls back to INTx
through `pci_irq_request_modes`; `e1000e_irq=intx|msi` forces either mode.
The 82540EM remains INTx-only. Startup leaves legacy INTx disabled under MSI,
keeps IAM auto-mask disabled for the legacy read-to-clear ICR path, and masks
the PCI MSI Enable bit while rings are inactive. The vector and callback remain
installed across reset, stop and rebind; activation restores ring, controller
and PCI interrupt state before enabling MSI. Logs show the actual vector or
legacy line.

Host probes exercise MSI programming and vector-allocation failures, INTx
fallback, forced-mode failure, stale-source cleanup and vector retention. The
QEMU 11.1.2 e1000e guest passed auto, forced INTx and forced MSI traffic,
recovery/rebind and shutdown. The existing shared-line INTx host and e1000
boot regressions pass.

Exit gate met: each delivery mode works independently, MSI keeps legacy INTx
disabled, and failed allocation/programming leaves no enabled MSI source or
leaked vector. The retained vector is intentionally not reclaimed because the
IRQ registry has no in-flight uninstall grace period.

## Phase 6 — launcher integration and release validation

1. Add `e1000e` to `RUN_NIC` validation, diagnostics and documented examples.
   Keep the existing meaning of `both` (RTL8139 plus e1000); use the test
   harness for richer combinations.
2. Generalize the boot harness around explicit NIC model, driver identity,
   MAC, PCI address and selected interface. Eliminate substring binding
   checks that would confuse `e1000` with `e1000e`.
3. Add e1000e-only, two-e1000e, e1000+e1000e, and
   RTL8139+e1000+e1000e cases. Select each supported driver as default in
   appropriate cases, while respecting the stack's single configuration.
4. Extend recovery and network statistics/NetworkManager coverage to
   e1000e. Check HTTP payload hashes and interface discovery alongside the
   protocol tests. Keep the existing no-NIC/RTL8139/e1000 regression cases.
5. Document supported IDs, interrupt policy, diagnostic options, test
   commands, known limits and actual validation results in `docs/e1000.md`.

Completed. `RUN_NIC=e1000e` is accepted by the interactive launchers; `both`
still means RTL8139 plus 82540EM e1000. The boot harness covers e1000e alone,
forced MSI and INTx, two e1000e functions, both Intel models, all three NIC
models, exact driver/MAC/interface selection, link notifications, and repeated
rebinds. The NetworkManager test now exercises e1000e discovery, live counters
and HTTP payload hashing through the current rootfs init configuration.

Validation passed on QEMU 11.1.2 with KVM: the SMP4 NIC matrix, four-stream
30-second stress in each e1000e mode with link toggles, 1- and 8-vCPU lifecycle
runs, representative TCG boots in both modes, and e1000e NetworkManager/
statistics/HTTP SHA-256 coverage. Host probes passed the native PCI allocator
fallback and e1000/e1000e profile/lifecycle checks. No physical 82574L hardware
was validated; the documentation limits support claims to QEMU.

Exit gate met: `make run RUN_NIC=e1000e` is accepted, the release checks pass,
and documentation distinguishes QEMU validation from physical hardware support.

## Final validation matrix

| Area | Required coverage |
| --- | --- |
| Host tests | Both profiles; identity/BAR/NVM failures; descriptors; bounded interrupts; DMA cleanup/quarantine; recovery; MSI allocation/fallback |
| Existing regressions | No NIC, RTL8139, e1000, existing mixed/default-selection and recovery cases |
| e1000e traffic | INTx and MSI; DHCP, ARP, TCP integrity/ring wrap, short/MTU UDP, ICMP, IPv6 |
| CPU/accelerator coverage | Full 4-vCPU KVM matrix when available; e1000e traffic/lifecycle at 1 and 8 vCPUs; representative 4-vCPU TCG runs in both interrupt modes |
| Multiple devices | Two e1000e devices; both Intel models; all three NIC models with explicit default selection |
| Stress | Four concurrent verified TCP streams, at least 30 seconds per mode; link down/up during load; traffic afterward |
| Lifecycle | Injected TX stall, lost TX interrupt, three or more rebind cycles, reset failure simulation, clean shutdown |
| Userland | Live counters/carrier/operstate, netlink notifications, NetworkManager discovery, HTTP SHA-256 integrity |

Use disposable test disks; retain serial/QMP logs and the tested kernel
snapshot. Record unavailable KVM or other infrastructure as an unexecuted
check, rather than a pass. Report throughput/CPU observations as measured
results; emulation support alone does not promise a speed improvement.

## Source references

- Existing implementation: `kernel/src/drivers/net/e1000.c`,
  `e1000_regs.h`, and `e1000.h`.
- Interrupt allocation/routing: `kernel/src/drivers/pci/pci_irq.c` and
  `pci_irq.h`.
- Existing checks: `scripts/tests/test_e1000_probe.py`,
  `test_e1000_probe_boot.py`, and `test_network_statistics_boot.py`.
- [QEMU v11.1.2 e1000e PCI model](https://github.com/qemu/qemu/blob/v11.1.2/hw/net/e1000e.c).
- [QEMU v11.1.2 e1000e core](https://github.com/qemu/qemu/blob/v11.1.2/hw/net/e1000e_core.c).
- [QEMU v11.1.2 Intel register definitions](https://github.com/qemu/qemu/blob/v11.1.2/hw/net/e1000x_regs.h).
- [Intel 82574 datasheet](https://www.intel.com/content/dam/doc/datasheet/82574l-gbe-controller-datasheet.pdf).

The QEMU source links are pinned to v11.1.2, matching the installed emulator.
