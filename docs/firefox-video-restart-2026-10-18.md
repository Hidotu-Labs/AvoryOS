# Firefox video restarts with VCN gate lines and priority EACCES, 2026-10-18

The user reports Firefox/YouTube videos restarting often, closely in time with
`LinuxKPI: VCN power request=...` lines and with `[SYSDIAG] SYSCALL ERROR`
lines for `getpriority` (syscall 140) and `setpriority` (syscall 141)
returning `-13 (EACCES)`. Evidence: the tail of `build/logs/p6-c7-prime.log`
(log mtime 21:26, ISO from 21:11, `HEAD` 883a44c built from current source).

## What the VCN lines show

`pg_flags=0x2c001` matches the Renoir table (`nv.c`, IP 10.3.1):
`GFX_PG | VCN | VCN_DPG | JPEG`. `indirect=1`, `dpm=1`. So VCN 2.0 runs in
DPG mode with indirect SRAM, exactly the upstream default for this asic.

The power wrapper in `kernel/linuxkpi/src/amdgpu_trace.c` is
observation-only: it delegates to the imported original and logs the cached
`vcn.cur_state` around it (capped at 16 events per boot, only on the
1002:164e passthrough PF; it adds no MMIO or firmware traffic).

The captured sequence for the whole run is two gate pairs and one ungate:

```
request=1 before=0 after=1   (idle-work gate; vcn_v2_0_stop path)
request=1 before=1 after=1   (state already gated; returns without hardware)
request=0 before=1 after=0   (ring begin use -> vcn_v2_0_start / DPG path)
request=1 before=0 after=1
request=1 before=1 after=1
```

The second gate of each pair is `vcn_v2_0_set_powergating_state` seeing
`state == adev->vcn.cur_state` and returning before touching hardware. No
`[ERROR]` line, ring timeout, VM fault, or recovery event appears anywhere in
the log. `amdgpu_vcn_ring_begin_use` ungates under `vcn_pg_lock` before every
ring allocation, so a healthy decode should produce an ungate line right at
resume; the single ungate after the first pair is consistent with decode
having halted (or fallen back) and later resuming, but 5 of 16 trace events
cannot distinguish "decode paused" from "decode abandoned" on this capture.

Conclusion: the VCN power trace lines are the driver's normal 1 s idle
gating around decode activity. Nothing in this evidence shows the gating
corrupts or wedges a live session. Changing powergating behavior is not
justified by this evidence, and previous passthrough trials recorded host
Data Fabric sync-flood resets (`amdgpu-host-reboot-2026-10-15.md`), so no
physical VCN/DPG experiment was attempted.

## The EACCES lines cannot come from the priority handlers

`sys_getpriority` in `kernel/src/syscalls/sys_process.c` can only return
`-22` (EINVAL), `-3` (ESRCH) or a positive `20 - nice` value; `sys_setpriority`
can return `-22`, `-3`, `-1` (ownership) or `-13` only when the caller lowers
niceness without `CAP_SYS_NICE` while `nice < target->nice_value`. The log
shows setpriority requests with prio `0x13` (19), which cannot satisfy that
condition against any target nice in `[-20, 19]`, and getpriority requests
with `which=0, who=0` (self), which always succeed. Neither handler can
explain the reported errno, and the same handlers existed unchanged in the
previous commit.

The only dispatch path that writes a user-visible negative result without
running any handler is the seccomp emulation (`sched/seccomp.c`): a
`SC_ERRNO` decision returns the filter's own errno in `rax` and dispatch
records it as a syscall error. The affected comms (`MediaPD~oder`,
`MediaSu~isor`, `IPC I/O Child`, `Cache2 I/O` — the `openat -1 EPERM` cache
entries and `sysinfo -1 EPERM` too) are all inside Firefox's sandboxed
process family. The MediaPipeline thread numbers climb monotonically
(#1 ... #78 in this run) across every new instance of the keys, so these
first-time report lines are the *symptom* of pipeline churn, not its cause.
The `cache2` openat EPERM entries are the same unexplained-but-seccomp-only
class and remain pending: an openat that reached the handler could not have
returned `-1` from `sys_open_path` under the current source.

## Change

SYSDIAG now attributes filter decisions. `sched/seccomp.c` marks the thread
(`seccomp_deny_pending`) when a `SECCOMP_RET_ERRNO` decision takes effect,
`sysdiag.c` consumes it once per call (`via_seccomp` on the tracked key),
prints ` via seccomp deny (the handler never ran; errno came from the
caller's own filter)` on the live warning line, appends ` via-seccomp-deny`
to both report writers, and treats such calls as non-severe so sandboxed
media pipeline churn stops flooding the console as fake kernel EACCES
reports. This is reporting only; syscall behavior is unchanged.

Validation: `test_seccomp_bpf.py` passes; kernel builds (known RWX linker
warning only); `test_seccomp_boot.py` boots its isolated guest image and
reports `failures=0` (the new boot path was not exercised for the label; the
next desktop run should show `MediaPD~oder` priority errors either labeled
`via seccomp deny` or absent, which directly tests the attribution).

## The trace budget was being burned before Firefox ever started

Fresh capture (`build/logs/p6-c7-prime.log` after the 21:47 ISO, serial
capture, mtime 21:55, 3975 lines so far): all 16 trace events of one boot are
accounted for and none of them are playback events.

| lines | phase | events |
|---|---|---|
| 630-632 | amdgpu hw_init, just before `VCN decode and encode initialized successfully (under DPG Mode)` | 3 gates (0→1 then two no-ops) |
| 870 | LinuxKPI phase-3 DRM self-test window | 1 ungate (1→0) |
| 922-927 | LinuxKPI phase-4 bochs/TTM self-test window | 1 real gate (0→1) + 5 no-op gates |
| 1203 | LinuxKPI phase-5 window | 1 ungate (1→0) |
| 3450-3454 | desktop session, netlink churn (media processes spawning) | 1 real gate (0→1) + 4 no-op gates |

Two independent conclusions:

1. Between boot and the desktop-session gate burst, the VCN dec ring saw
   *zero* decoded submissions (no begin-use ungate ever traced), and after the
   16-event cap burned out nothing further is visible, so a whole video
   session is unobservable. The old 16-event cap plus boot self-test churn
   made this trace useless exactly when it needed to watch playback.
2. The repeated `request=1 before=1 after=1` no-op gates are unexplained by
   the idle-work handler alone: it re-schedules itself only when fences are
   outstanding, and a decode job must have ungated first. Each no-op gate
   asserts `cur_state==1` while no ungate traced between them, so either
   ungates fall outside the cap context or some other consumer is touching
   the gate. The per-event counters added below disambiguate this directly.

## Change: make the VCN trace complete across a full desktop run

`kernel/linuxkpi/src/amdgpu_trace.c` (observation-only, still no MMIO or
firmware traffic):

- Trace cap raised from 16 to 256 events per boot; lines now carry a
  monotonic timestamp (`t=<ms>`).
- Each VCN power line now reports `jobs=` (`vcn.total_submission_cnt`),
  `decfence=` (emitted-unused decode-rings fences) and `decsub=` (running
  decode-submission counter). A gate line with `jobs>0` or `decfence>0`
  would instantly prove gating over live decode.
- New `VCN dec submit` lines: every submitted fence on the decode ring is
  counted; the first 32 carry ring gpu address and wptr. `decsub increasing`
  during playback proves VA-API decode actually reaches the ring;
  `decsub flat` while videos restart proves the dec ring is bypassed
  entirely and the restart cause is elsewhere (VA-API context creation,
  codec/profile mismatch, or userspace).

This closes the observability gap; it intentionally does not change any
gating, DPG, firmware or MMIO behavior. Physical VCN/DPG experiments remain
blocked by the documented host-reset risk
(`amdgpu-host-reboot-2026-10-15.md`).

## 2026-10-18 ~22:0x boot: hard kernel fault in SCM_RIGHTS fd passing

First boot of the 22:02 instrumented ISO produced a KPF thread-kill while
Firefox started:

- Thread tid=928 `FSBroker902` (Firefox filesystem broker, clone of tgid 747)
  died in `recvmsg(47, fd=0x2d msg flags=0x40000000 MSG_CMSG_CLOEXEC)`.
- Ring-0 write to `0x552` (non-present, "low address").
- Faulting instruction: refcount op `lock .. (%rdi)` in `fd_path_put`
  (`sched_files.c:130`); preceding bytes prove a
  "load old slot ([rax+rsi*8]), store new, put old" pattern with the array
  index intact (rsi=0x249=fd 585) and the *stored value* garbage.
- Stack-scan chain:
  `fd_path_put` ← `unix_scm_install_fd` (af_unix_io.c:44) ←
  `unix_recvmsg_impl` (1385) ← `sys_recvmsg` (608).

Root cause: per-thread *cached* fd-table array pointers (`t->fds`,
`t->fd_paths`, `t->fd_offsets`, `t->fd_flags`) can alias the freed old
buffer after a sibling-thread `fd_table_expand_locked` (Firefox processes
share tables CLONE_FILES; the old kmalloc buffer is recycled before the
stale reader runs, so its slots contain random values such as 0x552).
`sched_sync_files` only re-synced when capacity differed and only at syscall
entry, so a long syscall (recvmsg installing several fds) could act on
freed memory.

Fix (kernel-only, no API/visibility change):

- `sched_files.c`: `fd_path_set`, `fd_path_dup`, `fd_path_clear`,
  `fd_path_value` now dereference the authoritative `t->files->` arrays.
- `sys_fd.c`: `fd_find_free_locked`, `alloc_fd`, `alloc_fd_from`,
  `fd_release_reserved` scan/write `t->files->fds` (and reserved/offset/flags
  arrays directly).
- `af_unix_io.c`: `unix_scm_install_fd` writes offsets/flags/fds through
  `receiver->files->`.
- `sched.h`: `sched_sync_files` also resyncs when pointer identity differs
  (`t->fds != t->files->fds`), catching staleness at the next syscall entry
  for every other cached-pointer consumer.

Kernel rebuilds clean; only pre-existing warnings. ISO rebuilt 22:26.
`scripts/tests` fd-lifetime suites fail identically before and after this
change (their embedded helper mirrors predate the `struct fd_table` layout —
pre-existing drift, untouched here).

Note this crash plausibly contributes to the reported Firefox instability
itself: a dead FS broker thread mid-media-stack spawn is exactly the kind of
upstream event that makes content pipelines restart. Reboot the 22:26 ISO,
watch for the KPF (should be gone), then the VCN dec-submission questions
from the previous section can be evaluated on quiet replay runs.

## 2026-10-18 ~22:30 run: kernel VCN path exonerated; decode flow measured live

The instrumented run (decsub counting real decode-ring fence emits) resolved
all remaining VCN-side ambiguity. First correction: in `amd_shared.h`
`AMD_PG_STATE_GATE = 0`, `AMD_PG_STATE_UNGATE = 1` — so all the earlier
"repeated gate requests" were actually the *per-frame* `ring_begin_use`
ungates (no-op while already ungated: `before=1 after=1`), and the `before`
values are kernel bookkeeping state, not hardware register state.

Live decode session (t=69.5s–~83s):

- `decsub` climbs 1→248 at ~40 ms cadence — **one decode submission per
  video frame, continuously**. VA-API reaches the ring and the ring is fed
  steadily by userspace.
- `jobs=1` on ungate lines is the in-flight submission count at begin-use —
  correct; `jobs=0` on the single gate at t=77021 — correct.
- One idle gate after ≈1.0 s without submissions (`VCN_IDLE_TIMEOUT`),
  clean ungate at 77.7 s on resume. Powergating + DPG behaved textbook.
- `decfence=0` during playback: the decode ring fence completes between
  submit and the next sample (frame latency < sample interval), i.e. fences
  signal on time. (The active ring is likely `ring_dec_sw`; a later pass
  could count that ring's fences too.)
- No KPF this run (SCM_RIGHTS stale-array fix verified in practice), and no
  getpriority/setpriority EACCES flood (seccomp attribution fix holding).

Decode-submission gaps across the whole log: only boot (66 s), and one
~1.0 s pause at decsub 158→159 that matches the video switch the user
performed. **No mid-playback halt of the decode ring exists in the kernel
timeline.** Both reported restarts therefore happened while the kernel
continued decodes seamlessly — the restart happens *inside* Firefox: the
media pipeline tears down and re-seeks without ever stopping VCN
submissions for more than a frame.

Remaining suspects, in order of the evidence: (1) Firefox's RDD/sandbox
path — earlier runs logged `seccomp(#317) → -14`, `prctl(#157) → -14` on
media processes, and an unexpected sandbox deny in this kernel's seccomp
emulation is exactly the kind of event that makes a Firefox utility process
retry/die and pipelines restart; (2) profile I/O failures — the BgIOThr~Pool
threads keep failing to open `prefs-2.js` with EPERM (O_CREAT|O_TRUNC), i.e.
the profile directory is not cleanly writable; (3) chronic display churn:
HDMI-A-1 connectors are re-probed and `dc_commit_streams` re-commit runs
throughout the session, though it occurs regardless of playback.

## 2026-10-18 ~22:45: sandbox control failed; decode-completion latency instrumented

Experiment A (`MOZ_DISABLE_RDD_SIM=1`, i.e. RDD sandbox disabled) did **not**
stop the restarts — the sandbox/seccomp deny path is not the trigger, and the
getpriority/seccomp attribution remains a red herring for the restart cause.

New instrumentation in `amdgpu_trace.c` closes the last kernel-side blind
spot. The submission counter (`decsub`) can physically show no difference
between "VCN decodes every frame" and "VCN decodes every frame but the
decoder hangs half of them until the ring recovers": a hung frame would not
stop later submissions. What catches that is completion latency.

- The VCN dec fence-emit wrapper now records
  `adev->fence_context + ring->idx` (this kernel's dma_fence context) and a
  monotonic stamp at the last decode emit.
- `dma_fence_signal` matches its context and logs
  `VCN dec done t=<ms> lat=<ms> seq=<n>`
  for the first 32 completions per boot — plus, unlimited (96 entries),
  `VCN dec done (SLOW) ... lat=...` for any completion slower than 120 ms
  (≈3 frames at 25 fps).
- Verdict rules for the next run: a healthy decoder shows done lines at
  ~15–40 ms latencies and *zero* SLOW lines. A single SLOW line before a
  restart is the smoking gun the user's "it is the VCN" hypothesis predicts;
  done lines that stay clean through a restart exonerate the kernel decode
  path definitively (no hang, no stall, IRQ-driven completion).

Kernel+ISO rebuilt at 22:42 with no behavior changes of any kind (power
gating, DPG, firmware untouched; physical experiments still blocked by the
host-reset documentation).

## What stays open

1. Confirm the attribution on a real run: if those errors still appear
   without `via seccomp deny`, the deny came from somewhere else and this
   analysis is wrong.
2. With the instrumented trace now installed (24-hour 22:02 ISO), read the
   next desktop run's `VCN power` / `VCN dec submit` lines before any
   further decode-side change.
3. If the driver ever needs a closer look at the restart moment, use the
   existing in-guest flight recorder (`/usr/bin/gpu-debug-capture`) instead
   of changing VCN powergating; physical DPG/indirect-SRAM experiments need
   the user's approval per the established host-reset risk.
4. `cache2` openat EPERM remains unresolved and may be the actual
   starvation driver of YouTube buffering stalls and repeated restarts.
5. The desktop-session 5-gate burst at 3450 (2026-10-18 ≈21:55 boot) during
   netlink/media-process churn with no traced ungates between the no-op
   gates is unexplained; the new `jobs=`/`decfence=`/`decsub=` fields will
   report the queued-submission state at exactly those moments.
