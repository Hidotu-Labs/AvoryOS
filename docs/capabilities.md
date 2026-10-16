# Process capabilities and private networking

AvoryOS keeps effective, permitted, inheritable, bounding and ambient capability
sets per task, through capability version 1/2/3 capget/capset and capability
prctl operations. Fork and clone copy these sets. capset cannot add permitted
capabilities or effective capabilities absent from the permitted set. Dropping
bounding capabilities prevents root's subsequent exec from restoring them;
no_new_privs also prevents exec from acquiring additional permitted privileges.
Securebits locks and ambient subset constraints are enforced. UID transitions
remove capabilities according to keepcaps and no-setuid-fixup settings.

Namespace creation, mounts, chroot, credential changes, signal permission
exceptions, scheduler privilege exceptions, seccomp installation without
no_new_privs, and reboot use capability checks. Filesystem DAC and ownership
exceptions also check the corresponding capability instead of treating every
UID 0 process as privileged. These checks allow bubblewrap to drop root's
privileges while retaining the UID needed for the sandbox's ordinary files.
File capability xattrs and user namespace capability mappings are not implemented.

CLONE_NEWNET creates a reference-counted private network context. Each socket
retains the network context where it was created, including after descriptor
inheritance, namespace changes and accept. Abstract Unix socket names and
netlink port IDs, user delivery and kernel broadcasts are isolated. Filesystem
Unix sockets resolve through the caller's mount/root view. Existing sockets
intentionally remain usable in their original network namespace.

Private contexts expose a separate `lo` interface, initially down without an
IPv4 address. Route netlink supports setting its 127.0.0.1/8 address and UP state,
including the acknowledgement transactions bubblewrap uses. Interface ioctls
and route dumps expose only that local interface. Private proc network entries
do not disclose global devices or transport endpoints.

Private IPv4/IPv6 transport is currently disconnected: data and endpoint
operations return ENETUNREACH, including loopback traffic. These sockets never
allocate endpoints in the initial network namespace's transport tables. There
is no private TCP/UDP packet stack, physical device assignment or veth support.
This supports a browser subprocess that performs networking through separately
sandboxed broker processes and intentionally prevents direct IP communication.

Run `python3 scripts/tests/test_capability_namespace_boot.py` for an isolated
four-CPU VM check of capability drops across exec, ambient constraints, failed
privilege regain, abstract Unix isolation, loopback setup and rejected external
IP traffic. The test uses private disk and boot images.
