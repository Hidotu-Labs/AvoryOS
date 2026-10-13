# Firefox file descriptor assertion

The reported `libxul.so+file-offset=0x544cbab` matches the staged Firefox 157
binary. Its executable PT_LOAD segment has `p_vaddr - p_offset = 0x1000`, so
objdump/addr2line must use ELF virtual address `0x544dbab`.

At that address Firefox deliberately executes `mov %rcx,(%rax)` with RAX=0
and RCX=31. Immediately beforehand it stores the string
`MOZ_RELEASE_ASSERT(mHandle)` (file offset `0xe7986d`) in `gMozCrashReason`.
The containing routine calls `mozilla::DuplicateFileHandle(int)`; the caller
at ELF address `0x8190a1c` obtains the input with `PR_FileDesc2NativeHandle`.
The saved RBP is 406 and the last syscall is `fcntl -> -9` (EBADF), consistent
with failed `F_DUPFD_CLOEXEC` on that native descriptor. The fault dump did not
include syscall arguments, so they are now printed for custom-handler faults.
This is an explicit assertion, rather than evidence that address zero should
be mapped or that the user stack needs expanding.

## Kernel repair

The shared CLONE_FILES descriptor table had unlocked source validation/copying
in dup, dup2, dup3, and fcntl duplication. A sibling could close the source
between validation and copying, resulting in a NULL destination returned as
success, or free a node before the duplicate acquired its reference. Replacement
also closed the destination before securing the source reference.

The common duplication helper now validates and reserves under the file-table
lock, pins the source, copies its metadata/path, runs VFS open callbacks outside
the lock, and publishes the destination before releasing the temporary pin.
Replacement closes the displaced reference outside the lock. Reserved targets
return EBUSY. Close validates inside the lock and rejects reserved slots.
Invalid F_DUPFD minimum descriptors now return EINVAL instead of EMFILE.

## Validation and remaining confirmation

Run `python3 scripts/tests/test_fd_duplicate.py`. It extracts the actual syscall
functions and exercises deterministic close/dup interleavings, references,
replacement, reserved slots, CLOEXEC, invalid bounds, and table exhaustion under
UBSan. With the pre-change sys_fd.c supplied through FD_SOURCE, the first race
fails because the duplicate tries to reference a node whose last reference was
already closed. The patched code passes.

This establishes a real kernel bug capable of producing invalid descriptors;
it does not establish which operation originally created/closed descriptor 406
in the reported session. Boot the rebuilt kernel and repeat the Firefox action
to verify the application fix. If it recurs, retain the preceding log and new
`LAST SYSCALL` arguments to distinguish a lost descriptor from an application
lifetime bug or another descriptor producer. No live guest reproduction has
been performed as part of this patch.
