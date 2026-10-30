# i386 runtime startup support

Implemented i386 set_thread_area (243), readv/writev (145/146), ugetrlimit (191),
stat64/lstat64/fstat64 (195/196/197), set_robust_list (311), and rseq (386).

TLS uses slots 6–8 in per-CPU GDT copies; TSS slots now start at 9. A thread owns
three descriptors and its saved FS/GS selectors. Context switches restore them
without losing the kernel's GS base, and fork inherits their values. Descriptor
allocation, replacement, clearing and exhaustion follow the i386 user_desc ABI.
CLONE_SETTLS remains unsupported in the initial compat clone path.

Vectored I/O snapshots 8-byte i386 iovecs into native entries. Resource limits
copy two saturated 32-bit values. stat64 copies the existing 96-byte conversion.
Robust-futex exit cleanup reads 32-bit links and a signed 32-bit futex offset.
rseq has a fixed-width layout and uses the existing native implementation.

Validation passes:

- test_compat_tls_boot.py: real i386 TLS access, updating active GS, fork and
  1,000 yield iterations per process, three-slot allocation/clear/exhaustion,
  writev/readv pipe traffic and invalid vector arguments, rseq register/unregister,
  ugetrlimit/stat64 buffer boundaries, and robust-futex OWNER_DIED on exit.
- test_compat_stat_boot.py: real i386 metadata and symlink tests.
- test_socket_compat_boot.py: native 64-bit smoke regression.
- test_elf32_abi.py, test_append_writes.py (ASAN_OPTIONS=detect_leaks=0).
- test_tsc_log_records.py: concurrent snapshots, balanced locking and formatting.
- Kernel and ISO builds.

The later Half-Life screenshot reports a 64-bit libGL.so.1 being selected by a
32-bit process. Install lib32-libglvnd and lib32-mesa from Arch multilib in the
VM; no package installation or filesystem changes were made to the running VM.

The reported stack canary warning is not conclusively attributed. Two oversized
buffers were moved to heap storage: the metadata path buffer (compiled stack
reservation reduced from 4,296 to 160 bytes) and timing-report buffers (8,216 to
56 bytes). Canary detection stays enabled. The exact game launch has not been
reproduced with the graphics dependencies installed.

## Additional runtime calls (2026-10-29)

Added setpriority (97), prctl (172), statfs64/fstatfs64 (268/269), and
pselect6 (308). statfs64 checks the 84-byte size and copies a narrow structure;
pselect6 snapshots fd sets, time32 and the narrow signal-mask pointer pair.
PR_GET_TID_ADDRESS copies a 32-bit pointer. PR_GET_AUXV remains EOPNOTSUPP for
compat callers until its entries are converted. fcntl64 now permits scalar
ownership, pipe-size and seal operations in addition to descriptor flags.
Network interface ioctls use 8-byte ifconf and 32-byte ifreq records for i386,
including the network namespace path.

`test_compat_runtime_calls_boot.py` passes in a private QEMU guest, with output
canaries for statfs64, PR_GET_TID_ADDRESS, pselect6 and interface ioctls. The
ELF32 ABI host test and ISO build pass. This does not verify Half-Life gameplay.
Custom 32-bit rt_sigaction handlers still return EOPNOTSUPP; signal frame and
sigreturn support remain unfinished. The expanded combined TLS/synchronization
regression has not passed, despite earlier narrower tests passing.

Follow-up: fcntl/fcntl64 now dispatch all commands to the native handler instead
of rejecting lock requests in compat_prepare. The native handler still has no
byte-range lock tracking; this change does not implement locking semantics.
GETLK's fixed-width l_type copy now returns EFAULT on invalid pointers rather
than storing directly. Guest tests cover GETLK64 output, an invalid pointer,
and EINVAL for an unknown command.

IPv4 broadcast destinations now use the Ethernet broadcast address, and IPv4
multicast uses the standard 01:00:5e mapping, bypassing ARP. Missing routes,
unresolved neighbors and oversized packets return network-specific errors
rather than EPERM. test_ipv4_destination.py exercises these decisions; an
actual Half-Life network packet was not captured. ISO build, ELF32 ABI host
test and the extended private QEMU runtime-calls test all pass.

## Sparse shared memory and time/system information calls

Added i386 clock_nanosleep (267) with time32 snapshots, pointer checking,
clock/flag validation and absolute deadlines. Added sysinfo (116) using its
64-byte i386 structure, zeroed padding and a common scaled memory unit so
RAM counters do not wrap above 4 GiB. The existing sleep implementation still
does not provide full Linux signal-interruption/remainder behavior.

memfd now uses tmpfs page storage instead of a single contiguous RAM buffer.
ftruncate growth creates holes, and tmpfs/memfd mmap reserves virtual memory
without allocating every page immediately. A fault obtains the storage frame,
with a PMM reference for the mapping and COW for private views. Fault handling
pins the shmem vnode until completion. fallocate still reserves pages.

Validation: test_compat_memfd_boot.py passes (large i386 mapping, shared/private
views, growth preserving mappings, close lifetime, sysinfo canaries and invalid
pointers, time32 clock_nanosleep). test_memfd_private_views.py passes for native
64-bit calls, including 64 create/map/grow/close/unmap cycles and checking that
reserving a 768 MiB mapping consumes less than 16 MiB of RAM. Host ABI and
truncate error-reporting tests pass. ISO builds. Full Half-Life OpenGL startup
has not been verified.

The desktop disk later failed to resolve init despite the executables being
present. An offline partition copy had directory and allocation inconsistencies.
e2fsck repaired the copy and a QEMU snapshot boot reached userspace services.
Recovery artifacts and logs are in build/boot-recovery-current; the unmodified
pre-repair partition is root-before-repair.img.

A four-CPU compatibility probe did not complete successfully, including when
focused on memfd mappings rather than the earlier metadata checks. Single-CPU
compat and native probes pass. Do not treat the one-CPU results as evidence
that the new path is validated under SMP; the failure remains unresolved.

Recovery completed: the repaired partition was written back to disk.img in
place, and cmp verified all 15,030,288,384 partition bytes against root.img.
The final ISO plus the repaired original disk (QEMU snapshot mode, four CPUs)
started init and services and reached the login prompt. The Half-Life launcher
remains at /home/avory/Downloads/HLCS/game/hl_linux in the repaired image.
Some disconnected filesystem entries were moved to lost+found by e2fsck;
the full pre-repair partition is preserved for further recovery if needed.

Correction to recovery log: the first `dd` write returned without replacing the
partition correctly. A direct offset copy_file_range write was then used; a
full partition comparison returned equal, and a fresh four-CPU QEMU snapshot
boot of disk.img reached the login prompt without init lookup failures.
