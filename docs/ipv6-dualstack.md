# IPv6 dual-stack socket compatibility

A KDE Connect diagnostic with `setsockopt(54)`, level `0x29` (41),
option `0x1a` (26), and length 4 identifies `IPV6_V6ONLY`. Previously,
AF_INET6 accepted only the integer 1 and rejected 0 with ENOPROTOOPT.

AF_INET6 now remembers this boolean option, reports it through getsockopt,
and rejects changes after binding with EINVAL. Nonzero integers enable
IPv6-only operation. The existing IPv6-only default remains unchanged;
applications can explicitly select dual-stack operation with 0.

Dual-stack UDP wildcard binds reserve the same port in IPv4 and IPv6.
IPv4 delivery uses the IPv6 socket's existing datagram queue and wakeup
protocol; source addresses are returned as IPv4-mapped IPv6 addresses.
Mapped destinations use the IPv4 UDP transmit path. Closing the IPv6 socket
releases the IPv4 endpoint. Connected sockets filter both receive paths by
peer, and AF_UNSPEC disconnect clears that filter.

Dual-stack TCP wildcard listeners accept IPv4 handshakes through the existing
TCP implementation. Accepted IPv4 connections retain IPv4 transport state
and report mapped local and peer addresses through AF_INET6. Mapped outbound
connections and binds use IPv4 TCP. Implicit listen binds preserve IPv6.

The option's boolean and pre-bind rules follow
[Linux's IPV6_V6ONLY handler](https://github.com/torvalds/linux/blob/master/net/ipv6/ipv6_sockglue.c).
This change does not extend the existing transport implementations' general
address reuse rules or queue capacities.

## Validation

Run from the repository root:

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/src/include -I kernel/linuxkpi/include \
  -I kernel/limine-protocol/include \
  scripts/tests/test_ipv6_dualstack.c -Wl,--gc-sections \
  -o /tmp/test_ipv6_dualstack
ASAN_OPTIONS=detect_leaks=0 /tmp/test_ipv6_dualstack
make -C kernel -j4
```

The host regression executes the actual socket, UDP and TCP code with mocked
locks, scheduling, and network output. It checks boolean/short/late option
values, mapped datagram receive and reply, native IPv6 delivery on the same
socket, connected-peer filtering and disconnect, port conflicts and release,
IPv6-only isolation, an IPv4 TCP handshake and mapped accept, mapped TCP
connect, and implicit IPv6 listen. The existing TCP timer fairness regression
also passes. Kernel compilation and linking succeed.

A guest check remains: boot an image containing the rebuilt kernel, start
kdeconnectd, and verify the option error disappears and discovery/pairing works
with the intended IPv4 and IPv6 peers. Host tests do not exercise the NIC,
Qt, or KDE Connect itself.
