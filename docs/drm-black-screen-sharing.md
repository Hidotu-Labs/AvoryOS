# Black QEMU display: shared-pixmap setup and native syscall restart

## 2026-10-13: interrupted PRIME attachment and native ABI fix

The user subsequently reports no more black screens after booting the fix.
Intermittent Minecraft/3D pixels remain. This is confirmation of improved
desktop startup, not evidence that the rendering corruption has been fixed.

The newer supplied log contains the previously missing failure stage twice:

```text
[GPUDBG] PRIME stage=5 detail=0x0 size=4096000 error=-512 tid=109
```

Stage 5 is `dma_buf_attach()`, including the exporter's pin operation in the
upstream dynamic-attachment path. Thread 109 is Xorg in this capture. Error
512 is **ERESTARTSYS**, an internal Linux syscall-restart result, not an errno
that libc should receive. The native syscall dispatcher previously passed it
straight through. The native signal flags also lacked `SA_RESTART` handling.

This is a concrete ABI defect on the observed failed desktop-sharing path.
[Upstream libdrm's drmIoctl and drmPrimeFDToHandle](https://android.googlesource.com/platform/external/libdrm/+/refs/heads/main/xf86drm.c)
retry EINTR/EAGAIN; they do not retry errno 512. Thus an interrupted attachment
could make Xorg abandon its shared desktop pixmap even though AMDGPU and
RadeonSI initialized successfully. A smaller black QEMU window is consistent
with a guest mode change during failed desktop setup; the screenshot alone
does not prove a particular mode or GPU initialization failure. The many
AMDGPU `Destination Rectangle` messages include mode probing and do not by
themselves identify the active VirtIO scanout mode.

The correction is in native syscall return and signal-frame construction:
ERESTARTSYS replays the original x86-64 SYSCALL after a handler with SA_RESTART,
or returns EINTR after a handler without it. ERESTARTNOINTR and ERESTARTNOHAND
receive their respective restart policies. Correction happens before saving
the user signal frame, retains the original syscall arguments, and covers
both ordinary and raw syscall exits. Sigreturn's restored user register values
are excluded; redirected rseq/fault return addresses are not rewound. These
rules follow [Linux's x86 signal implementation](https://raw.githubusercontent.com/torvalds/linux/v6.6/arch/x86/kernel/signal.c).

Native restart-block state and i386 custom signal frames remain unimplemented:
ERESTART_RESTARTBLOCK and compat32 internal restart results are converted to
EINTR rather than leaked or replayed with an incorrect syscall number. This
does not claim complete restart-block or compat32 signal support.

The actual-code ASan/UBSan regression passes. Captured pre-fix signal conversion
fails the same regression. A static native userspace test interrupts a real
LinuxKPI `dma_fence_wait_timeout()` ioctl with SIGUSR1, verifies EINTR and the
saved handler context, verifies SA_RESTART completion after fence signaling,
and verifies a blocked signal neither interrupts the wait nor gets lost.
All three cases pass in both one-CPU and eight-CPU KVM boots. Existing signal
ownership and syscall-user-dispatch regressions also pass.

Repeatable validation, after rebuilding the kernel ISO and test executable:

```sh
python3 scripts/tests/test_syscall_restart.py
python3 scripts/syscall-restart-boot.py --cpus 1
python3 scripts/syscall-restart-boot.py --cpus 8
```

The boot fixture uses a private disk, no physical GPU, and normal guest driver
shutdown. Results are preserved in `build/logs/syscall-restart-kvm{1,8}-result.log`
and `build/syscall-restart-boot/kvm{1,8}-serial.log`. The rebuilt main ISO contains
the fix. No Mesa/AMDGPU vendor patch or application setting is involved.

**Verification limit:** this proves native interruption/restart behavior, not
recovery of the affected physical AMD/Xorg session or removal of Minecraft's
pixel corruption. No additional physical GPU boot was performed. A stage-5
ERESTARTSYS record can still describe an intermediate interrupt before a
successful retry; that line alone will no longer establish an unrecovered
userspace error.

## Earlier shared-pixmap evidence and diagnostics

The supplied 2026-10-13 log starts after GPU initialization. The complete host
serial log confirms AMDGPU registered `/dev/dri/card2` and `renderD129`, and
the corrected completion self-tests passed. This is different from the PSP
and KIQ initialization failures in the earlier private diagnostic boots.

A read-only disk reader recovered the running guest's Xorg log and display
configuration, without stopping the VM or writing to its disk. Live disk
reads may lag unflushed guest writes; the recovered Xorg log has the same
kernel parameters and initialization sequence as the supplied serial data.
The files are preserved under `build/logs/drm-black-screen-live/`.

Xorg initializes AMD RadeonSI/glamor successfully, discovers the native VirtIO
output GPU, then reports `randr: failed to create shared pixmap` twice at
25.309 and 26.447 seconds. In the staged upstream Xorg source, that message
means `rrCreateSharedPixmap()` returned NULL, either when creating the source
pixmap or sharing it to the secondary GPU. The output-side modesetting path
imports the exported FD through `DRM_IOCTL_PRIME_FD_TO_HANDLE`. This identifies
the failed setup operation, but the existing logs do not distinguish source
allocation/export from native FD lookup/attachment/mapping/handle failure.

The AMD `dc_commit_streams: 0 streams` and `No FB bound` messages alone do not
prove initialization failed: this profile deliberately renders on AMD while
VirtIO presents the QEMU window. The optional pm-sysfs error also precedes
successful driver registration. The two Xorg sharing errors are the useful
new failure evidence. They do not establish the cause of the older intermittent
Minecraft pixels.

## Native diagnostic correction

The prior flight recorder recorded only successful native imports. Source
ioctl failures could disappear on wrap, and a user with a black screen could
not export the proc report. Native hooks now record failure stages for AMD GEM
creation/export, imported FD/size checks, exporter attachment/pinning,
scatterlist/CPU mapping, unsupported I/O-memory mapping, native allocation and
GEM handle allocation. Failed imports are pinned in the anomaly ring.

The first 16 shared-buffer setup errors per boot print stage, detail, size,
negative errno and thread directly to the existing host serial log. The
recorder starts enabled. No guest console command is needed to obtain those
first error records. Normal producer paths still perform no serial logging.
Record meanings and limits are in `gpu-flight-recorder.md`.

Actual-code regressions inject failures at allocation, attach, scatterlist,
vmap and I/O-memory rejection, checking the stage/errno and balanced cleanup
with ASan/UBSan. Recorder tests verify the 16-line limit, retained errors after
3,000 ordinary events, disabled recording, existing wrap/freeze/export rules
and pixel hashes. The actual ioctl bridge tests cover source allocation and
export failures without dereferencing invalid userspace arguments.

That instrumentation change was a diagnostic correction, not a verified
black-screen behavior fix.
No Mesa, AMDGPU vendor source, Xorg source, game preferences or installed guest
disk were changed. No new physical GPU boot or host reset is performed. The
rebuilt ISO will emit the missing failure stage on the next affected boot.

The eight-CPU KVM presentation fixture passes 400 concurrent flips and 400
damage commits, with 72 active host frames and the final 1,024,000 pixels
matching. Its expected invalid-FD self-test also exercises the new automatic
stage-3 EBADF line in the real kernel. It attaches no physical AMD GPU and
does not open the user's disk. Evidence is preserved as
`build/logs/prime-black-startup-kvm-{boot,host,result}.log`. This establishes
working instrumentation and controlled native presentation, not recovery of
the affected Xorg/AMDGPU session.
