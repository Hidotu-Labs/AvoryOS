# R1 slice — imports and default-off family gates

Date: 2026-11-14. AvoryOS baseline: `db9e6f0cc9c320887882518f2dcc44afc905502a`.
Linux source: `v6.18.42`, `856a9b51680cbbed1ace1207b29e424a616cff48`.

## Implemented

- Added upstream Ethernet Realtek, PHY, Realtek wireless, cfg80211 and mac80211
  source trees to `scripts/linux/subset.txt`. Import copies sources and private
  headers; it does not register PCI drivers.
- Extended `scripts/linux/kbuild-subset.py` to evaluate selected upstream
  Kbuild fragments for `r8169`, PCI `rtw89` RTL8852BE, PCI `rtw88` RTL8822CE,
  cfg80211 and mac80211. It emits separate object groups with build gates.
- Added `KPI_R8169=0`, `KPI_RTW89=0`, `KPI_RTW88=0` defaults in
  `kernel/GNUmakefile`, independent of each other. Wireless core objects are
  included only when at least one Wi-Fi family is enabled. Gate values other
  than the exact strings `0` and `1` fail during make parsing.
- Added source sentinels: enabling a gate fails if its generated Kbuild
  fragment lacks required family/core objects. The kernel profile stamp now
  includes the three gates, so gate changes invalidate the build profile.
- The generated driver objects use `KBUILD_MODNAME="r8169"` and
  `KBUILD_MODNAME="rtw88_8822ce"` where those values are consumed as the PCI
  driver name. `rtw89_8852be` names its PCI driver explicitly in upstream code.
- Added runtime per-driver bind gates to LinuxKPI PCI. Each candidate needs a
  boolean and canonical BDF: `kpi_r8169=1 kpi_r8169_bdf=0000:bb:ss.f`,
  `kpi_rtw89_8852be=1 kpi_rtw89_8852be_bdf=0000:bb:ss.f`, or
  `kpi_rtw88_8822ce=1 kpi_rtw88_8822ce_bdf=0000:bb:ss.f`. All booleans default
  false; empty/malformed BDFs, non-Realtek vendor IDs, non-network classes,
  wrong driver names and BDF mismatches fail closed. Build gates and runtime
  selectors are independent. No physical NIC was bound.
- Realtek LinuxKPI probes reserve native device-manager ownership using the
  same `struct device` lock as native `dm_bind_device()`. Native kernel/user
  binds and device destruction reject the external owner state. Failed probes
  release the reservation; successful unregister keeps it through remove,
  resource cleanup and Linux sysfs unpublication, then releases it. This
  reservation change is scoped to the three Realtek candidates; graphics
  binding behavior is unchanged.
- Moved `net_core_init()` ahead of LinuxKPI initcalls on the root-mounted path.
  The diskless/mount-failure path still initializes it exactly once at
  `mount_fail` before native NIC registration.
- Extended the QEMU EDU PCI fixture to check that a second external claim is
  rejected and that claims are available again after unregister and failed
  probe cleanup.
- Added candidate Realtek firmware and license files to the packaging manifest
  and pinned SHA-256 list. linux-firmware 20260916 source reports RTL8168H
  firmware version 0.0.2; RTL8852B's code advertises firmware format maximum 1
  and tries `rtw8852b_fw-1.bin` followed by `rtw8852b_fw.bin`. `-2.bin` is not
  requested by this pinned driver code. RTL8822C lists normal and WoW firmware;
  the WoW image may be requested on a PM path.

## Evidence

Ran `scripts/linux-import.sh --force` against the local pinned source cache.
Import completed at the expected tag and commit. The Kbuild report included:
3 R8169 objects, 21 RTL8852BE objects (core/PCI/chip), 20 RTL8822CE objects
(core/PCI/chip), 17 cfg80211 base objects and 41 mac80211 base objects. Optional
features and PHYLIB runtime closure are not represented as link-complete.

`make -C kernel -n` succeeded with all Realtek defaults off and contained no
Realtek driver or wireless-core objects. Before regeneration, `KPI_R8169=1`
was rejected because the stale generated fragment lacked required sources;
invalid gate value `bogus` was also rejected. A targeted dry run showed the
`rtw88_8822ce` compile command overrides the default module name.

Focused object compilation succeeded for the native device manager, native
LinuxKPI PCI bridge, LinuxKPI PCI registry, kernel boot path and EDU PCI
fixture. The complete default-profile kernel linked and the ISO rebuilt with
`boot_selftest=1`. The full boot-test suite stalled before EDU, so a disposable
test-only build temporarily selected only `linuxkpi_test_phase4_pci()` in the
boot-test thread; that source change was reverted after the run. A second
disposable build exercised the real `pci.c` Realtek gate and claim lifecycle:
candidate-identity acceptance was temporarily extended to QEMU EDU and gate
state was set by a test-only helper; production gate and claim code paths were
otherwise unchanged. Driver callbacks did not claim/release manually. The
serial log `/tmp/r1-auto-edu.log` shows default-off rejected probe, wrong BDF
rejected probe, exact BDF ran probe with native ownership reserved, unregister
released the claim, and failed probe released the claim. The run ended with
`[  OK  ] LinuxKPI: pci suite complete`. All temporary source changes were
restored and the default ISO rebuilt. This validates selector/ownership logic
on an EDU fixture, not an actual Realtek device, probe or network I/O.
Separate fresh QEMU TCG boots using the rebuilt default-off ISO and
`-cpu max` reached DHCP on native RTL8139 and E1000E: both selected `eth0`,
received `10.0.2.15/24`, and logged `Lease obtained`. Userspace also logged
`CPU ISA level is lower than required`; no desktop or traffic test is claimed.
QEMU emitted host AIO permission warnings while the read-only baseline disk was
attached; the captured guest networking evidence precedes those warnings.

Enabled-family object recipes were also exercised by the gate paths, not just
dry-run parsed. The R0 incompatibilities recur: r8169 compilation reports
duplicate lockdep/timer/workqueue types, while rtw89 and rtw88 encounter the
missing pagepool/netmem layout. See `/tmp/avory-r1/R8169.log`,
`/tmp/avory-r1/RTW89.log` and `/tmp/avory-r1/RTW88.log`; enabled families
remain blocked on shared compatibility work.

Staged firmware from the local linux-firmware 20260916 tree through
`LINUX_FIRMWARE_SRC=/tmp/r1-fw-src scripts/linux-firmware-install.sh`; all 23
manifested files passed SHA-256 checks. Host `ethtool -i enp7s0` reports
`firmware-version rtl8168h-2_0.0.2 02/26/15` for `r8169` at `0000:07:00.0`,
confirming the selected blob matches the physical host's driver request. This
does not yet prove AvoryOS firmware-loader behavior.

## Still open

- The negative compile findings in `dependency-ledger.md` remain. Enabling a
  family is not a passing build and should not be used to bind hardware.
- Runtime default-off exact-BDF selectors, native ownership reservation and
  network-core init ordering are implemented. Focused object compilation and
  disposable EDU tests for disabled/wrong-BDF/exact-BDF selection and automatic
  claim lifecycle passed. The full default-profile kernel and ISO build
  succeeded. Actual Realtek PCI/native-manager integration remains unverified.
- Firmware identity is host-confirmed for rtl8168h-2 via `ethtool -i`; actual
  firmware loading by an AvoryOS Realtek probe remains untested.
- Firmware packaging does not establish network boot behavior. No firmware
  request has been exercised through a Realtek driver.
