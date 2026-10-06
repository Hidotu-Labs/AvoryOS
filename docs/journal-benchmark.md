# Measuring filesystem journal workloads

Build the static AvoryOS executable:

```sh
make userland/test_journal_speed.elf
```

The normal `disk.img` build installs it as `/bin/test_journal_speed`. For an
existing installation, copy the executable into the guest using your normal
file transfer workflow. Run it **inside AvoryOS** on a writable journaled
ext3/ext4 filesystem:

```sh
mkdir -p /home/avory/journal-tests
test_journal_speed /home/avory/journal-tests 500 1 5 > /home/avory/journal-1w.csv
test_journal_speed /home/avory/journal-tests 500 4 5 > /home/avory/journal-4w.csv
```

Arguments are the existing parent directory, files per worker, workers, and
rounds. Defaults after the directory are `500 1 5`; limits are 100,000 files
per worker, 32 workers, and 100 rounds. Start small on a disposable guest
image. The test creates a unique private directory each round, uses one
shared directory with distinct worker filenames, and removes only its own
files. Interrupted runs print the private directory path on stderr so that
leftover files can be located. Peak payload is approximately
`files × workers × 4096` bytes, plus filesystem metadata.

## What is measured

Each round runs these phases on the same file set:

1. `create_empty`: exclusive file creation and close.
2. `write_4k`: open, write 4 KiB into each empty file, and close.
3. `overwrite_4k_fsync`: open, overwrite 4 KiB with a different pattern,
   fsync, and close. This tests synchronous completion without allocating
   additional file data blocks.
4. `rename`: rename every file within the shared directory.
5. `unlink`: delete every renamed file.

Workers start through a common gate. Throughput includes worker completion,
fsync of each file after the create/write phases, and a final directory
fsync for every phase. It excludes thread creation, setup, verification, and
cleanup. File writeback can overlap other workers' operations. The drain
column reports the longest worker's drain time, not a sum across workers;
directory fsync time is reported separately.

The CSV provides round, phase, worker count, files per worker, operation
count, total milliseconds, operations/second, and nearest-rank p50/p95/p99
operation latencies in microseconds. Latencies cover the named operation
(including open/close for file phases), excluding deferred drain time.
Throughput covers that drain. Lines starting with `#` contain configuration,
median summaries, and the final success indicator. Progress/errors go to
stderr. Exit code 0 means all operations, file-content checks, and cleanup
succeeded; 1 indicates a runtime failure and 2 invalid arguments.

After write, overwrite, and rename, the program reopens every file, checks
the full deterministic pattern, and checks EOF. These checks are outside the
timed regions. They also warm the cache before subsequent phases, so keep
the phase order fixed when comparing results.

## Locating drain time

Use the optional drain profile to separate reopen/path lookup, file fsync,
and close costs after creation and first writes:

```sh
test_journal_speed --drain-profile /home/avory 500 1 5
test_journal_speed --drain-profile /home/avory 500 4 5
```

The normal CSV columns stay unchanged. Additional `# drain_profile` lines
report each worker's call count, sum of elapsed syscall times in milliseconds,
and worst individual call in microseconds for `openat`, `fsync`, and `close`.
The reports print after the timed phase, including partial counts if it fails.
Elapsed syscall time includes time waiting for locks, scheduling, and I/O;
it is not CPU time. Concurrent workers' totals overlap and must not be added
to estimate phase completion time. Formatting names and the extra clock
reads also occupy drain time, so the syscall totals need not equal the drain
column exactly.

This flag adds clock reads around every drain syscall and is for attribution.
Use the ordinary command without the flag for before/after speed claims;
without it, the drain path keeps its existing clock-read count. The descriptor
check and drain profile are separate modes.

## Bounded writes for plain ext2 inodes

The plain block-pointer write path now acquires journal ownership before
reading its inode and keeps block allocation, pointer updates, and the final
inode update within one transaction for at most 32 touched data blocks.
Larger writes commit progress, reacquire ownership, and reread the inode so
another writer's intervening changes are retained. For four newly allocated
1 KiB data blocks, the modeled commit count falls from five to one. This is
a transaction count, not a measured throughput multiplier.

The default disk-image recipe enables extents, and new files on such mounts
use the existing ext4 extent write path. The plain-inode batching result must
not be presented as a reduction in commits for those benchmark files.

Partial new blocks remain zero-filled. A user-copy fault during an existing
full-block overwrite preserves the original uncopied suffix; a zero-byte
fault after allocating a sparse block still initializes that block. Start,
mapping, block read/write, inode write, and commit failures return errors and
release ownership. User-copy faults and allocation exhaustion can return a
completed short write. The current journal has no rollback of earlier home
writes or reservations, so an I/O error does not promise that nothing changed.

The actual-source host harness checks commit counts, 33-block batch boundaries,
unaligned zeroing, user-copy failures, error-path ownership release, and
concurrent writers updating the same inode:

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/src/include -I kernel/linuxkpi/include \
  -I kernel/limine-protocol/include scripts/tests/test_ext2_write_batch.c \
  -Wl,--gc-sections -pthread -o /tmp/test_ext2_write_batch
ASAN_OPTIONS=detect_leaks=0 /tmp/test_ext2_write_batch
```

## Contiguous extent allocation

The default extent-backed write path now reserves a contiguous run with
one bitmap image, instead of reserving and journaling each block separately.
A four-block run therefore submits its bitmap once instead of four times.
With the existing immediate journal/home writes, that removes six block
writes for the bitmap alone; it does not remove the four data writes or
the transaction's other metadata/commit work. Goal locality remains a hint;
the old allocate/free/jump-ahead retry is replaced by a single run search.

Runs stop at occupied bits and group/filesystem boundaries. Short runs are
inserted as short extents, and the caller can allocate the remainder later.
Group/superblock counters are adjusted once per run, and every reserved
block's cache entry is invalidated with its generation advanced. The legacy
single-block API uses the same allocator with a run limit of one.

The host harness includes both real ext2 block code and the real ext4 extent
allocator. It checks four-block reservation/extent mapping and accounting,
96 unique blocks from four concurrent allocators, fragmentation, a shortened
last group, wrapped goals, stale counters, cache collisions, and error paths.
It also exercises checked direct/single/double/triple-indirect mappings used
by plain writes. Failed reads are distinguished from holes, and pointer-block
updates propagate errors and participate in the owning transaction. Existing
home-write/partial-reservation rollback limitations remain.

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/linuxkpi/include -I kernel/limine-protocol/include \
  scripts/tests/test_ext2_alloc_run.c -Wl,--gc-sections -pthread \
  -o /tmp/test_ext2_alloc_run
ASAN_OPTIONS=detect_leaks=0 /tmp/test_ext2_alloc_run
```

Guest measurements for this pass are recorded below. Run both ordinary
configurations for comparison, then use the drain-profile mode to attribute
growing drain costs. Against the original
one-worker baseline, the 10× targets are 2335.7 creates/s and 1927.2 first
4 KiB writes/s. A reduction in commits or bitmap submissions alone does not
establish either target or a whole-system 10× improvement.

User-reported contiguous-allocation pass: 500 files per worker, five rounds;
both ordinary runs passed content checks and cleanup.

| Phase | 1 worker files/s | Change vs second pass | Gain vs original | 4 workers files/s | Change vs second pass | Gain vs original |
|---|---:|---:|---:|---:|---:|---:|
| create_empty | 1028.68 | -1.7% | 4.40× | 591.76 | +23.0% | 3.86× |
| write_4k | 970.59 | +91.3% | 5.04× | 836.37 | +108.2% | 4.56× |
| overwrite_4k_fsync | 1801.07 | -5.2% | 1.15× | 1547.48 | -2.6% | 0.97× |
| rename | 2041.15 | -4.9% | 1.37× | 1802.28 | +13.9% | 1.08× |
| unlink | 2157.92 | -6.2% | 1.07× | 1724.52 | +3.3% | 1.22× |

First-write throughput nearly doubles relative to the immediately previous
pass, but 10× has not been reached. Single-worker creation still drains up to
200.068 ms; four-worker creation up to 1615.235 ms. Four-worker first-write
p99 ranges from 17459.682 to 23033.675 us, versus 33940.507 to 43328.128 us
in the second-pass run. These observations do not isolate device/guest state
from the code effect; synchronous overwrite throughput does not improve.

The accompanying profile log used `--drain-profile /home/avory 4 5`:
four files per worker, five workers, and the default five rounds, rather than
the intended 500 files/four workers/five rounds. It passes but never recreates
the large create-drain tail (maximum 0.012 ms). Its fast fsync/open/close
figures cannot explain the tail in the larger workload. Repeat with:

```sh
test_journal_speed --drain-profile /home/avory 500 4 5
```

That small profile run also logs a 154 ms TLB shootdown acknowledgment wait
on CPU 0 while `net-worker` is running, around a 162 ms rename phase. This
shows a separate kernel latency event; it does not prove the source of the
large-run drain or establish that every long syscall is a journal stall.

## Comparing before and after

Keep the same image contents/free space, device, QEMU cache mode, guest CPU
count, compiler options, and parameters. Record the kernel revision and
local changes alongside each CSV. Use a quiet guest and at least five
rounds. Increase the file count if a phase lasts only a few milliseconds or
the guest clock resolution causes many zero-latency samples.

For each phase, divide the new median operations/second by the old median
operations/second. For example, 3,000 versus 1,000 operations/second is a
3× improvement. Compare p99 latency and total completion time as well.
Repeat with 1, 2, 4, and 8 workers to expose contention; worker count changes
total file count, so compare the same configuration before and after.
For a fixed-total scaling experiment, decrease files per worker as the
worker count increases.

The filesystem magic is printed; `0xef53` identifies the ext family but does
not establish whether journaling is enabled. Confirm the intended root or
test mount and the successful journal initialization message in kernel logs.
Avoid tmpfs/ramfs for the baseline. An optional tmpfs run is a syscall/cache
control, and must be labeled separately. The benchmark does not clear caches;
label these results as the fixed-order repeated workload, and use a fresh
guest/image for separate cold-start experiments.

These are journal-heavy **filesystem** measurements, including VFS/cache,
allocator, scheduler, and device costs. They cannot alone prove the journal
is the cause of desktop slowness or report journal-internal commit latency.
Kernel transaction/I/O counters from the performance plan are the next step
for attribution. In the currently inspected kernel, fsync only calls cache
writeback and does not explicitly wait for durable journal/device completion.
Successful fsync and content verification therefore do not prove power-loss
durability; compare equivalent kernel semantics and keep crash testing
separate. Do not count deferred work as a speedup when those semantics change.

## Host smoke checks

For development, the same source can run on Linux:

```sh
cc -O2 -Wall -Wextra -Werror -pthread userland/test_journal_speed.c -o /tmp/test_journal_speed
/tmp/test_journal_speed /tmp 12 4 2
```

Host results validate the harness and cleanup, not AvoryOS journal performance.

## Diagnosing concurrent descriptor failures

Version 2 prints every failed worker's phase, operation, filename, file
descriptor, directory descriptor, and numeric errno. It also checks directory
access from each newly started worker. Failed phases produce no performance
row; do not use an incomplete run as an accepted baseline.

To separate descriptor-table/thread problems from file allocation and writes:

```sh
test_journal_speed --fd-check /home/avory 5000 4 1
test_journal_speed /home/avory 50 4 1
```

The descriptor check prepares one empty file per worker before measurement,
then repeatedly opens it, checks its file type with fstat, and closes it.
It uses the same shared pthread descriptor table and inherited directory fd
as the main benchmark. No files are created or written in its timed loop.
Its operation count means open/fstat/close cycles, and the CSV's
`files_per_worker` field denotes iterations for this mode. It still exercises
path lookup and scheduler behavior; passing it does not rule out every race.
Capture stderr as well as stdout to preserve diagnostics. Both modes require
rebuilding and installing the updated executable in the guest.

## Cache lifetime regression check

Concurrent cache eviction formerly allowed unlocked lookup to dereference a
node after its last reference was dropped. A sequence check after tryref did
not prevent access to freed memory. Dentry and full-path cache lookups now
acquire the caller reference while holding the eviction bucket lock.

The host regression harness includes the real VFS cache implementation,
substitutes host atomic locks for privileged IRQ locks, and forces four
threads to compete on one bucket. It exercises 120,000 inserts/lookups for
each cache, checks that acquired nodes remain live, and counts allocations
and frees after retiring entries. It does not simulate the scheduler, disk,
or interrupt behavior.

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/linuxkpi/include -I kernel/limine-protocol/include \
  scripts/tests/test_vfs_cache_lifetime.c -Wl,--gc-sections -pthread \
  -o /tmp/test_vfs_cache_lifetime
ASAN_OPTIONS=detect_leaks=0 /tmp/test_vfs_cache_lifetime
```

LeakSanitizer is disabled because it cannot run under the sandbox's ptrace
environment; the harness independently asserts matching node allocation and
free counts. AddressSanitizer and UndefinedBehaviorSanitizer remain active.
Reinstating the previous optimistic dentry lookup in a disposable source copy
reproduced an AddressSanitizer heap-use-after-free in `vfs_node_tryref()`.

Rebuild and boot the updated **kernel**, then rerun the guest benchmark.
If write fails, capture `[FD-WRITE]` diagnostics: `missing` identifies failed
descriptor pinning, while `backend EBADF` includes node type, inode, live
marker, reference count, and write callback. Error logging is capped at eight
messages per category per boot and adds no output on successful writes.

The shared ext2/ext4 rename backend also now owns a transaction before source
lookup and holds it across adding the new name and removing the old name.
Previously concurrent journaled renames could overwrite each other's
directory-block images. VFS invalidates both names after successful rename
as well as before it, to retire entries refilled during backend I/O. The host
cache harness models such refills and checks that stale positive and negative
entries are gone after return. This change uses the existing journal lock;
it does not add serialization to mounts where journaling is disabled.

## Inode-block cache coherence regression

A read miss can capture an old disk image, overlap a metadata write, and
finish after a colliding block evicts the newer cache entry. Checking only
whether the cache still holds the requested block misses that ordering and
can reinstall stale inode/bitmap contents. Cache slots now track a write
sequence and outstanding writes under `cache_lock`. A miss waits if its slot
has an outstanding write; on completion it uses a matching current cached
image or retries if a write/invalidation crossed the read. Failed writes
invalidate the speculative cached image. Journal home writes use the same
tracked publication path instead of publishing a second untracked image.

The deterministic fake-device test holds an old read until a newer write and
a collision complete. It checks the returned contents, then checks failed
write invalidation. The previous refill logic fails its content assertion;
the fixed logic passes with AddressSanitizer/UndefinedBehaviorSanitizer.

```sh
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/linuxkpi/include -I kernel/limine-protocol/include \
  scripts/tests/test_ext2_cache_coherence.c -Wl,--gc-sections -pthread \
  -o /tmp/test_ext2_cache_coherence
ASAN_OPTIONS=detect_leaks=0 /tmp/test_ext2_cache_coherence
```

Inode lookup now rejects zero-type inode images rather than constructing an
unwritable phantom node, and prints up to eight `[EXT2-INODE]` diagnostics per
boot. The fixes do not repair metadata already corrupted by an earlier run.
The full guest workload is still required to verify these fixes address the
observed failure rather than another independent metadata bug.

## Shared inode-table update regression

Post-create `open()` sets ownership through `vfs_chown()` after the create
transaction has ended. Historically `ext2_chown_impl()` and
`ext2_write_inode()` did not own a transaction. Their full inode-table block
read/modify/write could erase a neighboring file initialized by concurrent
creation, even when each individual block read was coherent.

`ext2_write_inode()` now starts a transaction before reading the shared table
block and releases it on success or error; existing transaction owners nest
normally. Chown and chmod also own the transaction across their initial inode
read. This uses the current journaled-mount serialization, and does not
provide exclusion for mounts whose journal feature is disabled.

The fake-device harness above also runs 8,000 updates from four workers to
four inodes sharing one block, checks every final on-disk image, tests nesting,
and checks invalid-group/write/start failures release ownership. Its mock
journal supplies owner exclusion; it does not test the journal disk format.
A disposable reproduction without the new wrapper, coordinating competing
RMW snapshots, loses neighboring inode updates. Guest timing may increase
because previously unjournaled ownership changes now commit correctly.

## First performance pass

Allocator changes now mark their group-descriptor block in a bounded list.
Flush writes each distinct dirty descriptor block once plus the filesystem
superblock, rather than writing the entire descriptor table. Directory-count
changes use the same mechanism. More than 64 dirty descriptor blocks falls
back to the existing full-table write; failed flushes retain dirty state for
retry. On a modeled 128-block table, updating one group reduces the counter
flush from 129 journal-block submissions to 2. This is a request-count result,
not a measured guest throughput multiplier.

Journal stop skips descriptor/commit/superblock I/O for an empty transaction.
Chown avoids the inode write when UID, GID, and the second-resolution ctime
already equal the requested result; it still acquires transaction ownership
and reads the inode. Changed ownership or a changed ctime retains the write.
These optimizations preserve the existing commit path for nonempty updates;
they do not implement deferred checkpointing or change the durability contract.

The host tests `test_ext2_metadata_writeback.c` and
`test_ext3_empty_commit.c` use the same compiler flags and include paths as
the coherence test above. Compile each to a separate executable and run with
`ASAN_OPTIONS=detect_leaks=0`. They check sparse/duplicate marks, overflow,
write/allocation failure retry, marks arriving during a flush, final-block
padding, and empty/nested/nonempty transaction completion.

Compare the updated guest against these pre-optimization median baselines:

| Phase | 1 worker (files/s) | 4 workers (files/s) |
|---|---:|---:|
| create_empty | 233.57 | 153.26 |
| write_4k | 192.72 | 183.42 |
| overwrite_4k_fsync | 1566.46 | 1594.29 |
| rename | 1495.16 | 1666.42 |
| unlink | 2015.60 | 1413.18 |

These are user-reported guest results with 500 files per worker and five
rounds, after the concurrency fixes. Rebuild and boot the updated kernel and
run the same two commands before attributing a speedup to this pass.

User-reported results after the first performance pass, with the same
500-files-per-worker/five-round parameters; both runs passed content checks
and cleanup:

| Phase | 1 worker (files/s) | Gain vs baseline | 4 workers (files/s) | Gain vs baseline |
|---|---:|---:|---:|---:|
| create_empty | 1110.01 | 4.75× | 573.03 | 3.74× |
| write_4k | 476.47 | 2.47× | 438.02 | 2.39× |
| overwrite_4k_fsync | 1812.61 | 1.16× | 1639.39 | 1.03× |
| rename | 1805.34 | 1.21× | 1675.81 | 1.01× |
| unlink | 2173.74 | 1.08× | 1779.91 | 1.26× |

These are filesystem-workload gains, not journal-internal or whole-desktop
speedups. Four-worker creation still exhibits a large drain tail: the longest
worker drain reaches about 1.5 seconds in later rounds. First-write throughput
does not improve with four workers. Before a further structural change,
profile transaction wait/hold time, metadata submissions per unique block,
and the per-file drain fsync calls. Attribute improvements against this
post-pass baseline as well as the original baseline; preserve the same
durability semantics when comparing.

## Second performance pass: journal mapping and scratch reuse

Normal transactions now reuse commit/superblock scratch buffers under the
existing mount transaction lock. A bounded 64-slot cache stores physical
mappings for the journal inode, whose allocation is fixed during the mount.
Each slot retains its logical key; collisions remap rather than returning
another block's address. Recovery continues to resolve mappings directly.
Journal contents are not cached here: stop still rereads the superblock.
Retained storage is two filesystem blocks plus 512 bytes of mapping arrays
per mount, in addition to the existing descriptor buffer.

The host journal test observes zero allocations and zero mapping resolutions
for a warmed one-block transaction. Previously each such commit allocated
two scratch buffers and resolved four journal mappings. The same six device
writes and one superblock read still occur in the same order, with checked
payloads, descriptor tags, and sequence numbers. This removes CPU/heap and
mapping overhead; it does not reduce payload/home writes, implement group
commit, or resolve the measured create-phase drain tail. User-reported guest
results for this pass are recorded below.

Missing/out-of-range journal mappings, scratch allocation failures, payload
and home write failures, descriptor/commit/superblock write failures, and the
superblock read failure now return an error instead of crashing or silently
completing the transaction. Payload/home failures latch until outer stop;
nested helpers cannot erase that failure. Every outer exit releases ownership,
and sequence advancement occurs only after a successful final write. This
does not add device flush ordering or fix existing durability/recovery gaps;
callers that ignore journal return values still require a separate audit.

The expanded `scripts/tests/test_ext3_empty_commit.c` checks all these paths,
mapping collisions, four concurrent nested writers, and full descriptors at
2 KiB and 4 KiB block sizes. It uses the host sanitizer command above. The
metadata, block-cache/inode-coherence, and VFS lifetime suites also pass.

Rebuild and boot the updated kernel, then repeat:

```sh
test_journal_speed /home/avory 500 1 5
test_journal_speed /home/avory 500 4 5
```

Compare against the first-pass table above, including per-operation tails
and drain time. Accept only complete runs with content checks and cleanup.

Second-pass guest results: 500 files per worker, five rounds; both the
one-worker and four-worker runs passed content checks and cleanup. The full
log supplied later repeats the same one-worker results, so it is not an
independent repeat of that configuration.

| Phase | Median files/s | Change vs first pass | Gain vs original baseline |
|---|---:|---:|---:|
| create_empty | 1046.62 | -5.7% | 4.48× |
| write_4k | 507.43 | +6.5% | 2.63× |
| overwrite_4k_fsync | 1900.75 | +4.9% | 1.21× |
| rename | 2145.34 | +18.8% | 1.43× |
| unlink | 2301.22 | +5.9% | 1.14× |

These comparisons describe the observed runs, not statistically established
effects of the changes. Creation's per-operation p50 is about 0.83 ms across
all rounds, while drain grows from 0.430 to 162.092 ms. Its median total time
is 477.727 ms versus 450.445 ms in the first pass. The drain tail persists;
repeat both configurations under the same guest conditions before deciding
whether the creation regression is reproducible.

| Phase | 4-worker median files/s | Change vs first pass | Gain vs original baseline |
|---|---:|---:|---:|
| create_empty | 481.07 | -16.0% | 3.14× |
| write_4k | 401.72 | -8.3% | 2.19× |
| overwrite_4k_fsync | 1588.63 | -3.1% | 1.00× |
| rename | 1582.61 | -5.6% | 0.95× |
| unlink | 1669.42 | -6.2% | 1.18× |

Four-worker creation drain is already 1224.736 ms in round 1 and reaches
1814.152 ms in round 5, versus 0.650 and 1472.883 ms in the first-pass run.
Creation p50 also rises from 3805.976 to 4919.883 us, and its round-5 p99
is 20889.539 us versus 9566.343 us in the earlier run. First-write p99
reaches 43328.128 us versus 36498.961 us. The regression includes operation
latency as well as drain time.

The second pass is not an established concurrent performance improvement.
The logs do not distinguish a change-induced regression from accumulated
guest/cache state, device variation, or other contention. Before further
structural optimization, collect transaction wait/hold and drain-path timings
and perform an A/B comparison from equivalent fresh guest/image states.
Keep the I/O error fixes distinct when comparing mapping/buffer reuse so
performance experiments do not discard error reporting.
