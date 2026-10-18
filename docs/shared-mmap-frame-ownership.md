# Shared tmpfs/memfd mappings must own a PMM reference per PTE

Diagnosed 2026-10-17 from the Epiphany/WebKit workload: the kernel faulted in
the VMA tree (`inorder_gather`, `vma_total_bytes_recursive`) after the PMM
buddy free lists were corrupted with user data (WebKit/JS strings and
near-white pixel runs such as `FAFAFB`). Those are two ends of one defect:
a user mapping kept pointing at frames the filesystem had already released,
so writes through the mapping corrupted whatever the PMM re-issued the frame
for, and a recycled frame later used for a `struct vma` produced the GPF.

## Root cause

The direct mmap handlers for tmpfs (`tmpfs_mmap`) and memfd (`memfd_mmap`)
installed the filesystem's own frames straight into user PTEs without taking a
PMM reference per PTE. The frames belong to the filesystem and are released on
every resize path, so any mapping outliving the resize became a write primitive
into freed memory:

- `tmpfs_truncate` shrink and `tmpfs_free_all_pages` -> `tmpfs_release_pages`
  (`kernel/src/fs/tmpfs.c`) free the frames in place.
- `memfd_mmap` reallocates and frees the old block when a mapping needs more
  capacity than the file has (`kernel/src/syscalls/sys_aio.c`).
- `ramfs_write` growth, `ramfs_truncate(0)` and `ramfs_fallocate` growth free
  the old block after copying it (`kernel/src/fs/ramfs.c`); memfd files are
  ramfs files.

In this workload `/tmp` and `/dev/shm` are tmpfs mounts, and WebKit/GTK move
pixel and cache data through memfd and tmpfs shared mappings, so the window is
hit regularly: the CommonMark cache files, GTK's shm buffers and WebKit's
shared memory all live in frames whose only reference was the file's.

## Fix

Direct private views already owned one reference per PTE
(`mmap_private_pmm_refs`); shared views now do the same:

- `kernel/src/fs/tmpfs.c` and `kernel/src/syscalls/sys_aio.c`: take a
  `pmm_incref()` per freshly installed PTE, and advertise the new
  `mmap_shared_pmm_refs` node flag.
- `kernel/src/fs/vfs.h`: `mmap_shared_pmm_refs` next to the existing
  `mmap_private_pmm_refs`.
- `kernel/src/syscalls/sys_mm.c`: `vma_has_file_page_refs()` returns true for
  shared views of nodes that own their PTEs, so munmap/MAP_FIXED/MADV teardown
  drops the mapping's reference after the TLB drain.
- `kernel/src/mm/vmm_free.c`: process exit releases shared file-backed PTEs
  instead of skipping every `MAP_SHARED` frame; device-backed shared views
  (GEM, framebuffers) still defer to their backing object.
- `kernel/src/mm/vmm_clone.c`: a forked child's copy of such a view takes its
  own reference.
- `kernel/src/fs/mount_namespace.c`, `kernel/src/linuxkpi/native_vfs.c`: clone
  the flag when nodes are re-exported as mount/namespace views.

The filesystem free paths are unchanged and remain correct: `pmm_free_page()`
and `pmm_free_pages()` are decrements, so a released storage block now leaves a
still-mapped frame alive until the last PTE goes away.

## Verification

`scripts/tests/test_shared_mmap_lifetime.py` (private guest probe) reproduces
the defect deterministically. It fills a shared mapping with `0x11`, forces the
backing release (memfd realloc on mmap growth, memfd realloc on write growth,
tmpfs `ftruncate` shrink), then claims a large anonymous buffer filled with
`0x5A` and reads the old mapping back. A recycled byte means the frame was
re-issued while the mapping was live. The probe never writes through a stale
PTE, so a failing run corrupts nothing.

| scenario | unfixed kernel | fixed kernel |
| --- | --- | --- |
| memfd realloc vs. live map | 188416/262144 recycled | 0 |
| memfd write-grow vs. live map | 262144/262144 recycled | 0 |
| tmpfs truncate vs. live map | 258048/262144 recycled | 0 |

The same probe fails through the standard harness with the pre-fix kernel
(`AVORY_TEST_KERNEL=... python3 scripts/tests/test_shared_mmap_lifetime.py`
raises) and passes with the fixed kernel. Kernel boot self-tests (tmpfs, PMM,
LinuxKPI GEM/TTM loops) pass with unchanged PMM deltas.

The full Epiphany/WebKit workload (Xorg + D-Bus + Epiphany with the default
llvmpipe/Vulkan renderer) then ran four times per kernel:

| run | kernel | result |
| --- | --- | --- |
| control 1 | unfixed | WebProcess never appeared (`webprocess=0`, browser alive) |
| control 2 | unfixed | GPF panic (`rip=0xFFFFFFFF800C4B82`, inside `vma_total_bytes_recursive`) while the probe walked `/proc/72/status` |
| control 3 | unfixed | nominal pass, but the WebProcess never grew past 5 worker entries (fixed runs: 29-33) |
| control 4 | unfixed | same partial WebProcess (4-5 worker entries) |
| fixed 1-4 | fixed | `EPIPHANY_FULL: PASS live browser and WebKitWebProcess` in all four, WebProcess at 29-33 worker entries |

Control 2 reproduces the original report exactly: a fault in the VMA-tree walk
during proc inspection, after the free lists had been poisoned by a stale
mapping. Controls 3 and 4 show the WebProcess starting but never reaching a
healthy thread count, which matches the reported default-renderer startup
failures; the fixed kernel reached a full WebProcess in every run. The browser
workload remains timing sensitive, so these runs are corroborating rather than
conclusive; the deterministic mapping probe above is the primary evidence.

Serial logs: `build/epiphany-recheck/sharedmap-{control,fixed}.log` for the
focused probe, `build/epiphany-recheck/browser-*.log` for the workload runs.

The rebuilt `avoryos-x86_64.iso` (2026-10-17) ships the fixed stripped kernel;
`make run` boots it.

## Related ownership gaps (not addressed here)

- SysV SHM (`kernel/src/syscalls/sys_shm.c`) takes one reference per attached
  page but neither process exit nor a forked child's PTE copy is accounted:
  exit leaks the attach references (the segment's `nattch` never drops), and a
  `shmdt` in a forked child decrements a reference it never took.
- Native DRM GEM dumb-buffer mappings (`drm_vfs_mmap`) map the BO frames with
  no per-PTE reference; destroying a BO immediately before `munmap` leaves a
  short live-mapping window. The PRIME-import token shares the same node, and
  those views keep dma-buf ownership, so the node-wide flag used here is not
  applicable without splitting the two paths.
- The NVMe direct-read path publishes page-cache frames from a failed transfer
  without the bounce path's "DMA may still land" guard
  (`kernel/src/drivers/storage/nvme.c`); a transport failure could leave disk
  data in a recycled frame.
