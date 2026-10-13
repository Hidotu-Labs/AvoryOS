# KIO xattr capability diagnostics

The reported getxattr(191) call has a null output pointer and zero size: it
queries an attribute's length/support, rather than reading its data. The
original log contains pointers, so the attribute name cannot be confirmed
from that line. AvoryOS supports user.* xattrs and returns EOPNOTSUPP for
other namespaces; POSIX ACL storage and enforcement are not implemented.

KDE uses a query of system.posix_acl_access to detect filesystem ACL support.
Its [properties dialog source](https://github.com/KDE/kio/blob/master/src/widgets/kpropertiesdialogbuiltin_p.cpp)
treats success or ENODATA as support. Returning ENODATA or fabricated success
would advertise a facility the kernel does not provide.

SYSDIAG now recognizes only EOPNOTSUPP from getxattr/lgetxattr/fgetxattr with
NULL output, zero size, and the exact system.posix_acl_access or
system.posix_acl_default name as an expected capability probe. It retains the
error in its diagnostic counters without printing a severe warning. Other
attribute failures remain severe. No syscall return value changes.

Names are copied with fault-safe uaccess before locking. Faulted or truncated
names do not qualify. Other named xattr warnings include xattr='...' and
path-based xattr calls capture their pathname. A later severe error promotes
an existing benign entry and emits a warning, even when syscall, errno, and
worker name match the earlier probe's aggregation key.

## Validation

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/src/include -I kernel/linuxkpi/include \
  -I kernel/limine-protocol/include \
  scripts/tests/test_xattr_probe.c -Wl,--gc-sections \
  -o /tmp/test_xattr_probe
ASAN_OPTIONS=detect_leaks=0 /tmp/test_xattr_probe
make -C kernel -j4
```

The test executes the real diagnostic recorder and xattr store with mocked
uaccess, logging, and locks. It checks exact ACL probe classification across
all three get variants, continued counting, severity promotion, name/path
logging, actual reads and writes remaining visible, fault/truncation handling,
unsupported ACL semantics, and the existing user.* store. AddressSanitizer
and UndefinedBehaviorSanitizer pass. LeakSanitizer is disabled because this
execution environment runs under ptrace. The kernel builds and links; the
existing linker warning about an RWX LOAD segment remains.

Boot the rebuilt kernel and repeat the KIO operation. If the original query
was an ACL probe, its severe warning disappears. If it queried another name,
the new diagnostic identifies that name for a targeted implementation. KIO
runtime behavior has not been verified in the guest.
