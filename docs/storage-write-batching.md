# Ext4 data-write batching and storage measurements

The extent-backed kernel write path now submits complete contiguous data blocks
in runs of at most 32 filesystem blocks (32 KiB at 1 KiB blocks, 128 KiB at
4 KiB blocks). Runs stop at extent boundaries, the end of the supplied buffer,
and the remaining transaction budget. Partial first/last blocks retain the
existing read-modify-write/zero-fill path. User memory is still staged before
journal ownership. Metadata, commit ordering, and durability policy are unchanged.

`ext2_write_blocks()` publishes every member to the block cache with in-flight
tracking and write generations before the device request. Completion advances
every generation and releases every in-flight count; failed requests invalidate
all affected speculative cache images. A run is bounded to avoid wrapping over
the same cache slot twice. A failed multi-block request can have changed some
device blocks: like the previous path, an I/O error does not imply rollback.

The sanitizer harness verifies 40 contiguous blocks use two data requests,
32 and 8 blocks, instead of 40 individual data requests. This is a **95% reduction
in data-request submissions for that fixture**, not a measured throughput or
Roblox join improvement. Small writes and fragmented extents gain less. The
NVMe driver may split runs according to transfer limits or perform partial-LBA
read-modify-write, so actual command counts must be measured separately.

## Pipelined block I/O batches

Batching runs removes submission calls but still waited for each device request
individually, so one submitter kept only one NVMe command in flight. The block
layer now has an optional pipelined API: `struct block_io_req` carries a
member's LBA, sector count, buffer, and result, and a driver should fill one
entry per member on a `read_batch`/`write_batch` call. Members must be
independent and disjoint; completions may be observed out of order. When a
device does not implement the slots, `block_read_batch()`/`block_write_batch()`
run the same members through `read_sectors`/`write_sectors` in order, and
partition devices split batches at partition bounds, translate LBAs, and
forward each valid run to the whole-disk batch call. So callers (ext3/ext4
commit, ext2 write paths) can batch unconditionally and still get the old
ordered single-request loop on any other device.

The NVMe implementation arrives with `NVME_BATCH_IO=1`.
`nvme_batch_submit()` walks the batch: a *fast* member (whole logical blocks,
kernel buffer, within the transfer cap) joins a wave of up to
`NVME_BATCH_WAVE_MAX` (16) commands, all submitted back to back before
`nvme_io_wait_many()` reaps the wave against one shared deadline and one
per-queue batch wake queue. Members that need the full path, CID-table
refills, and transport retries fall back to the existing `nvme_io()` while no
batched command is outstanding. `nvme_io_range()` chunking inside one large
request uses the same wave submission. Per-member results are 0, -1 (I/O or
pre-submit), or -2 (transport), and every member is reaped before the call
returns.

Journal commit payload and home-checkpoint writes are gathered the same way:
`ext3_commit_io_batch()`/`ext3_commit_home_batch()` submit up to
`EXT3_COMMIT_WRITE_BATCH` (16) staged blocks and wait once. The ordering
barriers are untouched: each batch is fully waited before the next commit step
starts (payload→descriptor→commit→super-publish→home→super-clear), the
superblock read-modify-write and FUA/flush semantics are unchanged, and a
failed home batch still retires the log while reporting its first failed block
(in write-set order) exactly as before. The knob gates only NVMe batch-op
registration; ext3 uses the generic helper unconditionally, so a kernel built
without the knob runs the same commit, just one request at a time.

## Counters

Read `/proc/storage_perf` inside AvoryOS after booting the rebuilt kernel.
It contains cumulative, read-only counters with no per-I/O logging:

- `io`: existing successful logical device requests and 512-byte sector counts.
  Whole disks and partitions both account forwarded requests; do not add them.
- `hw_reads`, `hw_writes`, `hw_read_bytes`, `hw_write_bytes`: actual submitted
  NVMe data commands and their requested bytes, including retries and failed
  completions. These belong to the whole namespace only, not its partitions.
  `hw_errors` counts nonzero data-command wait results. These measure host
  commands, not internal SSD NAND traffic. Pre-submission failures are excluded.
- `nvme_flush`: samples for submitted NVMe flush commands, including retries.
  Latency is elapsed time in the command wait, including scheduling delay;
  failures before submission are excluded. Flush errors are separate from
  `hw_errors`. Hardware/flush fields on non-NVMe devices are uninstrumented zeros.
- `journal_wait`: acquisition time for outer transaction starts, including
  uncontended starts and interrupted acquisition attempts. Nested starts do
  not add samples. Errors count interrupted waits.
- `journal_hold`: outer transaction elapsed time through finish, including
  operation, data I/O and commit. Errors count failed finishes and dead-owner
  aborts. This is not commit-only latency.

Latency lines contain count, total elapsed milliseconds, maximum milliseconds,
error count, and eight histogram buckets: `<1`, `[1,2)`, `[2,4)`, `[4,8)`,
`[8,16)`, `[16,32)`, `[32,64)`, and `>=64` ms. Millisecond resolution deliberately
keeps collection inexpensive; sub-millisecond requests accumulate in bucket 0.
Only finished attempts enter these latency samples. Existing LOCKDIAG owner
and hold-time output remains useful for transactions still in progress.

Counter fields are independent atomic reads, not a globally consistent
snapshot. Read in a quiet guest for exact finite-workload comparisons. Elapsed
sums can overlap across threads; do not add them to derive wall-clock runtime.
For a test interval subtract counts, totals, bytes, errors and histogram buckets.
`max_ms` is a lifetime maximum and cannot be subtracted to get interval maxima.

## Compare in the guest

Use the same boot configuration, filesystem contents, free space, CPU count,
QEMU cache mode, and game. Save baseline results with the previous kernel on a
cloned image; this change does not edit the existing disk image. New counters
cannot retrospectively provide old-kernel hardware measurements.

For an existing guest benchmark directory:

```sh
cat /proc/storage_perf > /tmp/storage-before.txt
test_journal_speed /home/avory/journal-tests 500 1 5
cat /proc/storage_perf > /tmp/storage-after.txt
```

Repeat with four workers. The existing 4 KiB phases mostly test small writes;
use large sequential writes as a separate workload to assess data batching.
Capture before/after snapshots around a Roblox join too, recording elapsed
seconds from Join to a playable scene. Run five repetitions and compare medians,
with cold and warm asset caches recorded separately. A throughput improvement
is `(new_rate / old_rate - 1) * 100`; shorter join time is
`(old_seconds - new_seconds) / old_seconds * 100`.

## Verification

Actual-source ASan/UBSan host harnesses:

```sh
python3 scripts/tests/test_ext4_sparse_write.py
python3 scripts/tests/test_ext2_data_run.py
python3 scripts/tests/test_storage_perf.py
python3 scripts/tests/test_nvme_wait_registration.py
python3 scripts/tests/test_journal_usercopy.py
python3 scripts/tests/test_procfs_statfs.py
```

The existing C `test_ext2_cache_coherence.c` and `test_ext3_empty_commit.c`
harnesses additionally verify a cache collision across a batched write and
journal latency/nesting/error accounting; the other ext2 harnesses mock the
batched write path. `test_nvme_wait_registration.py` now covers the batch wait
as well: every CID is reaped, per-member verdicts are kept, one deadline and
one batch wake queue are shared. Kernel and production ISO builds pass in both
knob configurations. These host tests do not establish guest performance,
real-device recovery, or a Roblox join-time percentage; those measurements
remain pending.
