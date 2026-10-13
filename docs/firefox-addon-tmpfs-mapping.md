# Firefox add-on archive mapping

The supplied screenshot shows Firefox cancelling uBlock Origin installation
because the add-on appears corrupt. The accompanying sysdiag report records
mmap returning ENOMEM twice in the parent Firefox process for addr=0,
length=0x46f474, prot=PROT_READ. It does not include the mmap flags, fd, or
filename, so it does not by itself prove that either failed mapping is the XPI.

## Confirmed kernel defect and repair

/tmp is backed by tmpfs. Every tmpfs regular file registered tmpfs_mmap, which
rejects MAP_PRIVATE with -1. sys_mmap converts that result to ENOMEM. The
shared-only flag was missing, so even a valid private read-only archive map
was routed into the shared handler and rejected.

Tmpfs regular files now set mmap_shared_only=true. sys_mmap already supports
this contract: MAP_PRIVATE bypasses the driver handler and creates a
MAP_PAGECACHE VMA served by generic file faults. Cached frames are owned by the
page cache, and writable private mappings use its existing copy-on-write path.
Shared mappings continue through tmpfs_mmap and keep tmpfs frame ownership.
This does not bypass Firefox's archive or signature checks.

## Validation

- python3 scripts/tests/test_mmap_fixed_reservations.py: actual tmpfs handler
  setup and sys_mmap routing, exact logged length with private/read-only mode,
  private/writable routing, shared mapping ownership, and existing address
  reservation checks. With TMPFS_SOURCE pointing to the old tmpfs.c, it fails
  because MAP_PRIVATE reaches the shared-only mock handler.
- python3 scripts/tests/test_file_cache_cow.py: private cache pages stay CoW
  when mprotect races their installation.
- python3 scripts/tests/test_memfd_private_views.py: existing private mapping,
  CoW, and unmap reference regressions remain passing.
- Kernel and regular ISO builds pass.

Guest add-on installation remains to be confirmed. If it still fails after
booting the rebuilt ISO, retain Firefox's add-on installation error details
and the downloaded XPI for byte/ZIP validation; the supplied log does not
establish actual download corruption.
