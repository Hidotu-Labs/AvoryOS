# Demand-allocated network receive backlog

The old receive ring in `kernel/src/net/core.c` had 1,024 slots, of which
1,023 were usable. Once producers filled it, `net_rx_submit_irq` dropped
frames with `[NET] rx queue full!` even when RAM was available. A second
4,096-packet allocation ceiling also restricted growth.

The software backlog is now an intrusive FIFO. Each incoming frame gets a
page-backed buffer on demand; linking an allocated buffer cannot fail because
of a full pointer ring. Existing workers dequeue up to 128 packets per batch.
Both e1000 and RTL8139 use this common receive path. Protocol handlers still
receive the same `struct net_packet`.

Allocation uses PMM pages directly, without heap OOM reclaim from interrupt
context. A buffer includes its FIFO link and frame storage in one 4 KiB page.
After processing, at most 64 buffers remain cached for reuse; excess pages go
back to PMM. The cache size is not a queue capacity.

Growth has a memory-pressure policy rather than a fixed packet count: the
budget is one eighth of free pages plus pages already owned by RX. It is
sampled at initialization and every 1,000 timer ticks by the network timer
thread. PMM free-list statistics are collected outside the RX lock and outside
interrupt context. Admission and all queue/accounting changes are serialized
by the existing IRQ-safe lock. Cached buffers remain reusable without new
allocation. If the sampled budget is reached or PMM allocation fails, the
frame is dropped and the rate-limited message is now
`[NET] rx dropped: memory pressure`. Existing `rx_dropped` and legacy
`queue_full` counters record these admission failures. The sampled budget is
a soft policy; availability can change between samples.

This changes the software receive backlog, not the NIC's finite DMA ring or
socket receive buffers. It does not implement Linux NAPI. Sustained traffic
above the processing rate can still require drops; a growing backlog also
increases latency. Device-generation checks continue to discard stale frames
following unregister/rebind.

## Regression checks

The host test includes the actual `net/core.c` with a mock page allocator and
thread-safe host locks. It checks a 12,000-frame FIFO burst (exceeding both old
limits), frame bytes and lengths, page reclamation, repeated reuse, device
generation checks, allocation failure, changing memory budgets, recovery, and
100,000 frames through two producers and two consumers. Every page is accounted
for, including the small retained cache.

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -pthread \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/src/include -I kernel/linuxkpi/include \
  -I kernel/limine-protocol/include \
  scripts/tests/test_net_rx_queue.c -Wl,--gc-sections -o /tmp/avory-net-rx-test
ASAN_OPTIONS=detect_leaks=0 /tmp/avory-net-rx-test
```

LeakSanitizer is unavailable under the local tracing environment; the test
explicitly checks that all allocated pages are returned after trimming its
cache. AddressSanitizer and UndefinedBehaviorSanitizer remain enabled.

The existing private-disk NIC regression accepts `--accel tcg` for machines
without KVM, in addition to its default KVM mode. Its PID-1 process installs
an explicit SIGCHLD handler to retain child exit statuses instead of invoking
default init auto-reaping between waits. Child waits retry `EINTR` and print
diagnostic status on other failures.

```sh
python3 scripts/tests/test_e1000_probe_boot.py --accel tcg \
  --case e1000 --case rtl8139 --seconds 30
```

To package the current kernel without invoking rootfs or VM-disk rebuild
prerequisites:

```sh
make -o /home/offihito/AvoryOS/build/arch/.built \
  -o disk.img -o nouveau-618-desktop-firmware -j4 avoryos-x86_64.iso
```

Validated on 2026-10-26: host ASan/UBSan test passed; e1000 and RTL8139 each
passed the private QEMU TCG regression with four CPUs and 30 seconds of four
concurrent TCP streams, plus TCP/UDP/ICMP/IPv6 integrity checks. Both reported
zero RX drops and zero RX errors. The e1000 case also verified carrier events,
traffic after cable toggling, and three unbind/rebind cycles. These are bounded
regressions, not a long-duration desktop soak test. The rebuilt ISO's kernel
matches the stripped kernel snapshot used by this NIC test run.
