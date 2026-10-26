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

## Forkit AppImage follow-up (2026-10-26)

The follow-up report is Firefox completing a download in QEMU, followed by
failure to launch Forkit 1.3.0. The later screenshot and read-only disk snapshot
confirmed a missing prefix; see the O_APPEND diagnosis below. The read-error
defects described here were independently found, but did not explain that
specific missing-prefix pattern.

GitHub's public release API for
https://github.com/Offihito/Forkit-Browser/releases/tag/V1.3.0 reports:

- Asset: `Forkit-1.3.0.AppImage`
- Size: 117,723,467 bytes
- SHA-256: `f018d6a1c41f71631757dab70ee509cff80f6429e935e46c737f598dad09ec39`

Confirmed defects found during the follow-up:

- Ext2 and ext4 block-at-a-time reads ignored data-read failures and could
  copy the previous scratch block (or uninitialized scratch bytes) into the
  destination and report success. They now stop before copying the failed
  block. Ext2 uses checked indirect mapping lookup, distinguishes failures
  from holes, and clamps EOF ranges without addition overflow. Both readers
  return the valid prefix or an error if no bytes were delivered, including
  failed user copies when zero-filling holes.
- Read-ahead compared unsigned backend results only with `got < requested`,
  and clustered paths clamped larger results to the request. Negative errno
  values use the unsigned VFS ABI and therefore passed those checks, allowing
  incomplete frames to become valid cache pages. Every read-ahead fill route
  now requires the exact requested count. Failed clustered reads retain the
  existing individual-page retry behavior.

`ASAN_OPTIONS=detect_leaks=0 python3 scripts/tests/test_download_read_errors.py`
passes with AddressSanitizer and UBSan. Leak detection is disabled because the
sandbox's tracing prevents LeakSanitizer from running. The test extracts actual
read/fill functions and injects inode, mapping, data I/O and user-copy failures,
checks valid prefixes, and covers contiguous, scratch and individual cache
fills. Substituting the old ext4 reader or old page cache fails its assertions.
`python3 scripts/tests/test_page_cache_batch_flush.py` also passes.

The older `test_download_preallocation.py` harness currently lacks mocks for
`vfs_mount_readonly` and `FD_IN_RANGE`; `test_ext4_sparse_write.py` extracts an
extent helper name that no longer exists. Both stop before executing checks
against the current tree and do not establish a preallocation/write regression.

A private QEMU regression is available as
`python3 scripts/tests/test_download_integrity_boot.py`. It reserves KEEP_SIZE
space, writes position-dependent bytes at the exact AppImage length in
unaligned chunks, fsyncs, renames the .part file, reopens it, and compares every
byte through read and MAP_PRIVATE. It uses a separate disk under build/ and
explicit supported filesystem features, leaving disk.img untouched.
`AVORY_TEST_FILESYSTEM=ext2` selects indirect mappings instead of ext4 extents.
This synthetic test does not exercise Firefox, HTTP, or AppImage execution.

Both ext4 and ext2 private QEMU tests pass with the complete 117,723,467-byte
readback and private-mapping comparison. Kernel and regular `avoryos-x86_64.iso` builds
pass. The final build retains existing compiler/linker warnings. The rebuilt
ISO must be booted to use these changes; existing damaged downloads are not
repaired. These synthetic read tests do not exercise Firefox or AppImage execution;
the subsequent continuation/runtime reproduction is documented below.

## Confirmed missing-prefix cause: O_APPEND (2026-10-26)

The follow-up screenshot reports `execve: Exec format error`, with kernel log
`[PROC] execve failed path="/Downloads/Forkit-1.3.0.AppImage" errno=8 (elf_load)`.
The ELF loader accepts the checksum-verified release asset and starts its
runtime with `--appimage-version` (output: `Version: effcebc`). No ELF format
exception is required for this AppImage.

A read-only reflink snapshot of the root partition in disk.img established the
actual saved-file defect without changing the running VM's disk:

- Saved size: 117,701,597 bytes, versus 117,723,467 in the release.
- Saved SHA-256: `653b0fd35fa795b16f096fe1adbb8c56a48c317dbf0f510a1c28372079094acc`.
- Every saved byte equals the corresponding byte of the release after removing
  exactly its first 21,870 bytes. This is a complete suffix match, not scattered
  corruption. The missing prefix contains the ELF header, explaining ENOEXEC.

`fd_write` retrieved descriptor flags but ignored O_APPEND. Reopening an
existing partial file for append therefore wrote subsequent data at the new
descriptor's initial position zero. Firefox's
[BackgroundFileSaver::ProcessStateChange](https://github.com/mozilla-firefox/firefox/blob/main/netwerk/base/BackgroundFileSaver.cpp)
uses PR_APPEND when continuing after a target change; this is the precise
close/move/reopen sequence exercised by the new regression.

Append writes now use VFS append dispatch and update the descriptor position to
the actual end of the appended bytes. `writev` inherits that behavior through
`fd_write`. `pwrite`/`pwritev` also honor Linux O_APPEND behavior while leaving
the descriptor position unchanged. Namespace views retain mount read-only
checks. Memory filesystem fallback uses the current vnode length; disk regular
files register `ext2_append_impl` for both indirect and extent-backed inodes.

Disk append stages user data before acquiring journal ownership, selects EOF
from the persistent inode under that ownership, and retains ownership through
writing and publication. This handles stale vnode lengths and serializes
competing writers on journaled mounts. Unjournaled filesystems retain their
existing write concurrency behavior. Each operation returns at most 31 filesystem blocks,
so even an unaligned EOF fits a single existing 32-block write batch. Callers
continue after a successful short write. Commit errors invalidate cached data
and return EIO rather than reporting byte success.

Validation:

- `ASAN_OPTIONS=detect_leaks=0 python3 scripts/tests/test_append_writes.py`
  passes under ASan/UBSan. It exercises actual syscall/VFS/append functions,
  the exact 21,870-byte prefix case, append after seek, stale aliases,
  positioned and vectored positioned writes, short writes/copies, bounds,
  allocation/I/O/commit failures and four concurrent journaled disk appenders.
  Substituting the old fd_write fails the prefix/size comparison.
- `AVORY_TEST_APPIMAGE=/tmp/avory-Forkit-1.3.0.AppImage python3
  scripts/tests/test_appimage_exec_boot.py` passes in a private QEMU guest:
  write first 21,870 bytes, close/move/reopen with O_APPEND, save remaining
  bytes, fsync/rename/reopen, compare the complete file to the verified release,
  and execute `--appimage-version` successfully. This validates the AppImage
  runtime, not the Electron GUI or FUSE mounting.
- Running the same guest test with the pre-append kernel fails its completed
  size check. Extracting that test output file shows the identical full suffix
  match and missing 21,870-byte prefix as the user's saved download.
  Before/after serial logs are retained under
  `build/appimage-exec-boot/serial-before-append.log` and
  `build/appimage-exec-boot/serial-after-append.log`.
- The read-error/cache regression and page-cache writeback regression pass.
  Existing fd-duplicate, ELF-placement, and ext2 write-batch host harnesses
  have stale mocks (dynamic fd table fields, an unused prefetch stub, and
  32-bit block declarations respectively) and stop before running assertions.
- Kernel and regular ISO builds pass. Boot the rebuilt ISO and download a new
  copy; the old file in disk.img was preserved for diagnosis.

The full-size close/rename/reopen-append/readback/MAP_PRIVATE test also passes
on the unjournaled ext2 private guest (`AVORY_TEST_FILESYSTEM=ext2`).
