# NetworkManager startup

`setup-arch.sh` installs NetworkManager but its rcS previously did not start it.
The rcS template now starts it after the system D-Bus socket and udev coldplug,
creates its runtime/state directories, avoids a duplicate daemon, and waits
for `LC_ALL=C nmcli --wait 1 -t -f RUNNING general` to report `running` before
marking startup successful. Output goes to `/var/log/NetworkManager.log`.
The staged rootfs rcS is updated as well. This uses the existing BusyBox boot
service manager; it introduces no separate session helper.

Validation: shell syntax checks and an isolated four-vCPU QEMU guest using a
private copy of the existing filesystem. rcS reports NetworkManager startup
`ok`; `nmcli general` and `nmcli device status` answer over the system bus. The
guest powers off normally. Artifacts: `build/networkmanager-boot/`.

This validates the service, not full networking integration. The kernel already
obtains a DHCP lease, but NetworkManager currently lists only unmanaged loopback
in the isolated test. Ethernet discovery/control remains separate work.

The running desktop disk was not edited. Its rcS installation is pending a
normal guest shutdown because QEMU holds `disk.img` open.

After the user shut down the guest, the disk update was installed and read back
from the actual raw image. Backup:
`build/networkmanager-boot/disk-before-networkmanager.img`. The installed script
contains the tested startup block. Nine existing comment lines were removed to
fit the update within the file's five already allocated blocks (5,026 -> 5,094
bytes); all existing commands were retained. Only those data blocks and the
existing inode size changed, through an atomically replaced raw disk clone.
The boot ISO was not modified.

The untouched disk already has filesystem corruption: inode block-count
mismatches and malformed directories, including inode 198969. `e2fsck -f -n`
reports exactly the same findings before and after the final update. No new
filesystem allocation or repair was made. The initial installation attempt
stopped on these errors before touching the real disk; the final installer uses
existing blocks only. Baseline and after-check logs are retained beside the
backup. These errors remain unresolved and are separate from service startup.

## Ethernet discovery correction

The subsequent desktop reports `Unknown` because service startup alone did not
make the interface usable to NetworkManager. Two RTM_NEWLINK ABI errors were
found: Ethernet and loopback hardware types were left zero, and the Ethernet
MTU attribute carried a u16 rather than the required u32. The kernel now reports
ARPHRD_ETHER (1), ARPHRD_LOOPBACK (772), and a 4-byte MTU. The intermediate guest
recognized loopback after fixing hardware types and Ethernet after correcting
the malformed MTU attribute; Ethernet was still unmanaged.

Network sysfs originally exposed attributes directly under `/sys/class/net/eth0`
without a uevent file, interface index or physical-device parenting. RTL8139 now
retains its actual PCI device in net_device. Sysfs creates the network directory
under that PCI device, a class symlink, subsystem/device links, index, carrier
and address-assignment attributes, and a writable uevent file carrying the real
interface/index through the existing kernel uevent broadcaster. The existing
udev daemon performs coldplug; no fabricated udev database is installed.

The final isolated guest reports `eth0 ethernet connected (externally) eth0`;
udevadm resolves `/devices/pci0000:00/0000:00:08.0/net/eth0` and reports
USEC_INITIALIZED. NetworkManager adopts the kernel's existing DHCP configuration;
its global state is `connected (local only)` with connectivity `none`. Internet
connectivity classification and full NetworkManager link/address control are
not established by this test. Carrier/sysfs fields are initial snapshots; link
hotplug, multiple adapters and Wi-Fi are not covered.

`test_netlink_link_types.py` checks actual link-message hardware types, identifiers
and attributes; the original source fails. The malformed 2-byte MTU also fails
its independent 4-byte attribute expectation. Existing sysfs uevent and socket
Netlink regressions pass. The kernel builds and the private guest powers off
normally. The normal desktop ISO kernel exactly matches that tested payload.

After the user stopped QEMU, the tested ISO was installed. Rollback:
`build/networkmanager-boot/iso-before-ethernet-fix.iso`. No additional desktop disk
modification was needed for the kernel update; its existing filesystem errors
remain unchanged. The matching debug ELF, candidate, hashes and guest logs are
retained in `build/networkmanager-boot/`.

## Carrier state and the tray icon

The desktop subsequently displayed eth0 under Connected but an X on the tray.
The isolated guest confirmed the address and default gateway/route were visible;
carrier reporting in RTM_NEWLINK was incomplete. It set IFF_RUNNING but omitted
IFF_LOWER_UP, IFLA_CARRIER and IFLA_OPERSTATE. The link message now obtains the
actual driver's link status and reports consistent up/down flags and u8 carrier
and operational-state attributes. Link-down messages no longer claim RUNNING.

With this kernel, the isolated guest reports `connected full`, eth0 connected
externally, wired carrier on, per-device IPv4 connectivity full, and `nmcli
networking connectivity check` returns full. The active external connection's
GENERAL.DEFAULT remains no; this did not prevent NetworkManager reporting global
full connectivity in the tested version. No NetworkManager policy override or
Plasma icon override was added. The CPU message regression now covers both link
states, carrier/operstate lengths and flags. The normal candidate's kernel is
byte-identical to the private guest payload.

Candidate: `build/networkmanager-boot/carrier-kernel.iso`; matching debug ELF,
checksums and guest capture are retained. Installation is pending normal guest
shutdown because QEMU PID 11546 holds the desktop ISO open. The prepared installer
verifies that the base ISO is unchanged, rejects open descriptors and keeps a
rollback copy. This kernel update does not edit the desktop disk.

After the user confirmed shutdown, the carrier-state candidate was installed
and its on-disk ISO checksum verified. Rollback ISO:
`build/networkmanager-boot/iso-before-carrier-fix.iso`. `disk.img` was unchanged.

## Live transfer rates and external wired tray selection (2026-10-16)

The desktop could still show a tray X with eth0 connected. This is distinct
from carrier reporting: NetworkManager deliberately excludes externally
configured devices from primary-connection selection. Plasma 6.7.5's wired
fallback required an already non-null connection, so it never selected the
external wired connection when the primary was absent. Its VPN exclusion also
used OR, making that exclusion ineffective. The patch in
`scripts/patches/plasma-nm-external-wired.patch` permits an empty initial
selection and preserves an existing VPN/WireGuard selection.

The 0 B/s display had a separate kernel cause: RTM_NEWLINK omitted
IFLA_STATS64. It now returns Linux-layout 64-bit packet, byte, error and drop
counters read afresh on each request. Unsupported counters are zero. This
repairs transfer-rate reporting; it does not establish higher throughput or
change the emulated RTL8139 hardware's link speed.

Validation:

- `test_netlink_link_types.py` checks actual encoded attributes, Linux counter
  offsets, values above 4 GiB, and changes between requests. Existing socket
  Netlink and sysfs uevent checks pass; the kernel builds.
- `test_network_statistics_boot.py` uses a private disk, a GPU-free KVM guest,
  real D-Bus/udev/NetworkManager, and its own loopback-only HTTP server. A
  256 KiB download increases both NetworkManager byte counters. It never opens
  the desktop disk or passes through host hardware.
- The same guest loads the original Plasma QML module and reproduces
  `network-wired-disconnected`, then loads the patched release library and
  obtains `network-wired-activated`. A direct instance of the patched class
  also verifies that the primary connection remains empty. The guest powers
  off normally. This tests the component's icon selection, without a rendered
  desktop screenshot.

Rebuild the Plasma library with `scripts/build-plasma-network-icon.sh` (host
Qt/KDE development libraries and CMake are required). It uses upstream tag
v6.7.5, applies the patch, and writes a stripped library beneath
`build/plasma-network-icon/overlay/usr/lib/`. It does not modify the desktop
or its disk. The optional icon checks in the boot test use
`AVORY_NETWORK_ICON_PROBE`, `AVORY_NETWORK_ICON_MODULE_PROBE`, and
`AVORY_NETWORK_ICON_LIBRARY` to select those artifacts. When the staged rootfs
already has the fix, set `AVORY_NETWORK_ICON_BASELINE_LIBRARY` to the saved
original library for the before/after comparison. The image builder reapplies
the prepared overlay only to plasma-nm 6.7.5-1.

Prepared artifacts are `build/network-fix/network-kernel.iso` and
`build/network-fix/overlay/usr/lib/libplasmanm_internal.so`; checksums, the
upstream source commit, and the base ISO hash are in `build/network-fix/report.json`.
The candidate kernel is byte-identical to the private guest's tested kernel.
Final evidence: `build/networkmanager-boot/final-network-validation.log`.
At preparation time the desktop disk and boot ISO were unchanged. Installation
was deferred until normal desktop VM shutdown and backup; the existing disk's
previously documented filesystem errors remain a separate concern.

The required Xerj refresh was attempted, but the service rejected the retained
`docs/amdgpu-testing.md` entry because its detected content family changed from
`txt-lines` to `txt-prose`. This investigation therefore used bounded current
source reads and exact ripgrep searches instead of relying on a stale index.

After the desktop VM stopped, the normal boot ISO was found to already contain
the byte-identical tested kernel. The compact Plasma library (571,368 bytes)
was installed into the stopped desktop disk, preserving the old file size
(668,336 bytes) with trailing zero padding. Its existing depth-zero extents
used one contiguous physical run separated by logical sparse holes. Those
existing data blocks were compacted into one logical run, with the remaining
holes at EOF; no blocks were allocated and allocation bitmaps were unchanged.
Readback exactly matched the tested ELF plus padding. Filesystem checker
findings before and after matched byte for byte after normalizing image paths.
The complete rollback disk is `build/network-fix/disk-before-network-icon.img`;
`build/network-fix/install.log` and `report.json` record the installed hashes.
The staged rootfs and version-guarded rebuild overlay contain the same library.
The desktop VM remains stopped; the installed icon and counters take effect
on its next boot.
