# Journal performance plan

Status: in progress; the first two optimization passes have guest measurements
in `journal-benchmark.md`. Contiguous extent allocation, bounded plain-inode
writes, and optional drain profiling are implemented; their guest results
are pending. The 10× target has not been demonstrated.
Date: 2026-10-06.

## Objective and scope

Improve AvoryOS ext3/ext4 journal-bound workloads by 3×, then 4×, with a
10× stretch target for metadata-heavy, concurrent workloads. These are
cumulative targets relative to the same baseline, not multipliers to stack.
They are hypotheses to test, not measured results or guarantees for the
whole desktop. This plan concerns the filesystem journal in
`kernel/src/fs/ext3/ext3.c`, which ext4 also uses; it does not concern
systemd-journald or console logging.

An old journal format alone does not establish the cause of slowness. If
journaling takes fraction p of a workload, accelerating that portion by s
gives total speedup 1 / ((1 - p) + p / s). Even eliminating a journal cost
that accounts for 80% of runtime caps total speedup at 5×. A 10× whole-task
improvement requires more than 90% of the original runtime to be removable.

## Findings in the current working tree

- `ext3_journal_start()` uses one owner and lock per mount. Other writers
  now sleep on a wait queue until the owner finishes the operation and its
  commit. Slow-owner logs expose hold time, phase and the outer call site.
- `ext3_journal_block()` skips full images already present in the coherent
  home cache with no in-flight write, and synchronously writes changed metadata images
  to the journal and its home block. Repeated changes to the same block get
  additional tags and additional writes; there is no transaction write-set.
- Immediate home writes were introduced to prevent stale allocator reads
  after collisions in the 2,048-slot direct-mapped cache. Simply deleting
  these writes would reintroduce corruption.
- `ext3_journal_stop()` writes the descriptor, commit record, and journal
  superblock twice; it also reads the superblock and allocates temporary
  buffers. It always starts at journal block 1 rather than maintaining a
  rotating log with deferred checkpointing.
- `ext4_extent_write_node()` directly writes and flushes an extent node.
  This is a separate expensive path that must join the coherent metadata
  model before its flush can be removed.
- `ext4_write_impl()` already allocates extents and closes transactions in
  batches of up to 32 data blocks. Lazy allocator counters already reduce
  repeated counter updates. Preserve these improvements in the baseline.
- Dirty allocator metadata now tracks dirty group-descriptor blocks, with a
  bounded full-table fallback, and genuinely empty transactions skip commit
  I/O. Journal mapping and scratch buffers are reused. These measured passes
  are recorded separately from the next extent-run/plain-write changes.
- The commit path has no explicit device flush/FUA sequence, ignores several
  I/O errors, and falls back to unjournaled home writes if descriptor tags
  overflow. `sys_fsync()` calls a void cache-sync function and returns success;
  it does not explicitly wait for a journal sequence or device durability.
  These are blockers for safe asynchronous commits.

## Stage 0: establish evidence and a correct durability baseline

1. Confirm the active root filesystem/device and that its journal actually
   initialized. Unsupported journal features currently only cause init to
   return; introduce explicit ready/failed state and reject writable mounts
   whose required journal cannot be used.
2. Add counters and timing histograms for transaction lock wait/hold time,
   unique versus submitted metadata blocks, mapping reads, journal/home
   bytes and requests, descriptor-table writes, flush latency, commit time,
   and checkpoint time. Expose snapshots through procfs; avoid per-block logs.
3. Benchmark small-file create/write/unlink, rename-heavy operations,
   archive extraction, a native compile/link including sparse output,
   sequential overwrite and append, and small writes with fsync. Use 1, 2,
   4, and 8 workers where supported. Record cold and warm runs separately.
4. Run on cloned disposable images, identical hardware and device cache
   settings. Use at least five repetitions and report median throughput,
   p95/p99 operation latency, CPU time, and physical I/O. Include all final
   draining/checkpoint costs in finite-workload measurements.
5. Correct error propagation, allocation failures, log bounds, descriptor
   overflow, recovery validation, and journal superblock placement/buffer
   sizing across supported filesystem block sizes. Reserve credits before
   mutation; never overflow into unjournaled writes.
6. Establish and test a conservative write-ahead protocol: durable recovery
   information and journal payload before durable commit; home metadata
   only after commit; durable checkpoint before retiring/reusing log space.
   Order associated file data before the metadata commit. Audit partition
   forwarding and device flush/FUA guarantees rather than assuming them.
   Make fsync/fdatasync, sync, O_SYNC/O_DSYNC, directory fsync, and unmount
   use explicit durable transaction completion and return writeback errors.

Keep the original performance measurements for context, but also measure
this corrected durable baseline. Subsequent speedups must be reported
against the durable baseline with equivalent semantics. Correctness fixes
may initially increase latency.

## Stage 1: target 3× through less repeated I/O

Implement a bounded transaction map keyed by home block number, holding
owned full-block images. All metadata reads consult it before the ordinary
cache or disk. Pin/version images until checkpoint; transaction snapshots
must survive cache collisions and later writers. Replace repeated updates
to one block with its final image and one journal tag.

Route inode, bitmap, directory, group-descriptor, superblock, and extent-node
updates through this mechanism. Audit direct metadata writes. Journal only
dirty descriptor-table blocks; reuse commit buffers and validated journal
block mappings. Skip genuinely empty transactions. Coalesce journal payload
into contiguous multi-sector requests where physical mappings allow it.

Initially keep one running transaction and synchronous completion to limit
the change. Write each unique home image once after durable commit. Remove
the extent-node flush only after transaction reads guarantee visibility.

Expected benefit: workloads that repeatedly modify allocator and inode-table
blocks. If unique-block counts are already close to submitted-block counts,
deduplication alone will not reach 3×; use the measurements to reprioritize.

Acceptance: at least 3× median throughput on a preselected journal-bound
workload, verified contents and recovery, and no material regression in the
fsync or sequential-I/O control workloads. Investigate any p99 regression
above 10% before accepting the stage.

## Stage 2: target 4× cumulative through group commit and checkpointing

Introduce transaction handles/credits and separate running, committing, and
checkpointing states. Close a transaction only when all joined handles have
finished. Freeze commit images so later updates cannot alter them.

Add a per-mount commit worker and a bounded circular journal with head/tail,
sequence tracking, wraparound, and space reservations. Let ordinary buffered
operations join a shared transaction; start with a tunable 1–5 ms batching
window and byte/credit thresholds, then select settings from latency data.
Fsync requests force the relevant sequence to commit and wait for it.

Checkpoint committed home images in the background and retire log space
only after checkpoint durability. Bound dirty memory and queue depth; apply
backpressure on memory pressure or a full log. Use sleeping wait queues
instead of the yield loop. Keep metadata synchronization explicit; dropping
the current mount lock without replacement locks is unsafe.

Recovery must now validate every transaction before replay, handle multiple
descriptors and circular wrap, and support revoke records or a proven block
reuse restriction so stale committed images cannot overwrite reused blocks.

Acceptance: 4× cumulative throughput on the chosen workload, stable results
after the journal wraps repeatedly, bounded memory, and successful fsync,
unmount, concurrent mutation, and crash-recovery tests.

## Stage 3: target 10× on selected workloads

Use profiles from Stage 2 to choose the next work:

- Fine-grained inode/directory/allocator locking with a documented lock order
  so independent writers can join transactions concurrently.
- Pipelined log submission/checkpoint I/O using an explicit completion API
  and measured queue-depth limits. NVMe already has multiple queues; the
  filesystem/block callback path remains synchronous.
- Larger contiguous allocation and data-I/O batches where append/extraction
  workloads show request overhead. This is filesystem I/O work in addition
  to journal optimization; attribute its contribution separately.
- Better metadata-cache indexing if conflict misses remain significant.

Only consider ext4 fast-commit-style delta logging after full-log correctness
is solid. It requires compatible feature negotiation, idempotent recovery,
and fallback for unsupported operations; it is a separate project rather
than a small switch to turn on. Do not invent private records inside an
otherwise standard ext4 journal.

Acceptance: demonstrate 10× on named metadata/concurrency workloads with
the same durability contract, including drain time. Publish lower gains for
single synchronous writers, sequential I/O, and desktop tasks when measured.
If commit/device latency dominates those workloads, revise the target.

## Validation and delivery order

Deliver separate changes for instrumentation, durability/recovery fixes,
transaction-map coherence, deduplication/batched I/O, circular-log recovery,
group commit/checkpoint workers, and concurrency/pipelining. Keep later
stages behind configuration flags until validated.

For each stage, inject failed writes, failed flushes, allocation failures,
and crashes around log publication, payload, commit, checkpoint, tail
advance, and wraparound. Test cache collisions, tag limits, empty/nested
transactions, multiple writers, directory operations, sparse linker output,
extent growth, block reuse, memory pressure, and all supported block sizes.
Use a backend capable of modeling volatile caches and write reordering;
terminating a VM with a write-through disk alone is insufficient.

After recovery, verify acknowledged fsync data and namespace expectations,
file checksums, bitmap/inode allocation consistency, and absence of duplicate
allocation. Use offline Linux/e2fsprogs inspection on disposable copies for
format compatibility. Re-run the same benchmark suite only after these pass.

## Update (2026-10-15): write-set, multi-descriptor commits and FIFO handoff

Stage 1 prerequisites are implemented and covered by actual-source host tests:
per-transaction write-set staging with dedup (identical restages write once),
commits spanning up to `EXT3_JOURNAL_SEG_MAX` descriptor groups instead of the
unjournaled-home fallback at the 126-tag limit, FIFO ownership handoff without
barging, and validate-before-replay recovery. See
`journal-writeset-fairness.md`. This removes the per-metadata double write and
the unbounded unjournaled fallback; it does not add group commit. Stage 2
(transaction handles/credits, batching window, background commit and
checkpoint workers, circular log with head/tail and wraparound) remains the
next step, and its acceptance criteria are unchanged.

## Update (2026-10-16): pipelined device I/O behind `NVME_BATCH_IO`

First slice of the Stage 3 pipelined-submission item, without adding group
commit or touching Stage 2. The block layer gained an optional
`read_batch`/`write_batch` API (`struct block_io_req`), and the ext3 commit
gathers payload and metadata-home writes into batches of up to 32 blocks so
one submitter waits once per batch instead of once per block. With
`NVME_BATCH_IO=1` (now the default), the NVMe driver submits waves of up to 32
independent commands back to back and reaps them against one shared deadline;
other devices and builds with `NVME_BATCH_IO=0` run the same members through
the old ordered single-request loop. Every batch is fully waited before the
next commit step starts, so the ordering barriers
(payload→descriptor→commit→super-publish→home→super-clear) and FUA/flush
semantics are unchanged. Actual-source host tests cover the batch wait,
per-member verdicts, and the batched commit loops; see
`storage-write-batching.md`. This is a mechanism, not a measured speedup:
guest throughput comparison against the unbatched build still has to be run,
and the knob stays until it is.

## Primary references

- [Linux ext4 journal format and fast commits](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html).
- [Linux journalling API: transactions, commit and checkpoint lifecycle](https://docs.kernel.org/filesystems/journalling.html).

These inform format and lifecycle design; the speedup targets above are
AvoryOS hypotheses based on the inspected implementation, not figures from
the references.
