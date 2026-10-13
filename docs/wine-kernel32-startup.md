# Wine kernel32.dll startup failure

The supplied report shows repeated `kernel32.dll` lookup failures, valid-looking
small descriptors rejected by `fchdir`, and wineserver `EMFILE` from `pipe2` and
opening `cryptbase.dll`. All eight CPUs acknowledge the manual hang probe; this
snapshot does not show a system-wide deadlock. Missing `/dev/ntsync` and
`userfaultfd` support are probes and do not by themselves establish the cause.

The kernel had concrete descriptor-transport bugs affecting Wine's server/client
path:

* SCM_RIGHTS copied only a node pointer. It dropped the pathname used by
  `fchdir`, file status flags and saved position. The received directory could
  consequently return EBADF even though its node was valid. The queued message
  now snapshots metadata and pins the source under the file-table lock. Receive
  installs metadata before publishing the descriptor. CLOEXEC is determined by
  MSG_CMSG_CLOEXEC, rather than inherited from the sender. Invalid source FDs
  and message allocation failures now fail the send instead of silently sending
  payload without the expected rights.
* Relative `openat(dirfd, path)` recorded a pathname based on cwd rather than
  the directory descriptor. The stored path now uses the directory's pathname,
  so transferring it and later calling fchdir uses the intended directory.
* A plain stream read ending exactly at the first byte carrying SCM_RIGHTS
  discarded the pending descriptors. That byte is still unread. Rights are now
  discarded only after a read passes their boundary.
* Failed pipe/socket allocations did not restore the allocator's free-slot
  hint. With one slot left, repeated failed pipe2 calls could make a free slot
  appear exhausted. Reservations now roll back under the file-table lock, and
  socketpair failure closes its installed socket and restores the hint. The
  LinuxKPI reservation release also restores the hint. Minimum-descriptor
  allocations no longer skip unused lower slots by advancing the global hint.

The table still has a real MAX_FDS limit; genuine exhaustion still returns
EMFILE. Raising that limit would mask rather than repair the allocation bugs.
SCM metadata is a snapshot within AvoryOS's single filesystem namespace; this
patch does not introduce Linux-style shared open-file descriptions.

## Local verification

```
python3 scripts/tests/test_wine_fd_transport.py
python3 scripts/tests/test_fd_duplicate.py
python3 scripts/tests/test_file_cache_cow.py
make -C kernel -j4
```

The first test executes the actual allocation/SCM/fchdir functions with mocked
VFS nodes under address/undefined-behavior sanitizers. It covers pathname
survival after sender close, directory fchdir, status/CLOEXEC, reserved sources,
path-allocation failure, last-slot pipe rollback, high minimum allocations,
and the exact stream SCM boundary. LeakSanitizer is disabled because it cannot
run under the tool environment's ptrace. Existing close/dup race checks pass.

Guest Wine reproduction is left to the user, as requested. After booting the
rebuilt ISO, stop the old server and update the existing prefix:

```
wineserver -k
wineboot -u
wine cmd /c ver
```

Use the same WINEPREFIX for all three commands when testing a custom prefix.
No prefix files were removed by these fixes. These checks
establish kernel fixes relevant to the report; successful Wine startup remains
to be confirmed in the user's guest.

## Follow-up: damaged runtime and private mapping race

After the descriptor fixes, the supplied guest log shows successful `fchdir`
calls but Wine still fails to load kernel32. The `0x82307201` ioctl is Wine's
VFAT directory alias probe; ENOTTY on ext4 selects its normal directory-reading
fallback. ENOSYS from userfaultfd likewise disables optional kernel write-watch.

A read-only audit of the GPT root partition in the workspace's `disk.img`
found the installed `/usr/lib/wine/x86_64-windows/kernelbase.dll` differed from
the staged/package file in exactly ten bytes, all inside the DOS header. Its
MZ signature and PE-header pointer had been cleared. All other bytes matched,
and both architectures' remaining packaged Wine modules matched the staged
tree. This is a concrete invalid dependency of kernel32, independently of the
optional syscall probes.

The first physical block of this file was restored from the matching staged
file without allocating blocks or changing inode metadata. The original block,
inode number and disk offset are retained in
`build/wine-kernelbase-header-backup.json`. The restored file's complete SHA-256
is `69f4761b4a3a22a039679803c1f699ab37124fc091bdc0b83d0d164f1928adc6`,
which also matches the cached Wine 11.19 package. The prefix was preserved.
The repair is in the disk image, so replacing only the ISO cannot repair a
different copy of the disk.

Separately, the read-only file-fault cluster could revalidate a neighbouring
VMA after a racing mprotect added write permission, then install its shared
cache frame writable even for MAP_PRIVATE. It now always installs private
cache frames read-only with CoW, using the current VMA permissions. The
regression executes the actual install loop with the permissions changed to
read/write and checks private isolation, executable permissions, shared writes,
remapped offsets/files, removed VMAs and failed installs. This confirms the
race fix, but the available evidence does not establish how the on-disk header
was originally damaged. Guest Wine startup still requires the user's test.

## Follow-up: parallel prefix initialization

The later notepad report reaches prefix initialization and graphics probing,
then aborts on ole32.CoInitialize, shell32.SHGetDesktopFolder and
advapi32.SystemFunction036 imports. The installed ole32, combase and advapi32
files match their staged package copies. CoInitialize has an implemented export;
Wine's loader can allocate an aborting stub when it cannot resolve an import
([Wine loader](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/loader.c)).
These messages therefore do not establish that Wine itself lacks these APIs.
The accompanying ntdll error `c000011f` is STATUS_TOO_MANY_OPENED_FILES; it may
also be secondary to the repeated crashes/debugger processes.

This follow-up fixes additional concrete kernel defects:

* ext2/ext4, ramfs, tmpfs, FAT, procfs and DRM directory callbacks returned
  filesystem-global scratch entries. The directory cursor/index lock does not
  protect the returned entry after the callback releases it, and different
  directories have different locks. Each thread now owns its result buffer,
  which remains valid across preemption and CPU migration until that thread's
  next directory callback. This prevents other threads from replacing names,
  inode numbers and file types before getdents copies them.
* unlink previously silently cleared matching descriptors in the caller without
  releasing their references or updating the free-slot hint. Unlink now only
  removes the name; descriptors and mappings keep the inode alive through the
  backend's existing deferred deletion mechanism. The epoll-specific close
  helper also clears metadata and lowers the hint under the table lock.
* Allocation and F_DUPFD now check below a stale hint before reporting EMFILE.
  Truly full tables remain limited to MAX_FDS. A bounded `[FD-LIMIT]` line
  reports occupied, reserved and free counts when allocation fails, without
  requiring a separate guest diagnostic command.
* Saved open paths collapse repeated slashes and `.` components. The reported
  system32 `/././...` growth can no longer exhaust the pathname buffer during
  repeated open(".")/fchdir cycles. `..` remains intact to preserve symlink
  traversal semantics. Oversized saved paths fail instead of being truncated.
  Open publishes its descriptor only after all metadata is installed, and
  rolls back the reservation/reference if pathname allocation fails.

Verification adds `scripts/tests/test_wine_directory_lifetime.py`, which
executes the real ext4 callback with two interleaved threads and checks unlink
with two open descriptors. The existing transport and duplication tests now
cover stale exhausted hints, high minimum allocations and 1,000 dot-directory
round trips. The kernel/ISO builds and ELF32, Wine mapping, CoW and lazy-FPU
regressions pass. Successful guest Wine startup remains unverified; the user
runs that test. The disk image and prefix are not modified by this follow-up.

## Diagnostic boot for the remaining dependency failure

The next user report still ends in kernel32 `c0000135`, without FD-limit
failures or the earlier dot-path growth. Neither that report nor its longer
version identifies the unresolved dependency. No additional startup fix is
claimed from those logs.

A boot with the exact `wine_loader_trace` command-line token now adds
`WINEDEBUG=err+module,warn+module,trace+loaddll` to Wine/preloader execs only
when the caller did not supply WINEDEBUG. Other executable environments and
ordinary boots are unchanged. Wine child processes inherit the setting;
explicit settings, including `WINEDEBUG=-all`, remain authoritative.
Relevant module warnings/errors and successful kernel32/kernelbase/ntdll loads
are mirrored from stderr to `[WINE-LOADER]` kernel-log lines. This also handles
writev's already-copied kernel payload, with fault-safe user copies for write,
1,023-byte records and a 128-record boot cap. Thus the user's ordinary launch
command can supply the missing loader evidence without a separate trace command.

The supplied ISO is built with this diagnostic token:

```
make -j4 avoryos-x86_64.iso KERNEL_CMDLINE='init=/sbin/init splash quiet wine_loader_trace'
python3 scripts/tests/test_wine_loader_trace.py
```

The diagnostic regression executes the actual helpers and checks exact boot
token parsing, unaffected non-Wine execs, inherited/explicit settings,
allocation-failure ownership, user-copy failure, kernel writev payloads,
message filtering and the record cap. Existing descriptor, ELF32, user-dispatch
and exit-user-access regressions pass. This is a diagnostic build; Wine startup
has not been fixed or verified by it. Rebuilding the ISO without the token
returns to ordinary boot behavior. No disk/prefix changes are made here.

The ISO command-line override also now replaces the primary entry’s existing
cmdline rather than creating a duplicate key. The diagnostic image retains
OpenRC init and its normal splash/quiet options. The actual awk recipe is
covered by `scripts/tests/test_iso_cmdline.py`.

### Explorer exception context (2026-10-10)

The next report gets through kernel32 initialization and faults in Explorer
at RIP zero, called from user32's window procedure trampoline. The handler
then faults in ntdll.so at file offset 0x828a reading address 0x10c.
Disassembly identifies that second fault as the absent WOW/16-bit TEB path.
AvoryOS was populating only CS in REG_CSGSFS and leaving its saved SS zero.
Wine compares saved SS with its native stack selector, so this made a native
64-bit exception take the 16-bit recovery path.

Signal delivery now packs CS, GS, FS and SS into their Linux ABI positions,
sets UC_SIGCONTEXT_SS and sets UC_STRICT_RESTORE_SS for native 64-bit code.
Signal return honors the saved SS, permits the flat-selector fallback for
legacy non-strict native contexts, and rejects invalid strict SS values
before IRETQ. AvoryOS currently supports the flat user data selector 0x23;
this does not add LDT/segmented execution support.
The flag and fallback rules follow the
[Linux x86 ucontext ABI](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/include/uapi/asm/ucontext.h).

`scripts/tests/test_signal_selectors.py` executes the real delivery and
restoration functions and verifies Wine's mode predicate, strict invalid
selectors, legacy fallback, native/compat selector packing and ordinary
register restoration. `test_signal_stack` also checks saved SS and flags
during SIGILL recovery. Its updated source passes on the Linux host; a guest
run is still pending. Only the kernel ISO is rebuilt here; the disk image's
test binary is refreshed on the user's normal disk-image build.

This fixes the identified secondary exception-handler fault. The initial
null window procedure and full Wine startup have not been verified in the
guest, and no claim is made that this resolves both.

### Remaining Explorer null callback from `wine cmd.exe`

The next report contains two instruction-fetch faults at RIP/address zero in
Explorer, tid 341. Both have return address `user32.dll+file-offset=0x5c9be`.
The installed user32 disassembly at `0x5c9b7` is
`call *0xb8(%rsp)` in `call_window_proc`; at the fault, the sixth argument
(the actual WNDPROC) is at interrupted RSP + `0xc0` and is zero. RDX is
`0x81` (WM_NCCREATE) on the first fault and `0x82` (WM_NCDESTROY) on the
second. The stack is inside a writable VMA with thousands of bytes to spare.
This establishes a null callback during window creation and its cleanup;
the preceding MAP_STACK lines do not establish stack exhaustion.

A read-only audit of the workspace disk's GPT root partition found exact
SHA-256 matches with the staged Wine 11.19 files for Explorer, user32,
win32u, ntdll.dll and ntdll.so. The audit did not modify the disk or prefix.
The supplied exception snapshot does not establish why the callback became
null, and there is no new runtime fix claimed for this report.

`wine_window_trace` is a separate opt-in diagnostic boot token. It adds
`WINEDEBUG=err+all,warn+module,warn+winstation,trace+loaddll,trace+win,trace+class,trace+msg`
to Wine/preloader execs that have no explicit WINEDEBUG. The class trace
includes registered window-procedure addresses, while window/message traces
show the route into creation. Relevant stderr is mirrored as `[WINE-WINDOW]`
records. Trace records have a 512-record budget and errors an independent
128-record budget, so early graphics traffic cannot consume the error budget.
The existing loader-record budget remains separate. Explicit WINEDEBUG
settings and ordinary boots keep their existing behavior.

```
make -j4 avoryos-x86_64.iso KERNEL_CMDLINE='init=/sbin/init splash quiet wine_loader_trace wine_window_trace'
python3 scripts/tests/test_wine_loader_trace.py
python3 scripts/tests/test_user_dispatch.py
python3 scripts/tests/test_file_cache_cow.py
python3 scripts/tests/test_iso_cmdline.py
```

These regressions pass, the kernel builds, and the stripped kernel extracted
from the ISO matches the current build. The ISO primary entry contains both
diagnostic tokens. A separate headless QEMU probe with a disposable qcow2
overlay could not start because another process holds the backing disk's
write lock; that probe did not reach guest execution. The user's desktop
reproduction remains required. Boot the rebuilt ISO and launch:

```
env -u WINEDEBUG wine cmd.exe
```

Retain the `[WINE-WINDOW]` and `[WINE-LOADER]` lines before the first
Explorer exception; they provide the missing evidence for a runtime fix.

### Follow-up: character-device identity suppressed every Wine diagnostic

The next supplied log repeats the same Explorer null callback with no Wine
records. Wine's `dlls/ntdll/unix/debug.c:init_options` disables debugging before
parsing WINEDEBUG when stderr and `/dev/null` are character devices with equal
`st_rdev`. AvoryOS's built-in terminal and null nodes all had inode/rdev zero.
The constructors now assign Linux device numbers to null/zero/random/urandom,
console/tty/tty0, framebuffer and PTYs. The tty lookup returns its registered
node, and statx reports character-device major/minor consistently with stat.
See [Wine's logging gate](https://github.com/wine-mirror/wine/blob/wine-11.19/dlls/ntdll/unix/debug.c).

`test_char_device_identity.py` executes actual constructors, device lookup,
fill_kstat and sys_statx with mocked storage; it checks pathname and fd metadata
and Wine's suppression predicate. It and the Wine diagnostic regression pass.

A reflink copy of the disk plus disposable qcow2 overlays allowed a VM probe
without modifying the original disk or prefix. A static launcher replaced only
an existing test executable in that copy. Before the fix it reported stderr=0,
null=0, suppressed=1. After the fix it reported stderr=1281, null=259,
suppressed=0, and Wine diagnostics appeared. Logs are in
`build/wine-window-diagnostic/probe-fixed-serial.log` and
`build/wine-window-diagnostic/probe-verbose-serial.log`.

The probe uncovered `Session object id doesn't match expected id 2/3` followed
by `Failed to get shared session object for window class` in wineboot.exe.
This points at shared-session memory coherence, but the probe did not reproduce
the user's Explorer callback fault and does not establish its root cause.
The Explorer crash is still unresolved. `wine_window_trace` now also enables
and mirrors `warn+winstation` so the user's next desktop reproduction includes
these errors. The normal OpenRC ISO is rebuilt with both diagnostic tokens;
boot it and run `env -u WINEDEBUG wine cmd.exe`, retaining the Wine lines
before the first exception.

### Confirmed fix: private memfd views missed later session updates

The QEMU session probe isolated a backing mismatch. Wineserver maps its
1,343,488-byte session memfd shared; clients map that same descriptor private
with MAP_FIXED. The shared-only memfd callback sent private mappings through
the generic VFS page cache, producing a copy detached from server writes.
Window-class objects published after the client's initial read therefore
remained invisible, causing the reported session-ID and class-registration
errors.

The memfd callback now maps private views directly onto live backing frames,
with read-only CoW PTEs and one PMM reference per PTE. A client's first write
copies the page; server writes remain visible before that write. Private
memfd mappings advertise their PTE reference ownership so both munmap paths
release the backing or private-copy references. PROT_NONE, NX and readonly
protection are retained. Invalid mapping ranges return their actual errno
without installing a VMA at a negative result address.

`scripts/tests/test_memfd_private_views.py` runs the actual mapper, the real
CoW reference/copy code and the actual batched unmap path under UBSan. It
checks later server publication, isolated client writes, balanced references,
offsets and protections. The mmap reservation regression also checks callback
error propagation. The descriptor transport, duplication, file-cache CoW,
character-device identity and Wine diagnostic regressions pass.

Before the fix, the QEMU probe fails to obtain shared session objects. After
the fix, the same prefix registers `#32769` and `Message` with non-null
procedures, and the explicit packaged cmd.exe exits zero. See
`build/wine-window-diagnostic/probe-cow-serial.log`. The final ownership build
also runs ordinary `wine cmd.exe /c ver` and the explicit packaged executable
successfully (both exit zero) in
`build/wine-window-diagnostic/probe-final-serial.log`, using a disposable prefix
whose system32 files were populated during a separate update attempt.

The original disk snapshot had an empty system32 directory, so ordinary cmd.exe
initially failed to load kernel32 while the explicit packaged executable
succeeded. The forced update with bootstrap mode populated that test prefix,
but later hung in a Wine Mono 32-bit helper with a general-protection fault.
This does not establish a complete prefix-update fix. The headless test also
records a later services.exe exception; full GUI startup remains unverified.
Only the reflink snapshot and disposable overlays were written during testing.

The additional user log contains repeated `recvmsg: Socket operation on
non-socket`, descriptor-exhaustion messages and secondary DLL-load failures.
The socket error was not reproduced by these isolated probes. Diagnostic
boots now log up to 16 `[WINE-FD] recvmsg` failures, including fd, node type
and name, with metadata copied under the file-table lock and logging outside
that lock. Existing `[FD-LIMIT]` records distinguish occupied, reserved and
free slots. Neither message is evidence that raising a descriptor limit will
fix the underlying fault.

The normal OpenRC ISO is rebuilt with `wine_loader_trace wine_window_trace`.
A verified copy is saved at
`build/wine-window-diagnostic/avoryos-wine-fix.iso` so later normal builds
cannot overwrite this test artifact's boot settings. Its stripped kernel
matches the current build, and its primary entry contains both tokens.
Reboot with it and retest `env -u WINEDEBUG wine cmd.exe`. Retain any new
`[WINE-FD]`, `[FD-LIMIT]` and `[WINE-WINDOW]` records. The confirmed session
coherence fix does not prove that the original Explorer null callback or the
additional socket failure is resolved.

### User verification and diagnostic cleanup

The user confirmed that Wine now works. The temporary loader/window boot
tokens, stderr mirrors, recvmsg fd snapshots, descriptor-limit dumps and memfd
debug prints have been removed. Routine directory-change, GS-base, stack-mmap
and guard-mprotect logs have also been removed. Kernel dumps for exceptions
delivered to a custom user handler require KLOG_VERBOSE; signal delivery and
fault records are unchanged.

Normal Wine/preloader launches now default to `WINEDEBUG=-all`, while an
explicit caller setting remains authoritative. The environment regression is
now `scripts/tests/test_wine_debug_env.py`; it replaces the removed trace
regression and checks quiet defaults, exact executable matching, inherited
settings, explicit overrides and allocation rollback. The trace commands in
earlier sections describe historical investigations and no longer enable
kernel Wine tracing.

The functional shared-session, CoW, mapping-reference and character-device
fixes remain in place. Their regressions, fd duplication/transport and mmap
reservation checks pass. Both `avoryos-x86_64.iso` and the saved
`build/wine-window-diagnostic/avoryos-wine-fix.iso` are rebuilt with the primary
entry `init=/sbin/init splash quiet`; the saved artifact now contains the quiet
build, with its kernel and boot configuration verified against the build.
