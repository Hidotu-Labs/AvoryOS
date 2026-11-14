# Realtek Ethernet and Wi-Fi through LinuxKPI

Status 2026-11-14: R0 inventory, software audit and native-network QEMU
baseline are recorded. R1 import/build gates, scoped default-off PCI selectors,
native ownership coordination and network-init ordering are implemented; a
disposable EDU fixture passed the Realtek gate and automatic claim lifecycle.
R2 has implemented shared
split-type compatibility plus RX and short-lived protocol TX references across
unregister, and bounded references/snapshots for audited procfs, sysfs, socket
ioctl, and route-netlink reporting paths. R2's reusable linear skb, NAPI,
RX/TX and backpressure bridge passed its opt-in QEMU software fixture,
including byte comparisons and a blocked-reference unregister test. Native
RTL8139 and E1000E QEMU DHCP/GPU regressions also passed. R3 is complete on
the physical RTL8168, and R4 wired data qualification passed on RTL8168h (with
RTL8125 deferred due to no hardware in inventory); R5–R9 are open. Linux is locked to
`v6.18.42` at `856a9b51680cbbed1ace1207b29e424a616cff48`, the current
`scripts/linux/baseline.txt` pin. The first wired target is onboard RTL8168
(`10ec:8168`, revision `0x15`); no physical Wi-Fi or RTL8125 target is
available in the current host inventory. See `docs/realtek/r0-audit.md` and
`docs/realtek/r1-import-gates.md`.

R3 is complete. Upstream `r8169` probed the physical RTL8168h (XID 541)
through VFIO passthrough. It loaded `rtl8168h-2` firmware, attached the
Realtek PHY, reached link-up and registered a native interface that obtained
DHCP. An injected failure after resource allocation unwound cleanly, and the
same-boot re-probe succeeded. That test found and fixed an unmanaged MSI vector
leak in LinuxKPI. r8169 is now built and bound by default (opt out with
`kpi_r8169=0` or `make KPI_R8169=0`); only RTL8168h has hardware evidence.
R4 wired traffic qualification passed on physical RTL8168h (DHCP, DNS,
HTTP/HTTPS, YouTube streaming, and dual-NIC `net_default=` coexistence with
emulated `e1000`), after fixing TCP `snd_wl1`/`snd_wl2`/`snd_wnd` initialization
for peer ISNs `>= 0x80000000`. Physical RTL8125 qualification is deferred
(no RTL8125 hardware in inventory).
Evidence: `docs/realtek/r3-compile-closure.md`, `docs/realtek/r4-wired-traffic.md`.

## Goal and boundaries

Bring up upstream `r8169` on one RTL8111/8168 revision and one RTL8125,
then bring up PCIe station mode on one `rtw89` device (candidate RTL8852BE)
and one `rtw88` device (candidate RTL8822CE). Record coverage by tested PCI
ID, subsystem ID and revision. First release Wi-Fi uses WPA2-Personal and
the native IP stack. Later chunks cover USB, other PCIe families, and more
wireless features.

AvoryOS already has native RTL8139 and Intel NICs, a native Ethernet frame
queue and LinuxKPI PCI, IRQ, DMA, workqueue and firmware services. The
network boot path selects one default interface for its IPv4/IPv6 state.
The imported networking path needs packet and lifecycle compatibility.
The wireless path also needs runtime association and IP activation.

```text
Wired:   native IP <-> native net_device bridge <-> Linux net_device <-> r8169
Wireless data:
         native IP <-> native net_device bridge <-> mac80211 <-> rtw89 / rtw88
Wireless control:
         wpa_supplicant <-> AF_NETLINK / Generic Netlink / nl80211
                        <-> cfg80211 <-> mac80211 <-> radio driver
```

The wireless architecture is provisional until the R0 dependency spike.
FreeBSD ships both Realtek Wi-Fi families using LinuxKPI and its native
wireless stack. Use its port as a reference for driver behavior and
compatibility gaps. [FreeBSD LinuxKPI wireless report](https://www.freebsd.org/status/report-2026-04-2026-06/lkpi-wireless/)

## Ordered implementation chunks

R0 → R1 → R2 → R3 → R4 closes wired bring-up. R5 can begin after R2,
while R3–R4 are running. R5 → R6 → R7 closes Wi-Fi bring-up. R8 joins both
paths for release qualification. R9 extends hardware and feature coverage.

### R0 — Freeze targets and audit the baseline

1. Inventory available NICs: PCI ID, subsystem ID, revision, BDF, host driver,
   IOMMU group, IRQ capabilities and reset support. Pick the first wired
   card and first radio based on availability. Record the AP and wired peer.
2. Capture a software baseline with native RTL8139 and Intel networking.
   Record boot, link, DHCP, traffic, current device selection and GPU smoke
   results so shared LinuxKPI changes have a comparison point.
3. Resolve Kbuild objects and Kconfig selects for `r8169`, selected
   `rtw89` and `rtw88` chips, PHYLIB/Realtek PHY and wireless core. Build
   isolated compile/link spikes. Log every unresolved symbol, behavioral
   gap, optional feature and firmware request, with its owning chunk.
4. Decide the wireless architecture from the spike. Evaluate importing
   pinned `mac80211`/`cfg80211` with AvoryOS compatibility APIs, including
   its crypto, RCU, regulatory and netlink dependency closure. Record the
   required behavior and cost before choosing any alternative.

**Exit:** Target and version lock, dependency ledger, baseline logs,
wireless architecture decision and list of hardware still needed.

### R1 — Wire reproducible imports and isolate driver binding

1. Update `scripts/linux/subset.txt`, object selection and
   `kernel/GNUmakefile` so each driver family has an independent
   build gate. Keep compatibility headers in the LinuxKPI overlay and
   document any tracked upstream patches. Verify clean import regeneration.
2. Add opt-in boot binding by exact BDF for each candidate. Audit LinuxKPI
   registration and native PCI claims together. Establish a single owner,
   return the claim on failed probe and order registration around native
   network initialization.
3. Extend the existing firmware packaging and hash manifests for the exact
   filenames requested by the chosen revisions. Record firmware version and
   distribution terms. Log missing firmware with the PCI ID and pathname.

**Exit:** Candidate builds are reproducible and disabled by default. Native
NIC and GPU behavior matches R0 with gates off. Scoped registration and
failed-claim release have software evidence.

### R2 — Implement the reusable Ethernet bridge

1. Define `sk_buff` ownership, linear frame allocation, headroom, queues
   and required DMA mapping behavior. Implement budgeted NAPI scheduling,
   completion and synchronous disable using the existing scheduler.
2. Map Linux registration, open/stop, carrier and TX queue state to
   `kernel/src/net/core.h`. Deliver RX through the native frame queue and
   TX through `ndo_start_xmit`. Propagate `NETDEV_TX_BUSY` without losing
   ownership or spinning. Begin with MTU 1500 and only implemented offloads.
3. Specify shutdown ordering and verify it under load: stop admission,
   synchronize IRQs, drain polling/work, and retire queued native references
   before freeing the Linux device. Use native generation checks plus a
   separate object lifetime guarantee.
4. Exercise the bridge with a software fixture for RX/TX, queue backpressure,
   budget exhaustion and teardown. Optionally use upstream `8139too` as a
   QEMU canary with exclusive ownership of the emulated RTL8139.

**Exit (software bridge):** Frame payloads match in both directions. Queue
stop/wake and failure paths preserve ownership. Repeated teardown drains
resources. Achieved by the fixture recorded in
`docs/realtek/r2-net-core.md`; imported Realtek driver and physical traffic
remain outside this exit and are tracked by R3/R4.

### R3 — Probe r8169 and negotiate link on RTL8111/8168

1. Close active PHYLIB, MDIO, Realtek PHY and firmware dependencies. Audit
   reachable ethtool/PM callbacks and expose the implemented capabilities.
2. Verify BAR mapping, IRQ mode, coherent rings, DMA masks and streaming
   sync. Include a 32-bit DMA case on a system with RAM above 4 GiB.
3. Boot with one assigned device, verify binding, reset, PHY discovery,
   firmware and negotiated link. Capture serial diagnostics at each step.
   Exercise missing firmware where applicable and a probe failure after
   resources have been allocated. Verify release and subsequent good probe.

**Exit:** The exact RTL8111/8168 target reaches link-up and registers a
native interface. Hardware logs show probe and failed-probe cleanup.

### R4 — Deliver wired data and qualify RTL8125

1. Run DHCP, gateway traffic and sustained bidirectional TCP/UDP on the
   RTL8111/8168 target. Cover TX saturation, cable pull/replug, link recovery
   and stop/rebind under load. Check counters and DMA/resource balance
   before and after repeated cycles.
2. Bring up RTL8125 through the same gates. Verify 2.5 GbE negotiation and
   record throughput against a capable peer, including dropped frames.
3. Boot with a native NIC and an imported NIC. Verify default selection,
   driver gating and existing native RTL8139 and Intel QEMU paths.

**Exit:** Publish a wired report with exact hardware revisions, firmware,
traffic measurements, recovery logs and known limits.

### R5 — Bring up the wireless core and scan with rtw89

1. Implement the R0 wireless architecture. Close the core dependencies
   required for registration, scan, regulatory enforcement, crypto and
   asynchronous completion. Add PHY registration and scan diagnostics.
2. Bring up the chosen `rtw89` PCIe card through firmware boot and hardware
   scan. Confirm valid channels and power limits before permitting radio TX.
   Check scan cancellation, timeout recovery and missing firmware cleanup.
3. Check teardown with a scan in flight and verify the card can reinitialize.

**Exit:** Hardware scan returns valid results on permitted channels, with
repeatable recovery and no orphaned IRQ, workqueue or DMA resources.

### R6 — Connect a secured Wi-Fi station and carry IP traffic

1. Extend AvoryOS's netlink socket layer with the required Generic Netlink
   family discovery, `nl80211` operations and multicast events. Audit
   interface discovery and the chosen `wpa_supplicant` EAPOL transport.
   Implement the required packet socket surface if the selected path uses it.
2. Bring up open association in a lab, then WPA2-Personal authentication,
   key install, EAPOL/data delivery and disconnect. Verify wrong credentials
   and clean reconnect. Keep credentials out of evidence logs.
3. Add runtime network activation to replace reliance on the one-shot boot
   selection. Under the single-default model, define how association and
   link loss affect DHCP, address clearing and interface selection. Test
   Wi-Fi-only boot and wired/Wi-Fi coexistence.

**Exit:** On the selected `rtw89` card, `wpa_supplicant` connects to a WPA2
AP and a Wi-Fi-only boot acquires DHCP and carries traffic. Disconnect and
reconnect leave IP configuration consistent.

### R7 — Bring up rtw88 on the same station path

1. Import the selected PCIe chip objects and firmware using R1 tooling.
   Record differences in scan handling, firmware requests, IRQ mode,
   DMA constraints and feature advertisement.
2. Run the R5 and R6 hardware gates on the selected `rtw88` device.
   Run `rtw89` regression coverage after shared fixes. Track each card's
   supported bands and rates from hardware evidence.

**Exit:** Publish a Wi-Fi report for each radio with scan, WPA2, DHCP,
traffic, reconnect and cleanup evidence.

### R8 — Qualify the first supported release

1. Repeat cold boot and warm reboot, stop/rebind under traffic, missing
   firmware and recovery on each advertised device. Run sustained traffic
   and capture throughput, errors, stalls and warmed-up resource counts.
2. Run native NIC and GPU regression boots with candidate drivers disabled
   and with validated devices enabled. Verify firmware packaging from a
   clean build and record the exact commands for reproducing each case.
3. Document device IDs, firmware, known limits, enable/disable controls
   and restore procedure. Advertise suspend/resume only after a separate
   hardware gate for it passes.

**Exit:** Supported-device matrix, reproducible evidence, and documented
rollback. Every advertised hardware revision has a physical result.

### R9 — Expand Realtek coverage in independent follow-ups

Audit additional `r8169` revisions before extending the matrix. Give
`rtase` and USB `r8152` their own import, probe and traffic gates. Treat
USB Wi-Fi and older `rtlwifi`/`rtl8xxxu` as separate transport and driver
work. Qualify WPA3, roaming and AP mode independently from new PCI IDs.

## Evidence and rollback

Use `docs/realtek/rN-*.md` for phase reports and add a checkpoint to
`docs/linuxkpi-progress.md` at each exit. Record AvoryOS commit, Linux pin,
firmware hashes, PCI IDs, build gates, boot parameters, serial log paths,
and results per case. Keep software fixture, QEMU and physical results
distinct. Record blocked conditions against their exact exit criterion.

For hardware sessions, start with one assigned device or a dedicated test
system. Capture the host binding and restore procedure before assignment.
Use the opt-in gates to roll back to the native NIC path. Keep wired as a
recovery path when testing wireless-only changes.

**First implementation deliverable:** R0 report, version lock and dependency
ledger. Re-estimate after the compile/link spikes and first hardware probe.
