# R4 — Wired data qualification on RTL8168h

Date: 2026-11-14. Linux source: `v6.18.42`,
`856a9b51680cbbed1ace1207b29e424a616cff48`.

## Status

**R4 partial exit met on RTL8168h (2026-11-14); RTL8125 deferred.**
Physical `RTL8168h/8111h` (`10ec:8168` rev `0x15`, XID 541) passed DHCP, ARP,
UDP DNS, HTTP/HTTPS TCP, sustained media streaming (YouTube), physical cable
pull/replug link recovery, and dual-NIC `net_default=` coexistence with
emulated `e1000`. Physical RTL8125 qualification is deferred because no RTL8125
hardware is present in the host inventory.

## Hardware and firmware

- Device: onboard Realtek `RTL8168h/8111h` (`0000:07:00.0` host ->
  `0000:00:05.0` VFIO guest), MAC `9c:6b:00:51:06:79`, XID `541`, IRQ `110`.
- Firmware: `rtl_nic/rtl8168h-2.fw` (976 bytes).
- Negotiated link: `100Mbps/Full - flow control off`.

## TCP window initialization fix

During R4 bring-up, ARP, DHCP, and UDP DNS worked on `RTL8168h`, while TCP
completed the 3-way handshake (`SYN` -> `SYN+ACK` -> `ACK`) and then stalled
with 0-byte `ACK` probes (`curl` timed out with 0 bytes received).

- **Root cause (`kernel/src/net/tcp.c`)**: `tcp_active_open()` and
  `tcp_active_open6()` initialized `snd_wl1 = 0`, `snd_wl2 = 0`, and
  `snd_wnd = 0`. In `input_locked()`, the peer send window was only updated
  when `seq_gt(seq, t->snd_wl1)` (`(int32_t)(seq - 0) > 0`). Any remote peer
  with an initial sequence number `>= 0x80000000` failed `seq_gt(seq, 0)` on
  `SYN+ACK` and subsequent segments, leaving `t->snd_wnd == 0` permanently so
  `tcp_output()` never transmitted queued payload.
- **Fix**:
  - In `input_locked()`, skip the `snd_wl1`/`snd_wl2` gate while in
    `TCP_SYN_SENT`, and unconditionally initialize `snd_wnd = raw_wnd`,
    `snd_wl1 = seq`, and `snd_wl2 = ack` when a valid `SYN+ACK` arrives
    (RFC 793).
  - In `tcp_input_ipv4()` and `tcp_input_ipv6()` passive open (`TCP_LISTEN`),
    initialize `child->snd_wnd = raw_wnd`, `child->snd_wl1 = seq`, and
    `child->snd_wl2 = child->snd_una` on the incoming `SYN`.

## Verified traffic and dual-NIC checks

1. **UDP, ARP, and DHCP** (`make run-vfio KPI_R8169=1 VFIO_BDF=0000:07:00.0 ...`):
   - `[DHCP] Lease applied: 0xC0A801A5/0xFFFFFF00 gw=0xC0A80101 lease=86400s`
   - `busybox nslookup google.com 192.168.1.1` resolved A and AAAA records.
2. **TCP HTTP/HTTPS and sustained traffic**:
   - `curl google.com` returned `301 Moved` HTML response.
   - `curl https://google.com/` completed TLS handshake and returned `301 Moved`
     HTML response.
   - Sustained HTTPS/media streaming verified with YouTube playback.
3. **Dual-NIC coexistence (`RTL8168h` VFIO + emulated `e1000`)**:
   - Command:
     ```sh
     make run-vfio KPI_R8169=1 VFIO_BDF=0000:07:00.0 VFIO_GUEST_ADDRESS=addr=05.0 \
       VFIO_ROM= VFIO_LAUNCHER= \
       VFIO_EXTRA='-nic none -device e1000,netdev=net0 -netdev user,id=net0' \
       KERNEL_CMDLINE='kpi_r8169=1 kpi_r8169_bdf=0000:00:05.0'
     ```
   - With default selection (`eth0` = `RTL8168h`), `curl` succeeded over the
     physical NIC.
   - With `net_default=eth1`, `e1000` took the default route (`10.0.2.x` user-mode
     slirp) and `curl` succeeded over `e1000`.
   - Note: `/proc/net/dev` and `/sys/class/net` currently expose only the active
     `default` interface (`net_device_default_acquire()`), so secondary
     registered interfaces are not listed there.
4. **Physical cable pull/replug link recovery**:
   - Before unplug: `curl -I http://google.com/` returned `301 Moved Permanently`;
     `/proc/net/dev` showed `eth0: rx 7464 bytes / 82 pkts, errs 0, drop 0`.
   - Two unplug/replug cycles logged cleanly:
     `(eth0) Link is Down` (`PHY state change RUNNING -> NOLINK`) followed by
     `(eth0) Link is Up - 100Mbps/Full - flow control off`
     (`PHY state change NOLINK -> RUNNING`).
   - After replug: `curl -I http://google.com` returned `301 Moved Permanently`;
     `/proc/net/dev` showed `eth0: rx 15539 bytes / 200 pkts, errs 0, drop 0`.

## Deferred / known limits

- **RTL8125 (2.5 GbE)**: Deferred until physical RTL8125 hardware is available.
