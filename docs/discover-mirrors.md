# Discover: no mirrors available

Discover showed an update/installation error and KPatience reported "No more
mirrors available". Both the staged rootfs and the actual disk image had zero
active `Server` entries in `/etc/pacman.d/mirrorlist`. The packaged list ships
with all entries commented out. PackageKit includes `/etc/pacman.conf` through
`/etc/PackageKit/alpm.d/pacman.conf`, so the build-only pacman configuration does
not configure Discover downloads.

`setup-arch.sh` now seeds the guest mirror list with the Arch geo and Fastly
HTTPS mirrors when no active server exists. Existing configured lists are
preserved. Package signatures remain required in the guest.

## Applied repair, 2026-10-15

- Updated the staged rootfs and the existing `disk.img` after QEMU exited.
- Kept a full pre-repair image at `build/discover-repair/disk-before.img`.
- Reused the mirror file's existing extent; changed its contents and size
  without allocating blocks or changing the installed package database.
- Independently read back the repaired partition and verified the mirror file
  byte for byte against the staged rootfs.
- Both mirror database endpoints returned HTTPS 200 from the host; the geo
  mirror also passed with the guest CA bundle.
- Discover, PackageKit, packagekit-qt6, pacman, CA certificates, and the Arch
  keyring were already installed. The image has a populated package keyring.
- Shell syntax and the fallback behavior passed checks: empty list repaired,
  repeat invocation unchanged, existing custom server preserved.

The guest has not yet been booted to retry a Discover transaction. Start it
normally and retry KPatience. Refresh Discover metadata if it is stale.

## Separate filesystem finding

Read-only `e2fsck -fn` detected pre-existing inode block-count, bitmap, and free
count inconsistencies. Full output is in `build/discover-repair/fsck-before.log`.
The output after the mirror repair (`fsck-after.log`) is byte-identical. This
repair does not resolve or attribute those filesystem issues. The original
image backup preserves the state before this work.

## Follow-up download diagnosis

After the mirror fix, Discover reported "Package download failed". Host HEAD
requests for the package URLs from the shipped sync databases returned 404 for
`kpat-26.08.1-1` and `libkdegames-26.08.1-1`; both solver dependencies returned 200.
The staged core/extra databases date from 2026-10-04. Refresh the guest repository
metadata through `pkcon refresh force`, then retry Discover.

A guest `pacman -Sw --debug kpat` additionally failed before download with
"Landlock is not supported by the kernel" and "switching to sandbox user alpm
failed". The serial log independently recorded missing syscall 444
(`landlock_create_ruleset`) from pacman. `setup-arch.sh` now sets
`DisableSandboxFilesystem` in the guest options section. This disables only the
unimplemented Landlock layer and preserves signature verification and the
configured download user. The staged pacman 7.1.0 supports this option.

PackageKit 1.4.0's alpm backend ignores the DownloadUser and sandbox directives;
its stale-URL failure must be addressed by refreshing its metadata separately.
See the upstream [backend configuration parser](https://github.com/PackageKit/PackageKit/blob/v1.4.0/backends/alpm/pk-alpm-config.c)
and [pacman configuration documentation](https://man.archlinux.org/man/pacman.conf.5).

The Landlock configuration change is now applied to `disk.img`, the staged
rootfs, and the build script. After QEMU exited, the existing disk was backed
up to `build/discover-repair/disk-before-landlock.img` and patched offline.
Both pacman and PackageKit now have fresh core/extra sync databases. The gzip
archives were losslessly recompressed and padded with zero bytes to their old
file sizes; libalpm and Python tarfile both parsed the results. Existing data
extents were reused without allocating new blocks. The abandoned, empty
PackageKit transaction lock was removed.

KPatience and libkdegames now resolve to 26.08.2-1. All four resolved package
URLs returned HTTPS 200. The configuration, mirror list, and both pairs of
sync databases were independently read back from the patched disk and matched
the prepared partition. Package signature checking remains required. The
filesystem check changed only by the expected removal of one stale lock inode;
all pre-existing findings remain. A guest installation still needs verification
on the next boot.

## Package extraction and account hook metadata fixes

The later Zed/Git installation downloaded and verified packages, but symlink
ownership changes failed and systemd-sysusers could not copy account-file
permissions. `sys_lchown` previously delegated to `sys_chown`, following the
final symlink; setting ownership before the target was extracted could fail.
`sys_fchownat` also ignored AT_SYMLINK_NOFOLLOW and AT_EMPTY_PATH, and
`sys_fchmodat2` treated an empty path as a missing name. systemd's
`fchmod_and_chown_with_fallback` uses empty names with O_PATH descriptors.

The metadata syscalls now share a resolver that pins descriptor references,
respects final-component nofollow, and handles empty paths against descriptors.
The ownership helpers normalize 32-bit uid/gid -1 sentinels. Unknown flags are
rejected, and non-owner permission checks remain enforced. chmod on a symlink
still returns EOPNOTSUPP, as on Linux.

`linkat` also compared signed directory descriptors as 64-bit values. A caller
passing AT_FDCWD zero-extended from a 32-bit int could get EBADF for relative
archive hard links. These comparisons now use the signed 32-bit argument.
This change does not redesign the kernel's pre-existing copy-based link
implementation.

Validation: `make -C kernel -j4` succeeded, and
`python3 scripts/tests/test_metadata_syscalls_boot.py` passed in a private
four-vCPU KVM boot. It exercises dangling symlink ownership, target isolation,
relative dirfds, O_PATH and unlinked-file metadata, 32-bit uid sentinels,
zero-extended AT_FDCWD in linkat, invalid inputs, and non-owner rejection.
The test does not open disk.img or assign a host GPU.

The tested kernel was packaged into `avoryos-x86_64.iso` and the ISO replaced
atomically, preserving the running VM's existing open ISO. The embedded kernel
was read back and compared with the build; the boot configuration was preserved
byte for byte. The previous ISO is saved at
`build/discover-repair/iso-before-metadata.iso`. The running VM needs a new
QEMU launch to load the new ISO. Package installation should then be retried;
already-extracted packages may need reinstalling to repair their metadata and
rerun failed hooks. An actual package reinstall remains unverified.

Arch's Zed package installs `/usr/bin/zeditor`, so the command `zed` being
absent is expected. See the [Arch package file list](https://archlinux.org/packages/extra/x86_64/zed/files/).
