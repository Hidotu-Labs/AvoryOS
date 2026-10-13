# Ext4 preallocation batching

`ext2_fallocate_impl()` now reserves up to 256 blocks per transaction for
extent-backed files, instead of 32. The default root image uses 1 KiB ext4
blocks, so contiguous reservations advance by up to 256 KiB per commit.
Bitmap allocation and extent insertion happen once per contiguous reservation.
Existing extents are walked by runs rather than querying every block.

Zeroing still uses transfers of at most 32 blocks and a reusable 32-block
buffer. All reserved blocks are zeroed before publishing their mapping. A
transaction also stops after 32 mapping operations, so fragmented allocations
do not multiply the previous metadata-operation bound. Plain indirect inodes
retain the 32-block transaction budget. KEEP_SIZE, existing data, final size
publication, and journal commit ordering retain their previous behavior.

The actual-source ASan/UBSan harness in `scripts/tests/test_disk_fallocate.py`
reserves the reported SDDM range of `0x1018068` bytes. At 1 KiB blocks, it now
uses 65 transactions and extent inserts instead of 516, an 87.4% reduction.
The 516 bounded zero-data requests are still necessary. This measures operation
counts in synthetic storage, not guest elapsed time or throughput.

The harness also checks zero readback, repeated reservations, sparse existing
data, KEEP_SIZE persistence, competing size updates, one-block allocator runs,
and allocation/start/read/zero/commit failures. A failure on the second zero
transfer releases the whole unpublished reservation and leaves it unmapped.

For guest performance, compare the same fresh file and requested length on the
same disk image with the previous and updated kernels. Capture
`/proc/storage_perf` before and after each allocation to compare journal hold
counts and device requests. Guest latency and hardware recovery remain to be
verified.
