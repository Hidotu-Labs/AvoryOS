# Epiphany D-Bus proxy abort with a runtime directory on /tmp

The October 17 crash aborts in WebKit's sandbox launcher with
`Failed to fully launch dbus-proxy: Child process exited with code 1`.
The utilization-clamp `sched_setattr` EOPNOTSUPP and null-pointer seccomp EFAULT
are expected feature probes and do not explain this abort.

## Reproduction and native fixes

The installed, unmodified bubblewrap and WebKit libraries were compared between
`disk.img` and the existing private Epiphany test image; their hashes match.
A private VM reproduced the supplied abort with its D-Bus socket at
`/tmp/epiphany-bus` and `XDG_RUNTIME_DIR=/tmp/runtime-0`. Capturing the child's
stderr revealed `bwrap: Can't open source /tmp/runtime-0/webkitgtk: No such file
or directory`. The same launch had previously passed with its runtime and bus
under `/run`.

Bubblewrap mounts its staging tmpfs on `/tmp`, then pivots into it, putting the
old root at `/oldroot`. AvoryOS implemented that pivot using a recursive bind
of the old root. It included the staging mount in the copied tree, so
`/oldroot/tmp` still showed the staging filesystem instead of the covered host
`/tmp`. The runtime directory existed but was hidden from the launcher.

The pivot's internal mount-tree copy now excludes the new-root mount and its
descendants. This exposes the covered mountpoint through put_old. Normal
recursive bind operations still copy their complete mount subtree.

Two additional kernel defects surfaced during launch verification:

* Namespace pathname resolution placed three 4096-byte arrays on the kernel
  stack. Its compiled frame was 0x30d8 bytes, against a 16 KiB thread stack,
  leaving too little space for syscall callers and filesystem callbacks.
  Bounded heap scratch storage and an overlapping-safe symlink-tail move reduce
  the frame allocation to 0xe8 bytes (240 bytes including the return address
  in GCC's stack-usage report). Allocation failure reports ENOMEM through namei.
* Signalfd notification checked `t->fds` once and then repeatedly dereferenced
  it without excluding exit's table detach or concurrent descriptor close.
  A private launch faulted in this loop while D-Bus killed an exiting child.
  Notification now snapshots matching nodes in bounded batches under tid_lock
  and the descriptor-table lock, pins them, and wakes waiters after dropping
  both locks. The pins retain each signalfd context through notification.

These changes are entirely in native kernel paths. No application executable,
launcher, sandbox setting, or installed disk filesystem was changed.

## Validation

The new mount regression covers staging on `/tmp`, old-root visibility,
binding the host runtime directory into the new tree, and its survival after
old-root detach. The packaged bubblewrap regression binds a source under
`/tmp/runtime-0/webkitgtk` and succeeds with both shared and private networking.
The signalfd host regression exercises the actual notifier with ASan/UBSan
while another thread closes descriptors and detaches tables across batches.
The namespace source also compiles with `-Werror=frame-larger-than=2048`.

`make test-sandbox` passed with the host musl compiler and host Linux UAPI
headers supplied by a test compiler wrapper. This includes the BPF, UTS, PID,
seccomp, IPC, mount, capability/network and packaged bubblewrap checks, plus the
new signalfd lifetime test. The shared guest runner accepts AVORY_TEST_CC and
falls back to a system musl compiler when the old repository-local toolchain
is absent. Evidence is in `build/epiphany-recheck/sandbox-final.log`.

The full private Xorg/D-Bus/Epiphany launch with the runtime directory on `/tmp`
passed three live browser/WebKit-process samples with a fresh cache and GTK's
Cairo renderer. See `build/epiphany-recheck/report-serial.log` and its
`EPIPHANY_FULL: PASS live browser and WebKitWebProcess` marker. This verifies
process startup and sandbox membership, not pixel-level page rendering.

Full tests with the default software Vulkan renderer exposed additional heap
corruption/LLVM failures and a later VMA-tree fault during proc inspection.
Those failures are unresolved; successful sandbox regressions do not establish
that default-renderer browser startup is reliable. Cairo was a test-only setting
and was not installed as a workaround.

Follow-up 2026-10-17: the VMA-tree fault and the corruption share one root
cause — shared tmpfs/memfd mappings installed filesystem frames without a
per-PTE PMM reference, so truncate/realloc released frames that were still
mapped and writable. That is fixed, with a deterministic guest regression
(`scripts/tests/test_shared_mmap_lifetime.py`) and a full-workload re-run; see
`docs/shared-mmap-frame-ownership.md`.

The rebuilt `avoryos-x86_64.iso` contains the verified stripped kernel and the
existing boot configuration. It was replaced atomically so the user's running
VM retains its opened ISO inode until reboot. The prior ISO is saved at
`build/epiphany-recheck/avoryos-before.iso`; the running VM and `disk.img` were
not modified.
