# Signal-frame write under memory exhaustion

The supplied Firefox capture ends in a kernel page fault while executing
`memset` from `signal_deliver_frame`. The destination is a user address whose
PTE is present, read-only and marked CoW, with two physical-frame references.
The pager explicitly rejects it with `CoW copy: PMM out of memory`. There is no
exception-table entry for the memset instruction, so the ISR panics. Earlier
intermediate-page-table allocations also fail. This establishes allocation
failure, not its underlying cause (a leak, retained mappings or workload size).

Checking writable VMA permissions before writing a signal frame did not ensure
that a CoW copy or a missing page could be allocated. Signal delivery now builds
the frame on the kernel stack and publishes it with `copy_to_user`. The signal
restorer address uses the same guarded copy. An allocation failure or concurrent
unmap can therefore take the assembly exception-table fixup. Failure to construct
a frame terminates the affected thread group with the existing SIGSEGV-style
status (139), including siblings retaining its shared address space. It does
not panic at an unguarded store or leave a partly installed handler running.
Existing invalid/overflowing signal-stack paths use the same fatal-group path.

All embedded ABI pointers must refer to the user copy: handler siginfo/ucontext
arguments and the ucontext's saved-FPU pointer are explicitly relocated to the
user frame. The red zone, layout, saved masks, queued siginfo, FPU image and
alternate-stack behavior are retained.

The forkserver's separate null write is deliberate Firefox crash code after
`ForkServer: SubprocessExecInfo receive error`. In the staged libxul ELF, file
offset 0x54436ee corresponds to ELF virtual address 0x54446ee (executable segment
VAddr minus Offset is 0x1000). The code zeros RBX, stores its crash reason, loads
RAX with 194 and executes `mov %rax,(%rbx)`, matching the supplied bytes and
registers. Mapping address zero would hide the error. This capture alone does
not establish why that IPC receive failed; it does not prove the kernel panic
and the forkserver failure have the same cause.

## Verification

- `scripts/tests/test_signal_frame_usercopy.py` compiles actual frame/context
  construction and real uaccess assembly. A host SIGSEGV handler applies the
  real exception-table entries. Writable VMA validation combined with read-only
  backing models a CoW write the pager cannot resolve. Initial, partial and
  restorer-only copy failures all recover and terminate the process group.
  Success cases check ABI pointers, queued payload, masks, red zone and altstack.
  Restoring the unguarded destination makes this test fail at the injected fault.
  This is a fault-recovery test, not a reproduction of physical memory exhaustion.
- `test_rt_sigqueueinfo.py` and `test_exit_user_access.py` pass.
- The kernel builds (only the existing RWX linker warning).
- An isolated eight-vCPU KVM guest boots the candidate kernel, runs both native
  syscall restart/signal-frame ABI and `test_signal_stack` tests, and powers off
  normally. Both tests exit zero. The latter covers normal/alternate/nested
  handlers, SIGILL recovery, disabled altstack and fork/exec behavior. Serial
  messages interleave, so validation uses the parent-recorded child exit status.
- `test_thread_directed_signals.py` cannot currently run: it extracts an obsolete
  `static void signal_dequeue_locked` signature; the current queued-signal
  implementation returns bool. That pre-existing harness mismatch was not changed.

No memory-size increase or disk image modification is part of this repair.
Physical memory growth and the earlier journal/scheduler freeze remain separate
investigations. Artifacts and the matching debug ELF are in
`build/signal-frame-oom/`.
