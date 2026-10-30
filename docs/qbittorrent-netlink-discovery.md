# qBittorrent Netlink discovery failure

The saved guest log at `/home/avory/.local/share/qBittorrent/logs/qbittorrent.log`
reports repeated startup failures on TCP/0 with `Input/output error`, including
PID 693 from the supplied screenshot/log. The icon write-access probe and
systemd-resolved's IP_MTU getsockopt failure are separate from that error.

Libtorrent's `read_nl_sock()` determines the next Netlink packet size using
`recv(fd, NULL, 0, MSG_PEEK | MSG_TRUNC)`. A zero return becomes EIO and aborts
interface/route enumeration:
https://github.com/arvidn/libtorrent/blob/RC_2_0/src/enum_net.cpp

AvoryOS returned zero in sys_recvfrom and the generic socket receive wrappers
before reaching Netlink's existing MSG_TRUNC/MSG_PEEK implementation. The
syscall now reaches descriptor lookup and family dispatch. Generic zero-byte
receives reach AF_NETLINK, while other families retain their previous behavior.
Netlink skips memcpy for an empty copy; the packet stays queued on MSG_PEEK.

The first patched application run then exposed `Address already in use` on
Ethernet UDP: the loopback socket occupied the port globally. TCP and UDP explicit binds
now check local-address overlap. Distinct concrete IPv4 addresses may share a
port; same-address and wildcard overlaps remain EADDRINUSE. TCP listener selection also checks the destination address, and shared-port
cleanup preserves remaining owners. Ephemeral allocation still avoids occupied
ports. Regression coverage verifies Ethernet replies
reach the Ethernet socket, loopback close does not release Ethernet ownership,
and wildcard/same-address collisions remain rejected for TCP and UDP. TCP
conflict checks also cover ports outside the ephemeral bitmap range.

Validation:

- `ASAN_OPTIONS=detect_leaks=0 python3 scripts/tests/test_netlink_zero_recv.py`
  exercises actual syscall, socket dispatch and Netlink receive source, including
  repeated size probes, dequeue, empty nonblocking queue and invalid descriptor.
- `python3 scripts/tests/test_netlink_zero_recv_boot.py` passes in a private KVM
  guest for RTM_GETLINK, RTM_GETADDR and RTM_GETROUTE, with sequence/port-ID checks.
- `test_ipv6_dualstack.c` passes with ASan/UBSan, including the new address-bind
  cases and existing dual-stack UDP and TCP handshake/accept cases. Its mocks
  were updated for existing namespace/usercopy/timer-tree dependencies.
- Kernel and `avoryos-x86_64.iso` rebuild successfully.
- Older test_socket_name_netlink.py and test_netlink_link_types.py cannot compile
  against pre-existing working-tree Netlink changes: their mocks lack newly
  added options and namespace fields. These failures are unrelated to this fix.

The user's installed disk is not edited. Reboot using the rebuilt ISO to load
this kernel. End-to-end torrent payload transfer needs a reachable peer and is
separate from verifying interface discovery.

Host dual-stack regression command:

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/src/include -I kernel/linuxkpi/include \
  -I kernel/limine-protocol/include \
  scripts/tests/test_ipv6_dualstack.c -Wl,--gc-sections \
  -o /tmp/test_qbit_udp
ASAN_OPTIONS=detect_leaks=0 /tmp/test_qbit_udp
```

Final application verification: qBittorrent 5.3.0 runs with a fresh profile and
QT_QPA_PLATFORM=offscreen in a disposable copy of the desktop filesystem. Its
log reports successful TCP and UTP listening on 127.0.0.1, 10.0.2.15, and the
Ethernet IPv6 link-local address, with no `Failed to listen` messages. Captured
output is in `build/qbittorrent-diag/app-final-serial.log`. The diagnostic VM
shuts down afterward; the large disposable filesystem copy is removed.
