# Polkit rules directory ownership

The reported openat flags 0x90800 describe a read-only, nonblocking directory
open with O_DIRECTORY and O_CLOEXEC. EACCES here is a real read/search denial,
not an executable-file metadata probe.

The staged Arch rootfs declares in usr/lib/tmpfiles.d/polkit-tmpfiles.conf:

    d /etc/polkit-1/rules.d 0750 root polkitd - -

Its etc/group assigns polkitd GID 102. Package installation runs in a user
namespace, so staged host ownership is flattened to the invoking host user.
The image population script copied modes but omitted this logical ownership,
leaving debugfs-created rules.d as root:root with mode 0750. A daemon running
as polkitd consequently cannot open it. This reproduces the supplied error
with the actual AvoryOS VFS permission function; the booted image's inode
has not been inspected directly.

The rootfs overlay now explicitly writes UID 0, the polkitd GID read from the
target etc/group, and mode 0750 for that directory. Missing/invalid account
information fails before image mutation. Neither host IDs nor a fixed 102
are used. Administrator rule files remain root-owned; polkitd can read/search
the directory but cannot create or replace policies. Permissions enforcement
and SYSDIAG warnings are unchanged.

For the already-running guest, run these commands as root:

```sh
chown root:polkitd /etc/polkit-1/rules.d
chmod 0750 /etc/polkit-1/rules.d
ls -ld /etc/polkit-1/rules.d
```

Restart polkit or reboot afterward so startup rule loading is retried. This
repair applies to the guest filesystem, so changing the kernel ISO alone
will not repair an existing disk. The existing disk.img was not rewritten;
future rootfs population incorporates the ownership repair. The disk.img
make recipe recreates the filesystem, so preserve existing guest data before
using a full image rebuild.

Verification:

```sh
python3 scripts/tests/test_polkit_image_permissions.py
bash -n scripts/populate-ext2-dir.sh
```

The test populates a disposable ext4 image, inspects real inode UID/GID/mode,
checks GIDs 777 and 102 and repeated repairs, and verifies account validation
leaves the image unchanged. It also compiles the actual VFS permission code
under ASan/UBSan: the old ownership denies daemon reads; repaired ownership
allows reads/search, denies daemon writes, and denies unrelated users. Live
polkit startup/rule loading remains to be verified after the guest repair.
