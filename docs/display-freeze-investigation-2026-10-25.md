# Remaining desktop freeze

The user reproduced a whole-screen freeze after the software vblank wait fix.
Audio continued; the pointer also stopped. Triggering a hang report was followed
by recovery. This does not establish that reporting itself repaired the stall.

The original complete capture is preserved as
`build/drm-freeze/desktop-freeze-original.log`, with its matching symbol ELF at
`build/drm-freeze/kernel-before-diagnostics.elf`.

The first report starts at uptime 189800 ms. Every CPU answers its probe and
every sampled current task is idle. Tracked graphics locks and the shootdown
lock are free. The journal is handed to Firefox thread 653, which is READY and
queued on CPU 0. Its ready age is 43 ms when sampled during report output;
that alone does not prove scheduler starvation. Only a 152 ms slow shootdown
has been recorded before the dump.

The report focuses on the journal owner's Firefox group and reaches its
128-thread output limit before listing the compositor. A second report starts
while the first is still printing, corrupting serial lines. Late overdue waits
and slow shootdowns are therefore not reliable evidence of the original freeze:
printing holds interrupts masked and holds the thread-list lock for much of
the report.

Diagnostic changes in `lockdiag.c`:

- A nonblocking atomic guard suppresses overlapping reports.
- Compositor process groups and native VirtIO GPU workers print before browser
  workers, including READY display threads and their runqueue/IRQ state.
- GPU completion/error counters print before lengthy thread output.

An isolated KVM guest with 150 browser-like sleeping threads confirms that
`kwin_wayland`, `virtio-present`, and `virtio-gpu` print before the browser group
even when the report reaches its limit. Evidence is in
`build/drm-freeze/diag-serial.log`. The kernel also compiles and links with the
GPU counter addition.

The diagnostic kernel was installed in `avoryos-x86_64.iso`; the prior ISO is
`build/drm-freeze/iso-before-diagnostics.iso`. The extracted replacement kernel
was compared before publication. A new frozen-desktop capture is required to
identify the compositor's actual wait. No remaining-freeze fix is claimed.

## Subsequent capture and extent metadata repair

The next capture (uptime 69474 ms) is saved at
`build/display-storage-probe/freeze-first.log`, with matching symbols in
`build/display-storage-probe/kernel-before.elf`. KWin's main thread is blocked
in `fallocate`. Its sampled kernel stack contains the extent allocation path,
`ext4_extent_write_node`, and NVMe flush/wait functions. These are stack-scan
candidates rather than a reliable unwind. GPU counters at report entry show
29174 submitted and completed commands, zero in flight, and no timeouts or
host errors. Both native GPU workers await work. The desktop did not recover;
the user closed the VM before a second snapshot could establish request progress.

Extent-node updates bypassed journal staging and synchronously wrote and
flushed the device for each update. The stated reason for this bypass was stale
tree reads during a transaction, but `ext2_read_block` already reads staged
journal images before the cache or disk. `ext4_extent_write_node` now stages
metadata during an active transaction, like inode and bitmap updates. Repeated
updates collapse into the final image per block. The unjournaled write/flush
fallback remains. This uses the existing journal commit machinery; it does not
establish new crash-durability guarantees for that machinery.

Verification:

- Kernel compilation/link succeeded.
- The C/shell actual-source harness builds a depth-two tree with 1600 fragmented
  mappings, checks staged reads after every insertion and checkpoint readback,
  and covers failure propagation and the unjournaled fallback. ASan/UBSan pass;
  leak detection is disabled because this environment cannot run LeakSanitizer.
  The prior helper fails the staging/no-home-flush assertion. 1669 updates become
  39 unique staged nodes.
- An isolated eight-vCPU KVM guest allocates a fragmented file, runs concurrent
  writes and a 16 MiB fallocate, and verifies old data and a zero-filled hole.
  Both baseline and repaired kernels pass. Baseline fallocate is 120 ms versus
  124 ms after the change: this workload demonstrates no latency improvement
  and does not reproduce the desktop freeze.
- Read-only e2fsck of the repaired guest's disk reports no corruption; it suggests
  only that the resulting extent tree could be narrower.

The replacement kernel is installed in `avoryos-x86_64.iso`, preserving its
production boot configuration. The extracted kernel was compared with the
intended stripped ELF before publishing. The prior ISO is
`build/display-storage-probe/iso-before-storage.iso`. `disk.img` was not modified
by this repair or attached to the isolated probes. A full desktop retest is
still required: removing this confirmed per-update flush behavior on KWin's
captured path is not proof that the remaining freeze is resolved.
