# F_DUPFD_QUERY (1027)

`fcntl(fd, F_DUPFD_QUERY, other_fd)` now returns 1 for the same open file
description, 0 for distinct descriptions, and EBADF if either descriptor is
invalid or reserved. Two independent opens of the same VFS node compare as
distinct even if their offsets and flags match. The command and descriptor
arguments use Linux's integer argument conversions.

The comparison follows upstream Linux's `f_dupfd_query()` semantics:
https://github.com/torvalds/linux/blob/master/fs/fcntl.c

Native descriptions have opaque 64-bit identities. New reservations reset the
slot's identity; dup, fork/clone/vfork, SCM_RIGHTS, and pidfd_getfd preserve it.
SCM messages retain the numeric identity after the sender closes or reuses its
slot. Queries validate both slots and compare under the shared file-table lock.
LinuxKPI wrappers use their underlying Linux `struct file` identity, so repeated
exports of one file compare equal even when their native wrappers differ.

Identity storage is private trailing storage in the allocation containing the
file table. The existing fd_table, thread, VFS-node, and Linux file layouts are
unchanged. It is zeroed with the table and freed by the existing final table
release. There are no identity pointers to refcount or free on close. The only
additional per-table storage is MAX_FDS 64-bit IDs (8 KiB at 1024 descriptors).

The three inheritance paths share a locked descriptor snapshot routine. Node
pins and path references protect snapshots against close/reuse; filesystem
callbacks run after releasing the file-table lock. pidfd_getfd publishes identity,
offset and flags before exposing the new descriptor, and sets CLOEXEC.

Validation:

- `python3 scripts/tests/test_fd_duplicate.py`: actual dup/fcntl/close code;
  same-descriptor queries, independent opens of one node, duplicate chains,
  CLOEXEC, fd reuse/replacement, invalid/reserved arguments, integer conversions,
  deterministic close interleavings, and 10,000 queries without diagnostics.
- `python3 scripts/tests/test_fd_description_lifetime.py`: actual table
  allocation/free, inheritance, shared tables, callbacks racing a source close,
  pidfd transfer, reserved descriptors, allocation failure and exhaustion.
- `python3 scripts/tests/test_wine_fd_transport.py`: actual SCM capture/install,
  source close/reuse, metadata, allocation rollback, and queued-reference lifetime.
- `python3 scripts/tests/test_linuxkpi_file_exports.py`: repeated overlapping
  exports compare equal; independently allocated Linux files compare distinct;
  wrapper/file lifetime and cancellation still work.
- `python3 scripts/tests/test_kpi_wait_fastpath.py`: previous AMDGPU cleanup KPF
  regression remains covered.

These host harnesses compile extracted production functions. A boot of the rebuilt
ISO is still required to validate the change in the user's VM and Firefox.
