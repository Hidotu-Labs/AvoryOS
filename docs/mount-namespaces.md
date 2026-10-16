# Mount namespaces and filesystem confinement

`CLONE_NEWNS` and `unshare(CLONE_NEWNS)` copy a task's mount table. Forked
processes and threads otherwise retain the same table. Each namespace has a
reference-counted identity exposed through `/proc/<pid>/ns/mnt`.

Namespace pathname lookup uses path views, separate from filesystem inodes.
A view retains its backing inode, parent path and mount identity. Consequently
bind mounts can have different access restrictions while sharing inode contents,
and `..` follows the mounted path's ancestry. Absolute paths and absolute symlink
targets start at the task root. A root boundary also clamps `..`.

The native mount API supports private/slave propagation, bind and recursive bind,
read-only remounts, mount moves, lazy detach, private tmpfs and devpts instances,
and proc mounts. Recursive binds copy covered submounts with new identities;
`mount_setattr` applies read-only, nosuid, nodev and noexec attributes recursively.
Mounts require `CAP_SYS_ADMIN`; chroot requires `CAP_SYS_CHROOT`. Unsupported
shared propagation, idmapped mounts, overlay filesystems and other mount
attributes are rejected. The modern filesystem construction API (`fsopen`,
`fsmount`, `open_tree` and related calls) remains unsupported.

`chroot` and both bubblewrap pivot-root forms change the actual lookup root.
Pre-opened directory handles retain their existing access, as on Linux; a sandbox
launcher must close handles that it does not intend to pass to its child. Native
`/dev` shortcuts are disabled in confined namespaces, so device opens must pass
through the visible filesystem. Private devpts instances expose only their own
PTYs and their own multiplexor.

Read-only checks cover writes, creation, removal, metadata changes and xattrs.
Writable shared mappings and later mprotect upgrades are denied on read-only
mounts; private writable mappings remain available. Noexec and nodev constrain
execution and device opens. Nosuid prevents exec from applying set-id privileges.
Private tmpfs roots retain their tree while mounts, path handles and mappings
exist, then retire their children and release data pages when the last reference
disappears.

`openat2` validates `open_how` and supports `RESOLVE_IN_ROOT`, `RESOLVE_BENEATH`,
`RESOLVE_NO_MAGICLINKS`, `RESOLVE_NO_SYMLINKS` and `RESOLVE_NO_XDEV` in the actual
component walk. `RESOLVE_CACHED` returns `EAGAIN`. Proc FD magic links resolve the
referenced open object, allowing bubblewrap to mount through retained descriptors
while performing its two pivot-root operations.

Run the private guest regression after building the kernel:

```sh
python3 scripts/tests/test_mount_namespace_boot.py
```

It checks parent/child mount separation, recursive binds and read-only attributes,
fd-relative resolution and escape attempts, mapping restrictions, private PTYs,
and bubblewrap's two pivots including legacy libc mount magic. This fixture uses
its own images under `build/mount-namespace-boot`; it does not modify `disk.img`.
The packaged bubblewrap and Epiphany integration probes provide additional checks
of the real application launch sequence.
