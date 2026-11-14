# R3 — `r8169` probe, PHY and link on RTL8168

Date: 2026-11-14. Linux source: `v6.18.42`,
`856a9b51680cbbed1ace1207b29e424a616cff48`.

## Status

**R3 exit met (2026-11-14).** Upstream `r8169` probed the physical onboard
RTL8168h/8111h (`10ec:8168` rev `0x15`, XID 541, ASRock subsystem), loaded
`rtl_nic/rtl8168h-2.fw`, attached the Realtek PHY over its MDIO bus, reached
link-up and registered native `eth0`, which then obtained a DHCP lease. In the
same boot, a probe failure injected after resource allocation unwound cleanly
and the following probe succeeded. The device was assigned through VFIO to a
KVM guest; see "Hardware evidence". Wired traffic qualification, link
recovery and RTL8125 belong to R4.

**Default since 2026-11-14 (after R3):** `KPI_R8169` defaults to `1` and the
runtime selector `kpi_r8169` defaults to on. With no `kpi_r8169_bdf`, r8169
binds every Realtek-vendor network-class device in its ID table; the BDF
selector narrows binding to one device and `kpi_r8169=0` (or
`make KPI_R8169=0`) opts out. Only RTL8168h rev `0x15` has hardware evidence;
other chips in r8169's table bind but are unqualified until R4/R9. RTW89 and
RTW88 remain default-off and BDF-gated.

## Implemented slice

The R2 narrow networking overlay had defined `IFF_BROADCAST` and
`IFF_MULTICAST` as macros. Upstream Linux UAPI declares them as enum values
before defining their self-referential compatibility macros, so compiling
`r8169_main.c` failed at the UAPI include with “expected identifier”. The
overlay now imports the pinned UAPI `if.h`, `if_arp.h` and `if_ether.h`
definitions instead of duplicating those constants. This preserves Linux's
flag values and resolves that namespace/parse conflict for both the frontend
and candidate source recipe.

The Kbuild subset now models R8169's explicit Kconfig dependency closure:
`libphy-y`, `mdio-bus-y`, `stubs.o`, `mdio_devres.o`, and Realtek PHY's
`realtek-y`. `KPI_R8169=1` gates these groups together with the MAC driver;
the generated Makefile fails closed if a required PHY, MDIO or Realtek PHY
source is missing. Selected objects receive object-scoped
`CONFIG_PHYLIB`/`CONFIG_MDIO_BUS`/`CONFIG_REALTEK_PHY` defines, plus the same
`LINUXKPI_NET_FRONTEND` API view used by R8169. The default profile does not
compile these groups or define their config symbols.

Added the enabled-profile PM-wakeup and managed-resource APIs reached by this
closure. The LinuxKPI `struct device` carries the `can_wakeup` and
`should_wakeup` state read by upstream's `!CONFIG_PM_SLEEP` helpers and
initializes both bits. `pm_system_wakeup()` and PM callback helpers follow the
selected PM config: the current no-suspend profile uses upstream's disabled
behavior, while a PM-enabled profile gets declarations/callbacks instead of
unconditional no-op macros. The frontend netdevice has an initialized
embedded device and `devm_alloc_etherdev_mqs()` ties its cleanup to parent
devres. `dmam_alloc_coherent()` allocates through the existing mask-checked
coherent DMA path and records a devres action that frees the same size,
address and attributes after driver removal.

Added a narrow R8169 netdevice feature/ops ABI and software statistics to the
LinuxKPI frontend, plus the linear skb fields used by its TX/RX routines. The
frontend's reconstructed transmit skbs have no page fragments, checksum
requests, VLAN metadata or GSO state. The pristine imported Realtek driver
source remains unchanged. The overlay reports no hardware offloads until the
common frontend can carry their metadata. RX/TX byte and packet counters use
atomic per-device counters; the driver-facing stats conversion copies the
legacy counters into Linux's `rtnl_link_stats64` layout. Netdev address
validation/setters, TX queue reset, device attach/detach state, and a TX
quiescence barrier now have LinuxKPI implementations. R8169 MAC fallback uses
the existing hardware-RNG-first identifier-byte service and marks the address
locally administered; this is identifier generation, not a cryptographic
randomness guarantee. Linear TX buffers reserve enough tailroom for the driver's
`ETH_ZLEN` padding path. No page-fragment or offload behavior is established.

The workqueue overlay now supports disable depth and synchronous cancellation:
queue submission is serialized against disable, pending work is removed, and
the sync form waits out an active callback after preventing it from requeueing.
The software IRQ-coalescing helper records Linux's conservative default values;
the NAPI backend does not yet enforce those timers, so no coalescing behavior is
claimed.

## Build evidence

- The generated Kbuild report includes 3 R8169, 10 PHYLIB, 2 MDIO-bus and 1
  Realtek PHY object (`/tmp/r3-kbuild-report.log`). A default dry run contains
  no R8169/PHY recipes; an enabled dry run shows the PHY source using scoped
  config symbols and `LINUXKPI_NET_FRONTEND` (`/tmp/r3-default-dryrun.log`,
  `/tmp/r3-enabled-dryrun.log`).
- Ran actual enabled recipes for the R8169 and representative PHYLIB, MDIO and
  Realtek PHY objects. R8169 objects set `KBUILD_MODNAME="r8169"`; every
  selected networking object uses the same explicit `net_device`/`sk_buff`
  overlay as the frontend. The enabled recipes remain compile failures; output
  is in `/tmp/r3-family-post-kbuild.log` and `/tmp/r3-phy-objects.log`.
- The enabled `r8169_firmware.c.o` recipe compiles. The latest focused
  `r8169_main.c.o` recipe has no direct driver-source diagnostics but fails in
  included upstream headers (`/tmp/r3-r8169-lkpi-final2.log`). Remaining
  blockers include Linux task stack and compat types, `kernel_cap_t` and task
  credentials, seqcount-latch and nofault helpers, netlink/skb clone and
  destructor state, `net_device.ingress_queue`/net namespace, VLAN notifier
  APIs, and page/netmem dependencies. This is not a successful driver object.
- The focused default `net_frontend.c.o` compile passes
  (`/tmp/r3-front-stats.log`). A full default `make -C kernel -j8` completed
  and linked after the latest LinuxKPI changes
  (`/tmp/r3-lkpi-default-poststats.log`). The previous QEMU TCG
  regression `/tmp/avory-r2-runtime/r3-managed.log` passed native RTL8139 DHCP,
  the R2 byte-level bridge fixture and DRM card registration, but predates the
  latest LinuxKPI header/queue changes and should not be treated as a fresh
  runtime result.
- Fresh QEMU TCG regression `/tmp/avory-r2-runtime/r3-managed.log` passed:
  native RTL8139 obtained DHCP at line 300, the R2 bridge fixture reached
  `[R2 NET] PASS` at line 332, and `/dev/dri/card0` registered at line 218.
  This validates the default-off profile and bridge fixture, not enabled R8169
  compilation or hardware traffic.

## Link closure and initcall order (2026-11-14, later session)

- A forced rebuild of all 16 gated objects compiled cleanly
  (`/tmp/r3-gated-objects.log`). The first gated link left five undefined
  symbols (`/tmp/r3-gated-link-undef.log`): `vprintk` (from `dev_err_probe()`
  and PHYLIB's `phy_attached_print()`), `fwnode_handle_get`,
  `fwnode_get_named_child_node` (MDIO bus registration/scan) and
  `device_property_read_bool` (RTL8211F probe).
- Added `linuxkpi/src/property.c` with the upstream
  `drivers/base/property.c` bodies of `fwnode_property_present/read_bool`,
  `device_property_present/read_bool`, `fwnode_get_named_child_node` and
  `fwnode_handle_get`. They dispatch through `fwnode_operations` (including
  the secondary node) exactly as Linux does. AvoryOS PCI devices currently have
  no firmware node, so the R8169/MDIO lookups see no properties and no `mdio`
  child, which is the ordinary PC behavior. Added a weak `vprintk()` beside
  `_printk()` that strips the Linux level prefix and writes to klog.
  The gated kernel then linked (`/tmp/r3-gated-link.log`).
- The first gated QEMU boot exposed an ordering bug:
  `Generic Clause 45 PHY: Error -19 in registering driver` (line 250 of
  `/tmp/avory-r2-runtime/r3-gated-default.log`). The generator emitted libphy
  before mdio-bus and r8169 before the Realtek PHY, so `phy_init` registered
  drivers on an unregistered `mdio_bus_type`, failed, and unwound the generic
  PHY drivers and stubs. r8169's level-6 initcall would also have probed
  before `phy_module_init`, so `r8169_mdio_register()` would find no dedicated
  PHY driver and fail with `-EUNATCH`. `kbuild-subset.py` now emits
  mdio_bus, phylib, realtek_phy, then r8169, mirroring upstream's
  `drivers/net/phy/Makefile` and `phy/`-before-`ethernet/` link order.
  The regenerated `Makefile.kbuild` differs only in order. `nm` confirms
  `mdio_bus_init` → `phy_init` (level 4) and `phy_module_init` →
  `rtl8169_pci_driver_init` (level 6).

## QEMU evidence (gated kernel, TCG)

QEMU has no RTL8168 model, so these boots validate the gated build and its
effect on other devices, not r8169 probe or traffic.

- `/tmp/avory-r2-runtime/r3-gated-order.log` (`kpi_r2_bridge=1`, selectors
  off): all 18 LinuxKPI initcalls return without error, native RTL8139
  obtained DHCP (line 304), `[R2 NET] PASS` (line 336), `/dev/dri/card0`
  (line 218).
- `/tmp/avory-r2-runtime/r3-gated-sel.log` (`kpi_r8169=1
  kpi_r8169_bdf=0000:00:05.0`, RTL8139 at `addr=0x5`): the selected BDF does
  not match r8169's ID table, native RTL8139 kept ownership and obtained DHCP
  (line 304), `/dev/dri/card0` (line 218).
- The default-profile (`KPI_R8169=0`) kernel was rebuilt after these changes;
  see `docs/linuxkpi-progress.md` for the result.

## Hardware evidence (VFIO passthrough, KVM)

Setup: host RTL8168 `0000:07:00.0` bound to `vfio-pci` (persistent
`/etc/modprobe.d/vfio.conf`: `options vfio-pci ids=10ec:8168` with
`softdep r8169 pre: vfio-pci`); host networking moved to USB tethering. Guest:
`qemu-system-x86_64 -accel kvm -cpu host -machine q35 -m 6144 -nic none
-device vfio-pci,host=0000:07:00.0,addr=0x5`, gated `KPI_R8169=1` kernel,
command line `kpi_r8169=1 kpi_r8169_bdf=0000:00:05.0` (guest BDF). The guest
has 6 GiB, so RAM spans above 4 GiB; the driver selected its normal DMA mask,
and no forced 32-bit-mask run was made. Boot script:
`/tmp/avory-r3-hw/boot-vfio.sh`.

- **Missing firmware and link** (`/tmp/avory-r3-hw/hw1.log`, normal
  `disk.img` without the blob): probe line 277, `RTL8168h/8111h,
  9c:6b:00:51:06:79, XID 541, IRQ 105` line 279, `Unable to load firmware
  rtl_nic/rtl8168h-2.fw (-2)` line 305 (non-fatal, as upstream), PHY attach
  line 307, `Link is Up - 100Mbps/Full` line 317, DHCP lease line 323.
- **Failed probe, cleanup, re-probe, firmware** (`/tmp/avory-r3-hw/hw3.log`,
  reflink copy `build/r3-disk.img` with the pinned blob added, plus
  `kpi_r8169_probe_fail_test=1`): first probe reached MDIO registration (line
  277), injected `register_netdev` failure `-5` (279), unwind check `[  OK  ]
  … maps=0/0 IRQ=0/0 netdevs=0/0 enables=0/0 vectors=0 unbound=1` (281),
  same-boot retry (282) probed and registered `eth0` (285–287), firmware
  `loaded 976 bytes` (313), PHY attached (314), link up 100 Mbps/Full (324),
  DHCP lease (330).
- **Bug found by the failed-probe case** (`/tmp/avory-r3-hw/hw2.log`, line
  288): the retry failed with `Can't allocate interrupt` / `-16`. LinuxKPI's
  `pci_alloc_irq_vectors()` never registered upstream's `pcim_msi_release`
  devres action, so vectors of a `pcim_enable_device()` device outlived a
  failed probe. `pci.c` now mirrors `pcim_setup_msi_release()` (managed
  devices get a devres action that calls `pci_free_irq_vectors()`), and the
  cleanup check counts allocated vectors and `is_msi_managed`, so this leak
  can no longer pass as clean.

The test hook is default-off: `kpi_r8169_probe_fail_test=1` arms a one-shot
`register_netdev` failure for the selected r8169 probe, runs the generic
failed-probe cleanup check (shared with nouveau), then probes the device once
more.

Regression after the MSI fix (default profile, QEMU TCG): native RTL8139 and
E1000E obtained DHCP and registered `/dev/dri/card0`
(`/tmp/avory-r2-runtime/r3-final-*.log`). The R2 fixture's timed "second RX
batch accepted before teardown" check failed in those two boots and passed in
two reruns (`r3-final-rerun1.log`, `r3-final-rerun2.log`), so it is
intermittent under TCG rather than a deterministic regression.

Open notes for R4: link negotiated 100 Mbps/Full on every boot, with and
without firmware. Host Linux's speed on the same cable and port was not
recorded, so check the router port and cable before measuring throughput.
After restoring the NIC to Linux, `ethtool enp7s0` gives the comparison.

## Hardware session runbook (reproduce)

r8169 is built and binds by default, so a normal build is enough. Either
pass the RTL8168 through VFIO (`make run-vfio VFIO_BDF=0000:07:00.0
VFIO_GUEST_ADDRESS=addr=05.0 VFIO_ROM= VFIO_LAUNCHER= VFIO_EXTRA='-nic none'`)
or boot bare metal. The image must contain
`/lib/firmware/rtl_nic/rtl8168h-2.fw`; the normal `disk.img` gains it on its
next rebuild because the firmware manifest is a disk dependency. Add
`kpi_r8169_probe_fail_test=1` for the failed-probe case. Expected serial
markers: the `XID 541` chip line, `firmware 'rtl_nic/rtl8168h-2.fw' loaded`,
`attached PHY driver`, `Link is Up`, and for the fault case
`[  OK  ] LinuxKPI: r8169 failed-probe cleanup` followed by a second probe.

Native AvoryOS has no RTL8168 driver, so no other driver competes for the
device. Without the two command-line selectors the device stays unbound.

## Remaining work (moves to R4)

R3 is closed. R4 covers sustained TCP/UDP traffic, TX saturation, cable
pull/replug and link recovery, stop/rebind under load, counter and resource
balance across cycles, and RTL8125. Offload, page-fragment and IRQ-coalescing
behavior remain disabled as described above.
