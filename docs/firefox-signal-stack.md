# Firefox isolated content signal delivery

The pasted log contains two `Isolated Web Co` SIGILL exceptions at `0F 0B`
(`UD2`) instructions in executable anonymous mappings. The kernel selects a
custom handler, then reports `Stack overflow/invalid during delivery` and exits
with SIGSEGV before that handler runs. The SIGKILL messages in the same excerpt
target zbus/glycin helper processes, not these content threads.

Firefox uses SIGILL to implement WebAssembly traps; its handler can recover by
changing the instruction pointer in the supplied ucontext. See
[Mozilla's Wasm signal handlers](https://searchfox.org/firefox-main/source/js/src/wasm/WasmSignalHandlers.cpp).
The instruction alone therefore does not establish an unsupported CPU feature
or an unrecoverable application crash.

## Repair

AvoryOS inherited the alternate signal stack across fork but never cleared it
on successful exec. Exec destroys the old mappings, leaving a registration
whose address can be unmapped or reused for unrelated memory. A subsequent
SA_ONSTACK handler can then fail writable-range validation. Linux explicitly
[discards the alternate signal stack on exec](https://man7.org/linux/man-pages/man2/execve.2.html).

Successful exec now clears ss_sp and ss_size and sets SS_DISABLE alongside the
signal-disposition reset. Failed exec and fork inheritance retain their original
behavior. The delivery failure now records the selected frame address, size,
interrupted RSP, alternate-stack selection, registration and action flags.

This is a verified kernel compatibility defect and a plausible explanation for
the Firefox failures. The supplied log omits the selected signal stack, so it
cannot prove that a stale registration caused these specific failures. The
existing writable-range and alternate-stack bounds checks remain in place.

## Validation

`userland/test_signal_stack.c` now exercises actual UD2 exceptions on an
alternate stack, siginfo fault addresses, ucontext RIP edits, repeated signal
return, fork inheritance, failed-exec preservation, successful-exec reset and
parent-state preservation, in addition to normal/alternate/nested delivery.

Build with `make userland/test_signal_stack.elf`; run `/bin/test_signal_stack`
in the guest. The test can also run as `init=/bin/test_signal_stack` and writes
its result to `/dev/kmsg` for serial capture.

The expanded test passed on host Linux and in a two-vCPU QEMU TCG AvoryOS guest
using a disposable 32 MiB ext2 image. The kernel rebuilt successfully.
The same guest test failed at the post-exec disabled-stack check when run with
an otherwise identical kernel with only the new exec reset removed.
Artifacts and guest logs are under `build/signal-stack-diagnostic/`; the ordinary disk
image was not modified. Full Firefox reproduction still requires booting the
updated kernel and repeating the page action that produced the pasted log.
