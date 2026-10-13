# TCP timer starvation audit (2026-10-07)

The reported Mocktail log shows a roughly 60-second HTTP/WebSocket timeout to
10.110.101.222:5052. That log alone does not establish whether this was a guest
network stall, an unreachable destination, or an application/server failure.

The TCP timer has a 24-item work budget and a cursor intended to resume at the
next connection when that budget fills. The no-deadline and SYN branches used
`continue` before the existing budget/cursor check. When delayed ACK or output
work filled the budget on those branches, scanning continued without preserving
the next position. Subsequent ticks could restart at the same busy connections,
indefinitely postponing other connections' ACKs, output, or SYN retransmissions.

`tcp_timer_tick` now checks the budget at the top of every iteration and saves
the next connection before stopping. The existing scan-count bound, TCB
references, work processing outside the lock, and per-connection retry policy
remain in place.

The actual-source host regression uses 72 connections and verifies:

- Three budgeted passes service all 72 continuously ACK-ready connections.
- Busy established connections cannot monopolize timer work ahead of SYN retries.
- An idle scan terminates without emitting packets or retaining references.

The first case fails against the original code. All cases pass with the change
under AddressSanitizer and UndefinedBehaviorSanitizer. Run from the repo root:

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/src/include -I kernel/linuxkpi/include \
  -I kernel/limine-protocol/include \
  scripts/tests/test_tcp_timer_fairness.c -Wl,--gc-sections \
  -o /tmp/test_tcp_timer_fairness
ASAN_OPTIONS=detect_leaks=0 /tmp/test_tcp_timer_fairness
```

The production ISO builds successfully. These checks exercise timer scheduling
with mocked packet output, not the NIC or remote endpoint. Reproducing the
reported timeout in the normal guest and comparing other applications' network
behavior remains necessary to connect this fix to that particular failure.
