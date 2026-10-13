# Netlink option queries, ACL reads and socket-name output

The supplied first message interleaves SYSDIAG's getsockopt report with normal
Netlink setsockopt traces. Its arguments identify SOL_SOCKET=1 and
SO_KEEPALIVE=9 returning ENOPROTOOPT. The caller's socket family is not in that
capture. The Netlink family previously rejected every getsockopt query; its
getter now returns keepalive and passcred state. Netlink keepalive is socket
option state, not TCP timer behavior. Invalid sizes and user-copy failures
remain errors, and unknown options remain ENOPROTOOPT. The generic setter
now preserves family errors rather than masking them after recognizing a
SOL_SOCKET option; ENOPROTOOPT can still use the existing generic fallback.

Routine multi-part Netlink setsockopt trace lines were removed so they no
longer garble these reports. The existing SO_ATTACH_FILTER stub is unchanged:
this patch does not implement BPF filtering. Membership IDs are checked before
shifting the 32-bit group mask, avoiding undefined shifts for invalid IDs.

KIO's reported system.posix_acl_access read used a 132-byte output buffer.
The previous diagnostic exception covered only NULL/zero size queries.
Unsupported reads of the exact ACL access/default attribute names are now
quietly counted with or without a buffer. Writes and other unsupported
attributes still warn. EOPNOTSUPP remains the result: ACL storage and access
enforcement are not implemented, and successful reads are not fabricated.

Socket-name calls previously passed the caller's address capacity into their
family's kernel-buffer fill, causing Netlink to reject short/zero buffers.
They now supply the full 128-byte kernel capacity, then truncate user output
and report the full address length. Null output with zero requested capacity
still reports the length; nonzero copies to inaccessible output fail EFAULT.
A missing length pointer fails EFAULT. These conventions follow Linux's
move_addr_to_user pattern in net/socket.c.

The name-output path also removes redundant VMA-tree writable predicates and
uses bounded, fault-safe copy_from_user/copy_to_user to check actual memory
access. Such copies can resolve demand and COW faults. Invalid addresses and
copy failures continue returning EFAULT. The supplied ModemManager addresses
are numerically in userspace, but there is no guest memory/length capture;
this does not establish which predicate/copy caused that exact failure.
Successful ModemManager startup remains unverified.

Actual-source ASan/UBSan checks pass:

- scripts/tests/test_socket_name_netlink.py: keepalive set/get, family error
  propagation through the syscall, passcred, unknown options, membership
  bounds, zero/short/full address output, full-length reporting, failed
  copies, negative lengths, and reference cleanup.
- scripts/tests/test_xattr_probe.c: NULL and buffered ACL query classification,
  severity promotion for real failures, writes and unrelated namespaces.
- scripts/tests/test_join_diagnostics.py and test_ipv6_dualstack.c: existing
  join filters, IPv4/IPv6 UDP and mapped TCP acceptance remain intact.

Kernel and avoryos-x86_64.iso builds pass. Boot the rebuilt ISO with the same
disk; no disk rebuild is needed for these kernel changes. If ModemManager's
EFAULT remains, obtain the new report and its socket family, requested length,
and memory mappings before attributing it to another compatibility failure.

Primary reference for socket option state:
https://github.com/torvalds/linux/blob/master/net/core/sock.c
