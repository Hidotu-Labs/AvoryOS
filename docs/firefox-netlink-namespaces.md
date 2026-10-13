# Firefox Netlink options and namespace probes

The Firefox setsockopt trace uses level 270 (SOL_NETLINK), option 11
(NETLINK_EXT_ACK). Netlink sockets now store this boolean option and expose
it through getsockopt. Short values return EINVAL, faulty user copies return
EFAULT, and unknown options still return ENOPROTOOPT. Enabling extended ACKs
permits optional error metadata; this change does not add or fabricate such
metadata. See Linux net/netlink/af_netlink.c for the option ABI.

Firefox's clone flags 0x10000011 are CLONE_NEWUSER | SIGCHLD. With all other
clone arguments zero, this matches Mozilla's CanCreateUserNamespace probe:
https://github.com/mozilla/gecko-dev/blob/master/security/sandbox/linux/SandboxInfo.cpp
The exact firefox probe returning EPERM remains counted in sysdiag without
an immediate severe warning. Other arguments, callers, and error codes do
not qualify; subsequent real failures promote the aggregate severity.

The bwrap clone flags 0x6e020011 request mount, cgroup, UTS, IPC, PID and
network namespaces. AvoryOS currently rejects namespace creation. Such
failures now explicitly say UNSUPPORTED NAMESPACES and explain that namespace
isolation is not implemented. The rejection and severity remain intact.
This is a diagnostic improvement, not a fix for the failing sandbox launch.
No Firefox sandbox settings or permissions are changed.

Making this bubblewrap invocation succeed requires namespace implementation,
including isolated mount views, namespace lifecycle/inheritance, PID mapping,
network isolation and namespace-aware permission checks. Returning clone
success without this isolation would be incorrect.

Validation: scripts/tests/test_socket_name_netlink.py executes the real socket
handlers and syscall dispatch with ASan/UBSan, checking option state, copies,
lengths and unknown options. scripts/tests/test_xattr_probe.c executes the
real diagnostic recorder, checking the narrow Firefox exception, severity
promotion, and continued bwrap warnings. Guest Firefox startup still needs
runtime verification. Boot the rebuilt ISO with the existing disk; disk
recreation is not needed for these kernel changes.

## Follow-up: strict checks and crashhelper statx

The later Firefox option 12 at SOL_NETLINK is NETLINK_GET_STRICT_CHK. Unlike
NETLINK_EXT_ACK, this promises strict validation of request headers and
attributes. The current route handler interprets only a header and returns
coarse dumps, so storing a boolean and returning success would misrepresent
support. ENOPROTOOPT remains visible, now with the option name and explicit
reason. Implementing this feature requires request validation and correct
Netlink error replies, plus filtering for the supported route dump requests.
Reference: https://github.com/torvalds/linux/blob/master/net/netlink/af_netlink.c

The crashhelper statx call has dirfd 0, NULL pathname, flags 0, mask 0x0fff,
and NULL output. This exactly matches Rust try_statx's availability check:
https://github.com/rust-lang/rust/blob/1.85.0/library/std/src/sys/pal/unix/fs.rs
It expects EFAULT to confirm the syscall exists. SYSDIAG now counts that
exact argument pattern quietly, ignoring the unused sixth register. Other
faults, flags, masks or descriptors still warn and can promote the aggregate.
The statx handler and return values are unchanged. This probe alone does not
explain a Firefox crash; it can occur after another filesystem query fails.

The real-recorder sanitizer test covers each significant argument, errno,
quiet counting, promotion, and the named strict-option warning. Namespace
isolation and strict Netlink validation remain unimplemented; neither is
claimed fixed by these diagnostic changes. Firefox startup needs guest testing.
