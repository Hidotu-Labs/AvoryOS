# Desktop syscall compatibility audit

The supplied SYSDIAG report contains no unregistered syscalls or unhandled
ioctls. Its syscall-error table records negative results, including normal
feature detection, optional-file searches, and nonblocking I/O. A nonzero
count alone does not establish a kernel defect.

## Changes

- `capget`: unknown-version discovery with NULL data now returns success
  after writing the supported version. Supported v1/v2/v3 headers are not
  rewritten; v1 copies one capability word. Invalid pointers return EFAULT,
  negative PIDs return EINVAL, and nonexistent target PIDs return ESRCH.
  The existing UID-based compatibility model reports defined capabilities
  0 through 40 for root and none for non-root. This does not add capability
  enforcement; `capset` remains the existing no-op.
- `getsockopt`: preserve family errors, especially SO_PEERGROUPS ERANGE and
  its required buffer length. Unix sockets retain supplementary-group
  snapshots rather than reading an unprotected pointer to the peer task.
  Groups remain available after a later setgroups call or peer closure.
- `shmctl`: implement IPC_SET, IPC_INFO, SHM_INFO, SHM_STAT, and SHM_STAT_ANY
  using actual segment state. IPC_STAT now reports the requested byte size,
  creator and owner identities, timestamps, and deferred-removal status.
  IPC_SET changes owner/group/mode while preserving the creator. Permission
  checks protect status and modification; user buffers use fault-safe copies
  outside the segment lock. All allocated pages are resident; swap counters
  are zero because this kernel has no swapping.
- sysfs: invalidate the initial negative lookup after creating `/sys`, so
  boot can mount sysfs even on a filesystem without a preexisting directory.
  The user-namespace enable parameter reports `N`, consistent with clone
  and unshare rejecting user-namespace flags. Standalone UTS namespace support
  is now implemented; see [namespace status](namespaces.md). The source contains
  the CPU online and cpu_capacity nodes reported missing in the attachment;
  the guest regression verifies that they are readable.

## Report entries intentionally retaining errors

| Entry | Reason |
| --- | --- |
| setsid EPERM for PID 1 | The caller is already a process-group leader. |
| rt_sigaction EINVAL for signal 19 | SIGSTOP cannot have a handler. |
| PR_CAPBSET_READ EINVAL for capability 41 | Capability numbers stop at 40; libcap probes the next number. |
| readlink EINVAL on `/proc` or `/usr` | These directories are not symlinks. |
| SO_PEERSEC ENOPROTOOPT | No security-label provider exists; a fabricated label would be misleading. |
| fsopen and other new mount APIs ENOSYS | No fs-context or detached-mount infrastructure exists; callers can use legacy mount. |
| O_TMPFILE EOPNOTSUPP | Anonymous-inode creation and later linking are not implemented; Qt falls back to named files. |
| waitid P_PIDFD EBADF for 0x7fffffff | Invalid-descriptor feature probe. |
| shmctl IPC_RMID EINVAL | Invalid/already-destroyed segment IDs must remain errors. |
| FUTEX_UNLOCK_PI EPERM | Unlocking a PI futex without owning it is not permitted. |
| VIRTGPU_GET_CAPS EINVAL | This DRM backend has no virtio 3D capability sets. |
| DIRTYFB ENOENT | The specified framebuffer ID was not found; the report does not contain its request payload. |
| recv/read EAGAIN, poll/epoll EINTR, futex timeout | Normal event-loop results. |
| ENOENT for optional configuration, cache, loader, or DMI files | Missing files are not resolved by changing syscall results. DMI data needs a real platform-information provider. |

Implementing namespaces, seccomp enforcement, the new mount API, or anonymous
temporary-file linking requires the corresponding kernel mechanisms. Returning
success from their stubs would not implement the requested operation.

## Validation

`make userland/test_syscompat.elf` builds a static regression program.
The disk-image target installs it as `/bin/test_syscompat` on its next rebuild.
Run that command as root in the guest to include socket-group and non-root
permission checks. The program also runs against Linux, omitting root-only
tests when invoked as a regular user.

The regression passed against Linux and in a two-CPU QEMU AvoryOS guest.
The guest used a small temporary ramdisk and a test-only kernel that omits the
unrelated LinuxKPI boot-test suite, which stalled in the initial test boot.
The normal kernel and `avoryos-x86_64.iso` were rebuilt with that suite intact.
This validation does not establish that every desktop application starts.

ABI references:
[Linux capability implementation](https://github.com/torvalds/linux/blob/master/kernel/capability.c),
[Linux socket options](https://github.com/torvalds/linux/blob/master/net/core/sock.c),
[Linux shared-memory UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/shm.h).
