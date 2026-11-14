# R0 audit — 2026-11-14

## Scope and evidence

Repository state inspected at AvoryOS `db9e6f0cc9c320887882518f2dcc44afc905502a`.
Linux source pin is `v6.18.42`, commit
`856a9b51680cbbed1ace1207b29e424a616cff48`. Source exists in the local
`build/linux-src` cache; the committed import manifest currently contains no
Realtek network drivers, PHYLIB, cfg80211 or mac80211 sources. No driver was
added, built into the OS, or bound during this audit.

## Target inventory

| Role | Evidence | Selection / gap |
| --- | --- | --- |
| Wired, first | Host PCI inventory: `0000:07:00.0`, Realtek `10ec:8168`, revision `0x15`, subsystem `1849:8168`, host-bound to `r8169`; IOMMU group 17; bus reset method; MSI-X routed IRQ 93 | Select as first R8169 target. IOMMU group sibling `0000:03:03.0` is AMD downstream bridge `1022:43f5`. Restore and assignment procedure needs a dedicated hardware gate. |
| Wired, second | No RTL8125 found | Obtain an exact PCI ID/revision and capable 2.5 GbE peer before R4. |
| Wi-Fi `rtw89` | No PCI Wi-Fi device found | RTL8852BE remains a candidate only. Acquire card and record subsystem/revision before lock. |
| Wi-Fi `rtw88` | No PCI Wi-Fi device found | RTL8822CE remains a candidate only. Acquire card and record subsystem/revision before lock. |
| USB | USB inventory unavailable (`lsusb`/libusb unavailable) | No USB device claims. USB is outside R0 target selection. |
| AP / wired peer | Not inventoried | Record AP model, regulatory domain, security configuration and wired peer during lab setup. |

The host reports IOMMU group 17 containing this endpoint and `0000:03:03.0`;
exclusive assignment is not yet established. No device binding or host-driver
change was made.

## Software baseline

Fresh QEMU 11.1.2 TCG runs used the existing ISO/disk artifacts (not a build
from this checkout) with `-cpu max`, q35, 4 vCPUs, 4096 MiB, OVMF and snapshot
disk mode. Separate RTL8139 and e1000e runs both initialized the native network
stack, selected `eth0`, and received DHCP lease `10.0.2.15`; e1000e
`8086:10d3` used MSI vector 105. Logs: `/tmp/avory-r0-boot/rtl8139-max.log`
and `/tmp/avory-r0-boot/e1000e-max.log`. The default CPU attempt also reached
a userspace ISA-level failure; that failure persisted with `-cpu max` in
desktop services. Traffic beyond DHCP, desktop and GPU smoke were not verified.
Earlier native RTL8139 evidence is in
`docs/firefox-rtl8139-reopen-2026-10-28.md`; historical Intel `e1000e` KVM
traffic evidence is in `docs/e1000e-plan.md`. `/dev/kvm` is unavailable, so
no fresh KVM baseline is claimed.

## Build and link spike

The pinned upstream Kbuild fragments were inspected for object lists and
Kconfig selects (see `dependency-ledger.md`). An isolated `-fsyntax-only`
negative compile was run against cached pinned sources with the same LinuxKPI
overlay/generated-header include order as `kernel/GNUmakefile`. The first
blockers are duplicate timer/workqueue/lockdep types for `r8169`; missing
page-pool descriptor fields for `rtw89`, `rtw88` and mac80211; and an incomplete
`timespec64` definition in cfg80211. Full diagnostics are in `/tmp/r0-compile/`
for this checkout. This did not emit objects or attempt a link, so unresolved
symbols remain unassessed. The committed import still omits these source trees;
R1 needs disposable import/build plumbing after the overlay collisions are
resolved.

## Architecture decision

Keep the wireless architecture provisional. The pinned tree's `rtw89` and
`rtw88` Kconfig entries depend on `MAC80211`; `MAC80211` depends on `CFG80211`.
Both families' core Kbuild objects include calls into those layers. A native
wireless replacement would still need to reproduce their driver-facing
contract, channel/regulatory handling, scan and station lifecycle, crypto/key
interfaces, and event/control plane. The current repository has no equivalent
wireless stack, and source-only inspection cannot prove that a smaller
replacement is cheaper. Next spike should evaluate importing cfg80211 and
mac80211 with a deliberately minimal feature set and enumerate missing
LinuxKPI services, especially RCU, crypto, regulatory database/policy,
Generic Netlink and wireless registration. Defer the choice until object
compile/link and interface-cost evidence exists.

## R0 disposition

Linux version is locked. RTL8168 is selected as the first wired target. Fresh
TCG baseline logs now cover native selection and DHCP, but not sustained
traffic or desktop/GPU. Required R0 exit evidence remains incomplete: two
Wi-Fi targets, RTL8125, AP/peer details, and compile/link spikes. Continue R0;
do not start binding or hardware probe work on this host from this audit alone.
