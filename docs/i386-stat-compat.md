# i386 metadata calls used by the Half-Life loader

The reported missing i386 syscalls are statx (383) and fstatat64 (300),
confirmed against Linux's asm/unistd_32.h. Both now enter the normal native
syscall dispatcher, retaining the existing compat seccomp checks.

statx uses the same fixed-width layout across i386 and x86-64. fstatat64 uses
i386's 96-byte, 4-byte-aligned stat64 layout, including the legacy inode field,
64-bit file size, block count and final inode. The native fstatat path fills a
kernel kstat, then copies the appropriate ABI layout to userspace. Padding is
zeroed; oversized link counts or block sizes return EOVERFLOW. Pathnames use
fault-safe copies, invalid flags are rejected, and failed metadata lookups do
not overwrite the result buffer.

Validation:

- scripts/tests/test_elf32_abi.py passes, including both new syscall mappings.
- scripts/tests/test_compat_stat_boot.py passes in a private KVM guest with a
  real static i386 binary. Checks statx/fstatat agreement, the 96-byte output
  boundary, missing paths, inaccessible output, invalid flags and symlink
  follow/nofollow behavior.
- Kernel and avoryos-x86_64.iso rebuild successfully.

Boot the rebuilt ISO to use this change. This verifies these two metadata
syscalls, not full Half-Life gameplay. Other missing i386 interfaces may still
need implementation as the loader and game progress.
