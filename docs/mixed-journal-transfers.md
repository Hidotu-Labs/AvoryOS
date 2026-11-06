# Faster mixed journal transfers

The first journal optimization merged only a whole batch that was already
consecutive on disk. Mixed checkpoint batches still issued separate requests
for adjacent metadata when other blocks appeared between them in the list.

The new implementation sorts a small index of at most 32 entries, merges each
adjacent run into one device request, and maps completion/error results back
to the original entries. It includes adjacent block-zero/superblock metadata.
The caller's entries remain in order; writes within a batch were already
independent and allowed to complete out of order. Journal stages, barriers,
commit/checkpoint operations and synchronization calls remain in place.

Scratch memory is bounded at 128 KiB. OOM or oversized scratch requirements
use the original scatter requests. When a merged request fails, all members
of that run fail conservatively, while unrelated successful runs remain
successful. The existing cache publication/versioning rules are retained.

The unchanged 32 MiB write-plus-fdatasync workload measured **152.3 -> 169.6
MiB/s**, another **11.4% throughput gain** over the previously optimized kernel.
That corresponds to approximately **10.2% less elapsed time**. There were nine
samples per kernel across three private KVM boots each; all optimized samples
exceeded the fastest baseline sample. This is a measured filesystem gain,
not a claim about every application or a fresh Linux comparison.

[Complete measurements, samples, hashes and caveats](../build/benchmark-results/mixed-journal-comparison.md)
include the discarded ready-buffer lock-elision experiment, whose benefit was
not clear enough to retain. No read-speed or p99 gain is claimed here.

Validation includes ASan/UBSan tests of actual transfer/cache code, unsorted
and mixed runs including block zero, original-index error mapping, OOM and
allocation bounds; a guest test with 1600 out-of-order sparse writes and fsync;
independent host extraction and byte verification after power-off; read-only
filesystem checks on four private images; and the full userland/accounting boot
regression. Older unrelated host fixtures with stale APIs remain documented
limitations; physical power-loss durability has not been tested.

```sh
python3 scripts/tests/test_ext2_gather_write.py
AVORY_STORAGE_RUN=after-mixed python3 scripts/tests/test_storage_bench_boot.py
```

For the preceding optimized kernel, set `AVORY_TEST_KERNEL` to
`build/benchmark-results/kernel-before-cache-prepare`. The runner uses three
unchanged `bench_desktop storage 128 / 2` repetitions on private journaled
4 KiB-block ext4 images. Test runs never modify the normal disk image.
The final kernel is in the rebuilt normal ISO; no disk-image rebuild is needed
for this kernel-only change.
