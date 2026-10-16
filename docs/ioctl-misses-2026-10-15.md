# Desktop and udev ioctl misses

The captured `build/logs/p6-c7-prime.log` repeatedly reports FIONREAD (0x541b)
on inotify descriptors from plasmashell, kded6 and gmenudbusmenupr, reaching more
than one million calls. The queue was readable but had no ioctl handler, so Qt
could not obtain the buffer size needed to drain it.

The inotify handler now returns the sum of serialized event headers and padded
names under the queue lock, then copies the integer out after unlocking. It
returns zero for an empty queue without consuming events. The private inotify
boot test checks query/read agreement, bad user pointers, partial draining,
overflow, and existing namespace/SDDM socket regressions.

Udev also requests PIDFD_GET_INFO (0xc058ff0b). The pidfd handler now implements
the extensible Linux layout, returns current process identifiers and credentials,
and advertises only those fields and supported-mask discovery. It supports the
64-byte original layout, the 88-byte layout, and zero-filled extension space.
It does not advertise cgroup, retained exit or coredump information. Tests cover
identifiers/credentials, versions, untouched older-layout canaries, zero-filled
extension bytes, invalid pointers and unsupported requests.

Other captured requests are FDGETFDCSTAT (0x80280215), CDROM_GET_CAPABILITY
(0x5331), and CDROM_LAST_WRITTEN (0x5395). These are floppy/optical-device probes
against NVMe block nodes. ENOTTY remains correct; diagnostic names now identify
them instead of reporting unknown requests.

ABI references: [Linux inotify implementation](https://github.com/torvalds/linux/blob/master/fs/notify/inotify/inotify_user.c),
[Linux pidfd implementation](https://github.com/torvalds/linux/blob/master/fs/pidfs.c),
and local Linux UAPI pidfd/cdrom/fd headers.

All dedicated ioctl checks, private SDDM greeter startup/connection and real
udev/PipeWire integration passed. The normal boot ISO was replaced while QEMU
was stopped, with its previous version saved as build/ioctl-fixes/iso-before.iso.
No disk.img changes were needed. Logs are under build/ioctl-fixes.
