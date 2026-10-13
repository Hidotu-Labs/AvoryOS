# Initial i386 ELF support

AvoryOS can now parse little-endian EM_386 ELF32 ET_EXEC and ET_DYN images,
including a matching ELF32 PT_INTERP. ELF64/EM_X86_64 remains supported. Headers,
program-table bounds, file-backed segment sizes and user-address bounds are
validated before each segment is registered. Pure BSS segments have anonymous
backing, so a zero-byte file segment cannot accidentally expose ELF header data.

The ELF32 stack ends at 0xbfffe000. Startup argc, argv, envp and auxiliary-vector
words are 32 bits, AT_PLATFORM is i686, and AT_SYSINFO_EHDR is zero: the existing
64-bit vDSO is not advertised to i386 programs. Self-loading/interpreter images
use 0x70000000; ordinary PIE programs use 0x400000. Anonymous mmap allocation
uses 0x40000000..0xb0000000 and mappings are bounded below 0xc0000000.

The CPU's existing 0x1b compatibility-mode code segment is entered through
IRETQ. Vector 0x80 is a DPL3 software syscall gate and is reserved from device
interrupt allocation; it does not send an APIC/PIC EOI or enter hardirq context.
The dispatcher translates EAX syscall numbers and EBX/ECX/EDX/ESI/EDI/EBP
arguments to the native handlers. Return shadows preserve ECX/ESI/EDI for
fork/clone; exec accepts the caller's pointer width and can change ABI in either
direction. Forked address spaces inherit their ABI and failed exec restores it.

Supported calls include basic file operations, scalar descriptor controls,
process identity, fork/vfork, execve, waitpid, mmap/mmap2, munmap, mprotect, brk,
poll and selected other calls with compatible layouts. mmap2 offsets are
converted from pages to bytes, signed PIDs/offsets/timeouts are sign-extended,
and pread/pwrite offsets are assembled from their two 32-bit words. The number
mapping follows Linux's `arch/x86/entry/syscalls/syscall_32.tbl`.

This is initial compatibility support, not a complete Linux i386 ABI. TLS
user_desc/set_thread_area, custom i386 signal frames, stat/stat64 conversion,
socketcall, iovec conversion, time32 structures and SYSENTER are not implemented.
Unsupported calls return ENOSYS with a limited `[COMPAT32]` diagnostic. In
particular, enabling ELF32 parsing does not install an i386 Unix Wine runtime,
and running an older 32-bit glibc/Wine build still needs the remaining ABI work.

## Verification

```
python3 scripts/tests/test_elf_loader_placement.py
python3 scripts/tests/test_elf32_abi.py
python3 scripts/tests/test_mmap_fixed_reservations.py
make userland/test_elf32.elf
```

Host fixtures exercise the actual kernel loader, stack builder, mmap function
and argument translator, including ELF64 regressions, malformed ELF32 images,
32-bit pointers/auxv, low mappings and fork return-frame offsets.

`test_elf32` is a static, libc-free i386 executable installed at /bin/test_elf32
by the disk-image recipe. It checks startup stack/auxv (stages 1–2), zero-filled
BSS (3), getpid through int80 (4), mmap2 and writable zero memory (5), munmap (6),
fork register preservation (7), and waitpid/child status (8). Its mmap check
accepts all successful 32-bit addresses, since 64-bit Linux may place compat
mappings above AvoryOS's 3 GiB bound. Failure output identifies the stage.

The corrected binary passes on the Linux host. The sandbox blocks i386 system
calls, so this host runtime check was run outside that sandbox. Kernel build and
host regression checks pass; AvoryOS runtime verification is left to the user.
