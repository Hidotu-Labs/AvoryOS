# Roblox blocked-I/O audit (2026-10-07)

The supplied manual LOCKDIAG capture has eight responsive, idle CPUs. The VM,
GPU and shootdown spotlight locks are free, with no wait-queue lock holder.
Roblox has many futex sleepers, plus a thread blocked in pwrite64 and an
HttpClient thread blocked in write. This does not prove a spinlock deadlock;
worker-pool futex sleeps alone are normal. The syscall error table is capped at
128 distinct entries and contains startup probes, so it does not identify a
specific Roblox syscall compatibility failure.

NVMe command waits and namespace partial-LBA read-modify-write waits published
THREAD_BLOCKED before registering the wake queue and timeout, with local
interrupts enabled. A timer-driven context switch between those steps can park
the thread without either mechanism to resume registration. An RMW contender
could also acquire the lock on its second check and remain BLOCKED until after
interrupts were enabled, potentially parking a new lock owner.

Both paths now mask local interrupts through deadline publication, blocked
state, wake registration, and the completion/lock recheck. A completed command
or newly acquired RMW owner is marked RUNNING and has its deadline cleared
before interrupt restoration. Actual sleeping happens after restoring the
caller's interrupt state. Changed command ordering and controller recovery
policy are outside this fix.

LOCKDIAG now gives non-futex waits priority within its bounded 24-thread list
and prints saved kernel-stack candidates for those waits. These addresses are
best-effort scan results, not a precise stack unwind. Resolve against the ELF
matching the booted kernel.

The test extracts the actual two wait functions and compiles them against
scheduler, interrupt and device mocks. It checks registration with interrupts
masked, an armed deadline, ordinary sleeping, completion during registration,
and an RMW release during registration. The old code fails the registration
assertion; the modified code passes AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
python3 scripts/tests/test_nvme_wait_registration.py
```

The production ISO builds successfully. A disposable eight-CPU QEMU test uses
fresh allocated files on ext3 over NVMe and passes eight concurrent workers
performing 240 writes, fsyncs, and verified readbacks. Its temporary test kernel omits unrelated LinuxKPI boot selftests;
the production ISO retains them. The exact Roblox stall is not yet reproduced
or proven resolved by these checks.

## Join capture after the NVMe fix

The next capture still has eight idle responsive CPUs and free spotlight locks.
Resolving its addresses against the current kernel places `Main` in
`eventfd_read` and `HttpClient` in `ext4_write_impl` / `ext4_alloc_extent`.
The latter is a file write, despite the thread's network-oriented name. The
capture does not include the deeper allocation/wait frames or the journal owner,
so it cannot establish which dependency stopped game joining.

The former eight-address parked-stack scan started at the top of the stack,
reporting oldest syscall frames and data pointers. Parked-stack scans now begin
at the saved RSP, print up to 16 candidates, and filter against linker-provided
text bounds. The blocked thread report prioritizes the journal owner's process
(or a blocked writer's process), includes its futex workers, and has a 128-thread
limit. Each entry includes CPU and wake deadline; the ext4 root journal includes
owner TID/comm, active state, depth, block count, phase and start call site.

SYSDIAG's 128 error keys previously filled with startup probes and stopped
recording new keys. It now replaces the least recently observed key, resets the
slot before reuse, and reports the eviction count. Counts for retained keys
cover their current residency; the aggregate error count still covers all
observations. This allows later application failures to appear in captures.

These diagnostic changes build successfully. They do not claim to fix the Join
freeze. A new capture from the rebuilt ISO while Join is stuck is needed to
identify the journal owner / deepest blocked call before changing more I/O or
synchronization behavior.

## Capture with parked-stack and wake deadlines

The following capture exposes a Roblox `Main` futex sleeper with
`wake_at_ms=1791393138191`, while kernel uptime is about 60 seconds. This is a
realtime epoch timestamp stored directly in a scheduler queue that expects
uptime milliseconds. `FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME` previously used
the same absolute-time arithmetic as monotonic waits, without subtracting the
RTC boot epoch. A timed condition-variable wait could consequently sleep far
beyond the caller's intended deadline.

Futex waits now convert realtime absolute deadlines into uptime, retain
monotonic absolute and relative semantics, round fractional milliseconds up,
validate timespec fields, and saturate very large durations. The requeue wait
path also uses its required absolute-deadline interpretation. The existing PI
futex implementation is unchanged.

Guest validation then exposed a second bug: a 30-ms timed wait could return
ETIMEDOUT after 1–3 ms on an unrelated scheduler wake. A waiter still registered
in its futex bucket now re-arms under that lock until its actual deadline or a
pending signal, instead of treating any reschedule as expiration. A matching
futex wake still removes the waiter and completes normally.

The actual clock-conversion helper passes sanitizer checks via
`scripts/tests/test_futex_clock.py`. The eight-CPU disposable QEMU guest passes
30-ms realtime absolute, monotonic absolute and relative waits (measured
at 30–31 ms), invalid/past timespec checks, and a matching wake before a future
realtime deadline via `userland/test_futex_clock.c`. The earlier guest test
failed with an early realtime timeout before the re-arm fix. The test is built
and installed by the normal disk-image recipe as `/bin/test_futex_clock`.

The capture also has a writer parked in NVMe flush during ext4 allocation;
that snapshot alone does not show a controller failure. No crash or hung CPU
is reported. These futex failures are confirmed kernel issues affecting Roblox;
normal-client game joining still needs retesting before claiming the entire
freeze is resolved.

## Remaining errors after futex clock fixes

The next report's wake deadlines are uptime values and the futex timeout count
is greatly reduced. CPUs respond and tracked locks are free. Roblox still has
an NVMe-flush writer, ordinary polls/futex waits, and cache-thread rename calls
returning EXDEV. There is no confirmed new hung lock in this capture.

Two concrete bugs were fixed:

- `do_poll` could publish BLOCKED before linking wake queues and arming its
  safety deadline with local interrupts enabled. Registration now masks local
  interrupts, arms the deadline before BLOCKED, links the queues, and restores
  interrupts before the readiness recheck and sleep.
- ext2/ext4 directory lookups may return distinct VFS handles for one directory.
  The syscall's pointer-based same-parent check then dispatched an ordinary
  rename to a cross-directory path supporting only tmpfs, returning EXDEV.
  VFS now recognizes identical backend/mount/inode directory handles and uses
  the existing serialized ext2 rename. True cross-directory ext2/ext4 moves
  remain unsupported; cross-mount moves still return EXDEV.

Zero-fd `poll` now uses the ordinary deadline/signal path: an infinite request
previously returned immediately, and the finite sleep used an unsafe
state/deadline publication sequence.

Roblox also repeatedly probes `/proc/self/oom_score`. A read-only neutral `0`
score stub is now exposed alongside the existing `oom_score_adj` stub. No OOM
victim-selection policy is implemented, and this does not emulate Linux's
memory-pressure scoring.

`scripts/tests/test_poll_rename.py` compiles actual poll/router functions with
mocks and passes sanitizer checks for raced readiness, sleeping/waking,
timeout/signal behavior, directory aliases, and mount/directory distinctions.
Each relevant case fails against its original source. An eight-CPU disposable
ext3/NVMe guest passes an actual rename/readback, pipe readiness wake, zero-fd
finite timeout. The oom_score guest probe could not be validated: the minimal
fixture initially had no mounted procfs, and adding its mount directory exposed
a kernel fault and repeated init launch before a reliable check completed.
The neutral score reuses the existing score-adjustment read stub. The production
ISO builds.

## Game-entry capture and futex requeue cleanup

The latest supplied report has responsive CPUs, free spotlight locks, a Main
thread waiting on a futex, and an HttpClient waiting for journal ownership in
ext4_write_impl. Bucket-wide wake counts include unrelated futex addresses and
cannot establish a lost wake for the Main thread. LOCKDIAG now prints each
futex sleeper's syscall address, operation and expected value without reading
userspace memory from the report handler.

Two further futex bugs have deterministic host reproductions:

- Timeout cleanup hashed the waiter's key before acquiring the bucket lock.
  A concurrent REQUEUE or CMP_REQUEUE could move it between those operations.
  Cleanup then searched the old list, falsely returned success, and left a
  pointer to its expired kernel-stack waiter on the new list. Each waiter now
  publishes its bucket atomically; cleanup locks and rechecks that bucket,
  retrying when it moved. Both ordinary and PI cleanup use this helper.
- WAKE and the wake portion of either requeue operation removed a registered
  waiter but counted it only if its thread was BLOCKED. A timeout or signal
  could already have made that thread runnable. This could return a false zero
  and consume more waiters than the requested limit. Every selected registered
  thread now counts, while sched_wakeup retains its runnable-thread guard.

`python3 scripts/tests/test_futex_requeue.py` compiles the actual functions and
forces relocation between the cleanup bucket read and lock acquisition. It
also checks timed rearming after an unrelated scheduler wake and wake limits
for all three operations. The original code fails the relocation and wake-limit
cases independently; the corrected code passes ASan/UBSan. The existing futex
clock regression passes too. These tests run on the host in temporary files.
Normal-client game joining has not been reproduced here, so these confirmed
kernel defects do not yet establish that the entire reported freeze is fixed.

## Ext4 failures while the Roblox background workers remain alive

The later screenshot shows flag fetching and a successful cache write after the
window stopped reacting. That establishes continued background activity, not a
responsive Main thread. The appended kernel log contains repeated extent
allocation failures for inode 186412 at logical blocks 14 and 1, `reason=3`.
In this implementation reason 3 means extent-tree insertion failed; reason 2
means physical allocation failed. The capture does not identify that inode's
pathname or establish that it belongs to Roblox. Journal contention includes a
Firefox SQLite worker and holds lasting 5–7 seconds.

Two actual ext4 write bugs were reproduced and fixed:

- Sparse writes reserved up to 32 blocks without stopping at the next logical
  extent. The reservation could overlap an existing mapping, fail insertion,
  or create an overlapping merged mapping. Allocation now caps the reservation
  at the hole boundary, including boundaries between indexed extent leaves.
- Writes read the inode before acquiring journal ownership and retained it
  across transaction batches. A competing writer's newly published mappings
  could be overwritten by this stale snapshot. Initial and subsequent snapshots
  now occur while owning the transaction. Data, metadata, journal and restart
  failures are checked; extent insertion errors return EIO rather than ENOSPC.

Cache publication also needs real cross-directory renames on one filesystem.
Regular files and symlinks can now move between ext2/ext4 directories in one
transaction. Destination replacement preserves existing open handles and uses
extent-aware directory mapping. Directory moves remain unsupported; different
mounts return EXDEV. Journal errors in directory insertion/removal propagate.

The actual-source sanitizer tests `test_ext4_sparse_write.py`,
`test_ext2_cross_rename.py` and `test_poll_rename.py` pass. Sparse allocation and
competing-writer snapshot cases fail independently against their saved original
sources. The production ISO rebuilds. Verification uses temporary host files
and a disposable ext4/NVMe QEMU image; the user's disk.img is untouched.

An eight-CPU QEMU guest using the rebuilt production kernel passes sparse
writes spanning an existing extent, four concurrent writers with complete
readback, cross-directory file movement, replacement, and readback through an
open descriptor referring to the old destination. The rename router pins the
cached source/destination objects before invalidating their entries, matching
same-directory rename's lifetime handling. The fixture and its disk are under
/tmp/mocktail-fs-guest. Normal Roblox game joining remains unverified; these
reproductions prove filesystem defects rather than exclusive causation of the
UI stall. Existing general VFS inode-alias lifetime semantics are outside this
focused rename change.

## Join-stall capture expansion (lockdiag + sysdiag)

Join wedges Main on a futex while workers sit in poll/write, and the journal
may be owned by an unrelated daemon, so a single journal-owner focus hides the
client. LOCKDIAG now also tracks the tgid with the most BLOCKED/SLEEPING
threads (`busiest blocked group`) and prints that group first with the journal
owner group. Per-thread lines add poll snapshots (`nfds + first 4 fd/ev/path`
from `last_poll_*`), fd/count for read/write/pread/pwrite, and `OVERDUE+ms`
when a deadline (`wakeup_ticks`) passes while still BLOCKED. Long-parked
futex waiters (>10 s) get parked stacks even when unfocused.

SYSDIAG adds a `join-stall signatures` subsection re-listing error keys for
game comms (mocktail/roblox/wine/sober, case-insensitive) plus
EIO/ENOSPC/EXDEV/ETIMEDOUT/EPIPE. Ordinary EAGAIN/EINTR/futex-timeout stays
out. Reproduce Join, trigger the hang report, and read the busiest group +
OVERDUE lines + signatures to name the dependency.

## Wedged journal after kill -9 (dead owner, live waiters)

`kill -9 872` left `[JBD2] transaction wait tid=1986 owner=969 comm='Main'
active=1 depth=2 hold_ms=89629`: the owner was a killed worker still holding
`trans->lock` with `active=1`. No exit path released it, so every later writer
blocked on `try_acquire` with repeating 5 s warnings, and the wait loop never
checked signals, so even SIGKILL could not free waiters (shutdown hangs).

Fix: `ext3_journal_release_owner()` aborts a dead owner's transaction
(discard staged blocks, wake waiters) from `sched_reap_thread()` — the one
place every dead thread passes through off-CPU, next to the existing
`futex_remove_thread_waiters`/`wait_queue_cleanup_thread` — plus an EINTR
return on pending signals in the `ext3_journal_start()` wait loop so wedged
waiters stay killable. `make kernel` passes. Rebuild the ISO, re-run Join,
and `kill -9` must now print one abort line and unblock the mount instead of
repeating the wait warning.

## Follow-up capture: file I/O waits and missing journal spotlight

The supplied eight-CPU report has live CPUs, free spotlight locks and no
wait-queue holders. Main tid 997 is parked in pwrite64 (fd 34, 24 bytes);
symbolizing its addresses against the local kernel points through
`ext4_write_impl` / extent allocation to `nvme_flush_once` and `nvme_io_wait`.
HttpClient tid 988 is parked in write (fd 30, 4096 bytes), with
`ext3_journal_start` in its stack. This makes storage and transaction ownership
a useful next lead, but one snapshot does not establish a permanent deadlock.
Parked stacks are best-effort scans and the exact boot ELF was not verified.

The root journal spotlight was accidentally gated on the regular-file
`ext4_read_impl` callback. Root directories lack that callback. It now identifies
ext2/ext4 mounts by their statfs callback and prints transaction `hold_ms`, so
the next report can expose the owner and phase behind journal waiters.

The join filter also contradicted its intended exclusions: the game-name
shortcut admitted EAGAIN/EINTR, and the generic ETIMEDOUT rule admitted normal
futex deadlines. Exclusions now run first. The subsection is named
`join investigation candidates` because inclusion alone is not evidence of
causation. The ordinary error aggregate still retains these results.

`test_join_diagnostics.py` exercises the actual filter and root-mount lookup
under ASan/UBSan. Existing futex clock/requeue, NVMe wait registration,
ext4 sparse-write and ext2 cache-coherence checks pass; the production ISO
rebuilds. No storage durability or futex return semantics changed in this
follow-up. Boot the rebuilt ISO and reproduce joining; if it still stalls,
capture the root journal owner/phase/hold_ms and adjacent JBD2/NVMe messages.
Successful joining remains unverified.

## Follow-up: live desktop, long client waits, and futex queue starvation

The next report (Mocktail tgid 1541) has eight probe acknowledgements, free
spotlight locks, no wait-queue holders, and no missing syscall/ioctl entries.
The user confirms that the desktop and terminal continue working while the
client remains frozen. Main tid 1541 and the llvmpipe workers have been parked
for roughly 6.8 seconds. Three asset-cache workers and HttpClient share mutex
address `0x7f019c000cc8`; their park ages range from 119 ms to 36 seconds.
The journal is owned by worker 1634 in pwrite64, with a 17 ms hold. Its stack
resolves through ext4 extent allocation and NVMe I/O. That short transaction
snapshot is not evidence of a permanent storage deadlock.

The futex implementation did contain a reproducible starvation mechanism:
WAIT inserted new waiters at the head, and WAKE selected the first matching
waiter. Repeated new arrivals could overtake an older registered waiter
indefinitely. REQUEUE and CMP_REQUEUE also prepended to the destination,
reversing arrivals and overtaking waiters already there. They now append
under the same bucket locks. Validation of expired/invalid WAIT deadlines
finishes before insertion so an error cannot unlink an unrelated waiter.
PI waiters already used FIFO insertion and their handoff policy is unchanged.

The actual-source `test_futex_requeue.py` test seeds an older waiter, enters
the real WAIT function for a newer waiter, and verifies that WAKE(1) wakes
the older one first. It fails against the saved pre-change source. Further
cases preserve destination and source order for both requeue operations,
including distinct keys hashing to the same bucket. Existing raced timeout,
wake limits and spurious wake tests pass under ASan/UBSan, as do the futex
clock, NVMe wait registration and join diagnostic checks. The kernel builds.

The report's `consumed` number was actually the count of wake calls finding
no match. It is now labelled `no-match`. These are lifetime totals for a
hash bucket, potentially containing many unrelated futex keys; neither the
totals nor a no-match wake establish a lost wake for the displayed thread.

This fixes a proven queue starvation mechanism, not a confirmed exclusive
cause of the supplied join stall. Boot the rebuilt ISO and retry joining.
If it remains frozen, two hang reports several seconds apart and the native
Mocktail/Roblox session log can distinguish progressing file writes from an
unchanged blocked dependency. Actual successful game joining is unverified.

## Long capture: client condition wait and audio mutex dependency

The `ecea79ed` capture identifies Main tid/tgid 1523 parked for 83,979 ms in
private WAIT_BITSET on `0x7f00fc5453cc`, with no deadline. The sampled word
still equals its expected value, `0xdd7`. Disassembly of the local active
2628 payload confirms that `libroblox.so` file offset `0x24848e8` is the
return from its indefinite futex syscall. Calls at `0x249029e`, `0x248663f`
and `0x2487d07` trace through the client task scheduler; `0x20bfb4d` is an
event/condition wait loop. This identifies the parked client path but does
not identify the event producer or prove a kernel lost wake. The report's
executable-address scan is not a stack unwind, and its 16-candidate limit
cuts off the outer caller.

Thread 1598 is parked for 82,135 ms on a different private WAIT mutex,
`0x7f02815b1e90`. Its sampled four words are `{2, 2, 1688, 2}`. With the
local glibc layout this suggests mutex owner tid 1688. The native callers
resolve to glibc condition-wait mutex reacquisition, libspa-support
`0x72bd`, and SDL3 `0x1a37e2`. SDL's disassembly places this in the PipeWire
audio-device readiness loop. The supplied text begins midway through a
thread entry and does not contain a heading for tid 1688, so an owner stack
and a complete dependency cycle cannot be established from this paste.

HttpClient's captured write is a 4096-byte write to
`/opt/mocktail/data/android/data/wob/wob-1523000048`, only 13 ms into the
syscall. Mesa cache workers are sleeping on their work condition. Neither
observation establishes the cause of Main's long wait.

A targeted guest experiment is `SDL_AUDIO_DRIVER=dummy mocktail` followed
by joining. The bundled SDL3 contains that exact environment-variable
name and the dummy audio backend; the launcher preserves this SDL3 variable
(it clears the legacy `SDL_AUDIODRIVER`). This disables audio for one launch
and tests involvement of the observed audio dependency. It is not a
permanent workaround or a verified fix. No synchronization, filesystem or
default audio behavior was changed on the strength of this capture.

The user subsequently confirmed successful joining with
`SDL_AUDIO_DRIVER=dummy mocktail`. Audio is therefore a confirmed
contributor to this reproduction, though this experiment does not identify
the complete deadlock cycle or prove which layer is defective. The launcher
now defaults to this verified silent workaround and preserves explicit
audio selections. The user also confirmed joining via PulseAudio but heard
no sound; direct ALSA has not been tested. Audible playback and the complete
deadlock cause remain unresolved. No speculative futex or storage changes
were made for this result.

## Error 262 after bypassing audio: preparing a 1.0.4 comparison

The subsequent screenshot reports "There was a problem sending data"
(Error 262). This is a different symptom from the indefinite audio wait.
The screenshot alone does not identify a failing syscall, a dropped packet,
or a client-version incompatibility.

The official Mocktail 1.0.4 release lists support for Roblox 2.736.1408 but
does not list an Error 262 fix. The downloaded AppImage matches the release
asset SHA-256
`07f1c93a00809b436d2f0bb5609f88be20ec3bf7126f253e6e81186d6cb54efe`.
Its extracted compatibility manifest allows 2.734.917 / code 2908 and
2.736.1408 / code 2998, in addition to the old supported profiles. The
installed 1.0.3 manifest only allows 2.725.1142 and 2.727.1199.

Installer preparation exposed a concrete upgrade bug: after populating the
new runtime into the partition copy, cleanup unconditionally issued
`rm_rf /opt/mocktail-1.0.4`. This would delete the selected runtime if debugfs
supports that command. Cleanup now skips `RUNTIME_INSTALL_DIR`, matching the
existing staged-tree guard. `test_mocktail_runtime_cleanup.py` exercises the
actual shell blocks with disposable fixtures for 1.0.3, 1.0.4 and continuous.
The original deletion reproduces separately; the corrected block preserves
1.0.4. Shell checks and all 12 session recovery tests pass.

The verified 1.0.4 download and extraction are in the build cache. The pin,
staged launchers and guest disk are still on 1.0.3; no upgrade was performed
against the running VM. After shutting down the guest and QEMU, a comparison
install can use `MOCKTAIL_VERSION=1.0.4 ./scripts/setup-mocktail.sh`. This
refreshes the shared selected Roblox payload through the 1.0.4 updater and
retains the audio workaround. Successful joining without Error 262 under
1.0.4 remains unverified.

## 1.0.4 webview abort: WebKit's separate namespace sandbox

The user installed 1.0.4 with the exact-supported Roblox 2.736.1408 payload.
The next capture shows `bwrap` failing `clone(0x2c020011)` with EPERM,
followed by `mocktail_webview_helper` aborting with "Failed to fully launch
dbus-proxy: Child process exited with code 1". Those clone flags request
mount, UTS, IPC and PID namespaces that Avory does not implement.

`MOCKTAIL_SKIP_NAMESPACE_CHECK=1` bypasses the bundle's namespace wrapper,
but does not disable WebKit's internal sandbox. The bundled
`libwebkitgtk-6.0.so.4.16.10` reads
`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` at ELF address 0x1bb77b0;
unset or exactly `0` proceeds to enable the sandbox, whereas `1` skips it.
The generated launcher now exports this variable with a default of `1`,
preserving an explicit override. This runs this application's webview
without WebKit's namespace sandbox; it does not implement kernel namespaces.
The staged primary launcher still selects 1.0.4. The running guest disk was
not edited. An immediate guest test uses
`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 mocktail`.

Shell syntax and child-process environment inheritance are checked locally.
Avoiding this abort in the guest and the later Error 262 remain unverified.

## Error 304 after the game starts: verified staging defect

The next screenshot reaches the game's interface, then reports missing or
corrupted files (Error 304). No corresponding client log has been supplied
yet, so the screenshot does not establish which file or check failed.

The selected 2998 host payload's library and APKs match updater metadata.
Its 1,836 assets hash to
`caee48e11c194bc11852493f53ff67d5e2bb233172f12b87ea5fca97c0c10840`.
The staged tree had 1,837 assets and instead hashed to
`d0adc7fe8e83740a212c7b4d5a625660ab8aea66dab0c48d29809f96be63eed9`.
The only differences were the installer's added `NotoSansCJK-Regular.otf`
and rewritten `NotoSansCJKFallback.json`. Mocktail's
[1.0.4 integrity implementation](https://github.com/komaruworld/mocktail/blob/1.0.4/src/update/payload_integrity.cc)
checks the asset count and this digest against metadata.

The installer no longer mutates these assets and now verifies library, APK
and asset hashes before and after staging. The staged assets were restored
from the verified host payload, retaining original metadata. The validator
rejects the actual previously modified tree and accepts the restored tree;
disposable fixtures also confirm rejection of added/changed assets and a
damaged APK. All 14 Mocktail session/cleanup tests and shell checks pass.

This repairs a proven payload integrity defect, but its relationship to
Roblox's Error 304 is still unverified. The running VM and disk.img were not
edited. After a clean guest shutdown and closing QEMU, use
`MOCKTAIL_VERSION=1.0.4 MOCKTAIL_REFRESH_PAYLOAD=0 MOCKTAIL_FORCE_PAYLOAD=1 ./scripts/setup-mocktail.sh`
to replace the same-ID guest payload as well as the launcher. The force flag
is required because the existing image selection already has ID 2998.

## Error 304 persists: dynamic procfs filesystem identity

The user confirmed the forced clean-payload reinstall completed successfully,
but a subsequent join still disconnects with Error 304. The screenshots show
repeated Blink Update queue warnings followed by the same Network disconnect
reason. They do not name a rejected file or establish that the queue warning
causes the disconnect. Restoring the staged assets did not resolve Error 304.

Mocktail's [FreeBSD notes](https://github.com/komaruworld/mocktail/blob/main/packaging/freebsd/README.md)
document Error 304 after roughly a minute when procfs reports the wrong Linux
filesystem magic. Avory already gives its procfs root `0x9fa0`, but the dynamic
`make_pid_dir` tree is constructed independently of that root using ramfs.
Its process directory, maps file and task tree consequently inherited RAMFS
magic (`0x858458f6`). Dynamically generated fd, fdinfo and namespace entries
also lacked a filesystem handler. Correct root reporting did not cover these.

All procfs nodes now receive the procfs handler during initialization, before
ramfs directory construction. This retains procfs identity on both static
and dynamic nodes without changing RAMFS elsewhere. The procfs statfs handler
also initializes the complete Linux ABI structure, including spare fields.
`scripts/tests/test_procfs_statfs.py` compiles the actual process constructors,
ramfs mounting code and statfs handler with host fixtures. It checks 31
directories, files and links, including maps, task stat and generated fd
links, under ASan/UBSan. The saved previous implementation fails the same
test on the process-directory filesystem magic; the fixed code passes.

The kernel and `avoryos-x86_64.iso` were rebuilt successfully. The user still
needs to boot the new kernel and test the disconnect: matching an upstream
cause and fixing this proven kernel defect does not yet confirm this instance
of Error 304 is resolved. No running guest disk contents were changed.
