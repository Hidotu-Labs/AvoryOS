# R2 — Shared LinuxKPI networking compatibility slice

Date: 2026-11-14. Linux source: `v6.18.42`,
`856a9b51680cbbed1ace1207b29e424a616cff48`.

## Implemented

- Added LinuxKPI split-type overlays for `lockdep_types.h`, `timer_types.h`
  and `workqueue_types.h`. Each uses the Linux 6.18 split header guard and
  exposes the existing LinuxKPI representation, so imported callers and the
  full overlay headers share one type definition regardless of include order.
- Updated `lockdep.h`, `timer.h` and `workqueue.h` to consume those shared
  definitions. The native timer wheel still has its `running` state and
  list-based entry; the workqueue implementation retains its pending/running
  flags, workqueue pointer and list entry. No scheduling or timer behavior was
  changed. Removed the duplicate `delayed_work_timer_fn` declaration.
- Added per-device references under the existing packet queue lock. RX
  admission resolves the fallback interface and snapshots its generation
  under that lock, then takes one reference for each accepted frame. Rejected
  stale submissions drop those references; queued and worker-local packets
  keep them through handler completion.
- Added `net_device_default_acquire()`/`net_device_put()` for brief protocol
  device access, and made unregister close admission/remove publication under
  the same lock, bump generation, drop the lock, and yield until RX and active
  TX references drain. Re-registration is rejected while the drain is in
  progress. IPv4/IPv6 Ethernet paths hold a reference over source-MAC access
  and transmit; ARP uses the same pinned device for the ARP sender MAC and
  Ethernet transmit. IPv4 and IPv6 capture the source MAC before potentially
  blocking neighbor resolution and abort a later send if the default device
  identity changed.
- DHCP snapshots the interface MAC and drops its reference before waiting for
  offers/ACKs. Each DISCOVER/REQUEST reacquires the current default, verifies
  the same MAC, transmits, and releases before retry waits. Lease renewal holds
  a reference only through frame construction and one transmit.
- Registry/default updates and lookups are serialized with the queue lock.
  RX producers must stop/synchronize and keep their device allocation valid
  through the submit call before unregister. Unregister is process-context
  only, must not run in the RX handler, and must not be called while the caller
  itself holds a device reference.
- Added `linuxkpi_bridge.c` and a Linux-facing narrow Ethernet frontend with
  linear `sk_buff` allocation/headroom, `register_netdev()` lifecycle,
  carrier and TX queue state, RX delivery, bounded TX ownership/backpressure,
  NAPI budget/repoll and synchronous disable. Teardown closes admission,
  stops the Linux device, synchronizes deferred work/NAPI and drains native RX
  references before bridge storage is freed. `NETDEV_TX_BUSY` retains the
  native frame and retries only after queue wake; Linux owns an accepted skb.
- Added `kpi_r2_bridge=1`, an isolated software fixture for registration/open,
  byte-exact RX/TX, NAPI budget/missed schedule, BUSY retry, queue capacity,
  worker-held RX teardown, and repeated register/unregister. The fixture is
  separate from the general boot self-test path. It intentionally checks the
  bridge frontend and native backend together, not a Realtek driver.

## Verification

- `make -C kernel -j8` passed with the default profile after the split-header,
  RX/TX lifetime, and reporting snapshot changes. This compiles and links the
  kernel with all Realtek gates disabled.
- Ran `make -C kernel -j8 KPI_R8169=1` to exercise the enabled imported source
  recipes with the production include order. The duplicate definitions of
  `lock_class_key`, `lockdep_map`, `timer_list`, `work_struct` and
  `delayed_work_timer_fn` no longer appear. The build remains blocked by later
  compatibility gaps: `seqcount_spinlock_t`, the Linux `netmem`/page-pool
  layout assertions against AvoryOS `struct page`, and absent page, NUMA,
  timekeeping and task/compat interfaces. Full diagnostics are in
  `/tmp/r2-r8169-compile.log`.
- `git diff --check` passed.
- Focused `net/core.c`, IPv4, IPv6, DHCP, E1000E and kernel boot-path object
  compilation passed after extending references across TX.
- The rebuilt QEMU fixture passed every check in
  `/tmp/avory-r2-runtime/fixture-pass.log` (lines 306–332): registration and
  automatic open, NAPI budget and missed scheduling, all 64-byte RX/TX payload
  comparisons, BUSY retry, the 128-frame queue limit, and repeated lifecycle.
  Its teardown test blocks an RX worker with a live device reference, starts
  unregister on another thread, confirms unregister waits, then releases the
  handler and confirms unregister completes.
- Final default-kernel QEMU regressions passed DHCP and `/dev/dri/card0`
  registration for native RTL8139 and E1000E using
  `/tmp/avory-r2-runtime/native-final-rtl8139.log` and
  `/tmp/avory-r2-runtime/native-final-e1000e.log`. These runs exercise native
  drivers; they do not claim traffic through an imported Realtek driver.
- Focused default-profile frontend, bridge, RX lifetime and fixture objects
  compile successfully (`/tmp/r2-focused-final3.log`).

## Remaining boundaries

- The native reference grace period covers accepted RX frames, worker-local
  batches, and short IPv4/IPv6/ARP/DHCP transmissions after the producer has
  been stopped. Procfs `/proc/net/dev` and `/proc/net/route`, socket interface
  ioctls, and AF_NETLINK route dumps now pin briefly and copy fields before
  formatting, user copies, reply queueing, or wakeups. `netlink_notify_link()`
  keeps its pin only while building the link snapshot, including `link_up`.
  Persistent sysfs callbacks are installed only for persistent native static
  NIC storage; non-persistent endpoints get snapshots while pinned.
- `net_device_find()` remains a borrowed presence check in native E1000E and
  user-driver paths, not a lifetime reference. Runtime lookup consumers beyond
  the audited reporting/protocol paths still need review before wrappers can
  be safely freed after arbitrary unregister.
- TX/neighbor operations use MAC identity checks across waits, but protocol
  configuration and routes remain global. Interface change handling and
  recovery of in-progress operations need a dedicated bridge state design.
- Imported `r8169` and wireless drivers still do not build. The R1 enabled
  `r8169` recipe reaches later compatibility failures including Linux
  `netmem`/page-pool layout assertions against AvoryOS `struct page`, as well
  as missing page, NUMA, timekeeping and task/compat interfaces. These belong
  to R3/R5 and are not papered over by the bridge. Diagnostics are in
  `/tmp/avory-r1/R8169.log` and `/tmp/r2-r8169-compile.log`.

R2's reusable bridge and software-fixture exit is satisfied. It proves the
frontend/backend contract, not `r8169` compilation, hardware binding, or
Realtek traffic.
