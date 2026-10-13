# Firefox download integrity investigation

Reported on AvoryOS with the FastyPDF 1.1.0 Windows installer:
https://github.com/Piotriox/FastyPDF/releases/tag/1.1.0

No failing guest file, checksum, Firefox completion status, or installer error
has been supplied yet. The fixes below address confirmed kernel defects, but
are not yet established as the cause of this particular report.

## Preallocation repairs

The generic VFS fallocate fallback used truncate instead of reserving blocks.
With FALLOC_FL_KEEP_SIZE it extended the persistent inode, then restored only
node->length. Closing/reopening exposed the extended length; ext4 reads could
also repair the node length from that enlarged inode. Mode 0 claimed reserved
space while creating only a sparse extension. Neither operation implements the
allocation contract.

The initial repair returned EOPNOTSUPP for disk-backed files. A later report
showed sddm-greeter-qt requesting mode 0, offset 0, length 0x1018068 and
receiving that error while the greeter failed to open. Disk-backed regular
files now register ext2_fallocate_impl for both indirect and extent inodes.
It reserves missing blocks, zeroes them before installing their mappings,
and preserves all existing mapped bytes. Transactions process at most 32
logical blocks and reload the inode under journal ownership on every batch.
Mode 0 extends the persistent size only after completing the reservation;
KEEP_SIZE leaves that size untouched throughout. Native ramfs/tmpfs
allocation still dispatches normally. Other backends without an allocator
return EOPNOTSUPP without modifying the file. Unsupported modes are rejected by the
syscall rather than reaching backends that could interpret them as ordinary
allocation. Zero/negative ranges are rejected, and ranges outside the 32-bit
VFS limit return EFBIG before narrowing. ftruncate likewise rejects oversized
lengths instead of silently wrapping. This does not implement files above 4 GiB.

## Verification

`python3 scripts/tests/test_download_preallocation.py` extracts the actual VFS
and syscall code and exercises persistent KEEP_SIZE invariants, mode/range
validation, native dispatch, and ordinary truncate/cache invalidation under
UBSan. The original VFS fails the persistent-size assertion.

`python3 scripts/tests/test_disk_fallocate.py` tests the exact greeter request,
zero readback, repeat reservations, preserved existing bytes, KEEP_SIZE and
reopen size, competing size extensions, bounded batches, and allocation/I/O
failures under ASan/UBSan. It uses the real reservation function and extent
leaf lookup/insertion with modeled disk allocation and indirect mappings.
Greeter startup in the guest still requires confirmation. On a mapping I/O
failure, uncertain data blocks are not returned for unrelated reuse; recovery
may need filesystem checking.

`python3 scripts/tests/test_ext4_sparse_write.py` passes, including complete
readback of 40 blocks, unaligned writes, fragmented runs, and short user copies.
Kernel and regular avoryos-x86_64.iso builds pass. Guest Firefox reproduction
has not yet been performed; existing downloaded files are not repaired.

## Check the reported file in the guest

GitHub's public release API reports the asset as:

- Filename: FastyPDF_Setup_v1.1.0.exe
- Size: 35,762,668 bytes
- SHA-256: 5c01d447812ad5170ad3bcbf3ffe1c8f123c738f1da0f4a89d6c03bd59f259cf

Run `sha256sum ~/Downloads/FastyPDF_Setup_v1.1.0.exe`, adjusting the path.
A matching hash establishes that the saved bytes match the release asset;
retain the exact opening/installer error to investigate Wine compatibility.
A different hash warrants comparing a fresh Firefox download after booting the
rebuilt ISO, plus a curl download from the same release asset. Record both file
sizes/hashes and whether Firefox reported successful completion. Keep the old
file for comparison. This separates a browser-specific path from shared
network/storage faults.

## Greeter black-screen follow-up

The supplied manual hang report shows every CPU answering the probe and the
main greeter thread inside fallocate. Resolving the parked stack against the
matching build identifies nvme_io_wait -> nvme_flush_once ->
ext4_extent_write_node -> ext2_fallocate_impl. The root journal snapshot is
owned by that thread, held for 263 ms. A single manual report does not establish
a permanent deadlock: the thread snapshots are collected while execution
continues. In particular, the printed overdue timeout is not proof that its
scheduler deadline was lost.

The initial allocator inserted one extent mapping per missing block. Once the
extent tree uses external nodes, those insertions execute the existing durable
node-write/flush path. Extent-backed reservations now allocate contiguous runs,
zero each run with ext2_write_blocks before inserting it, and stop at existing
mappings and the 32-block transaction limit. Physical fragmentation can shorten
a run. The existing extent-node durability policy is unchanged. Journal phase
labels now identify inode reads, zeroing, mapping, and inode publication.

For the exact 0x1018068-byte request on the contiguous 1 KiB test fixture,
mapping insertions and zero-data submissions are 516 rather than 16,481.
The regression fails its insertion-count assertion with the previous allocator.
It also checks sparse reservations stop before an existing extent and preserve
its bytes, and that a failed run zero-write releases all unmapped reserved blocks.
The NVMe wait-registration and extent-write regression suites pass, as does the
regular ISO build. This is a tested reduction in requests, not a measured guest
startup time or confirmation that the black screen has been eliminated.
