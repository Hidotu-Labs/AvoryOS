> The follow-up [mixed journal transfer optimization](mixed-journal-transfers.md) extends this initial implementation. The measurements below describe the first optimization pass.

# Faster journal transfers

The filesystem now combines consecutive metadata blocks into one device write
when they arrive as a scattered-memory batch. Journal payloads commonly have
this layout. Previously, even a contiguous run required a separate device
request for every block.

`ext2_write_blocks_batch()` validates the batch as before, gathers contiguous
images into at most 128 KiB of temporary memory, and uses the existing
`ext2_write_blocks()` path. This preserves its versioned cache publication and
failure invalidation. Scattered batches, block-zero metadata, oversized batches
and allocation failures retain the original path. A merged I/O failure marks
all members failed, so the journal cannot mistake partial completion for a
successful batch. Journal ordering, commit/checkpoint stages and synchronization
calls remain in place.

The unchanged real-filesystem benchmark measured a **10.5% throughput gain**:
32 MiB write plus fdatasync increased from **137.1 to 151.5 MiB/s**, using the
median of six runs per kernel on 4 KiB-block ext4. This corresponds to about
**9.5% less elapsed time** for that write. A separate three-run pair on
1 KiB-block ext4 improved 13.3%. These are private KVM guest measurements, not
a fresh Linux comparison or a claim about every application's performance.
One optimized outlier below baseline is retained in the results; reads did not
show a consistent improvement.

[Full measurements, raw logs and validation limitations](../build/benchmark-results/journal-gather-comparison.md)
record the exact environment and discarded experiments. Larger unmap batches,
physical-allocator drain batching and less-frequent NVMe status polling did not
produce a reliable benefit and were removed. Only the filesystem coalescing
change remains from this optimization pass.

Validation includes production-function tests under ASan/UBSan for byte/cache
coherence, I/O errors, OOM fallback and allocation bounds; actual guest sparse
writes with 1600 out-of-order blocks, fsync and complete content verification;
and read-only filesystem checks on three optimized test images. Older unit
fixtures with stale pre-existing APIs could not compile and are documented
separately. Physical power-loss durability has not been tested.

```sh
python3 scripts/tests/test_ext2_gather_write.py
AVORY_STORAGE_RUN=after AVORY_STORAGE_BLOCK_SIZE=4096 \
  python3 scripts/tests/test_storage_bench_boot.py
```

The storage runner creates private ext4/NVMe images, uses the unchanged
`bench_desktop storage 128 / 2` workload three times per boot, and collects
`/proc/storage_perf`. Set `AVORY_TEST_KERNEL` to the preserved pre-change
kernel for a baseline. Both 1 KiB and 4 KiB block layouts must be compared
against matching baselines. The updated kernel is shipped in the normal ISO;
this kernel-only change needs no disk-image rebuild.
