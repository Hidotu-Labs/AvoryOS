# C7 — NVK/Zink userspace

Started 2026-10-21. NVK discovery and deterministic Zink shader rendering now
pass on hardware. NVIDIA-primary Xorg/glamor, GLX shader readback and captured
VirtIO PRIME output pass in the isolated profile. Full installed KDE/session
restart and resource-baseline qualification are still pending; C7 is not complete.

The owner requested installation through `scripts/setup-arch.sh` and an offline
patch of the existing writable desktop disk. `--nouveau-userspace` installs the
focused graphics transaction without re-running account/session configuration.
The normal setup also now requests `vulkan-nouveau`.

Mesa, NVK, Radeon Vulkan, software Vulkan and Mesa implicit layers are aligned at
`1:26.2.5-1`. Existing Vulkan loader/tools `1.4.363.0-1` and Mesa utilities remain.
The cached repository database was reused; no rolling full-system upgrade ran.
The PackageKit refresh hook could not contact its absent host-side daemon; pacman
completed installation and the payload was independently checked.

`build/nouveau-c7/image-patch.json` records the activated image and package set.
All 105 regular payload files and 74 symlinks match the installed rootfs.
Recursive ELF dependency verification checks 49 paths; it does not establish
runtime ABI correctness or acceleration. The staged filesystem needed bitmap/
inode bookkeeping repair; the subsequent read-only fsck passes and all package
payloads were reverified before activation. Failed staging attempts never
overwrote the original image.

The original disk is retained at `build/nouveau-c7/rollback/disk.img` and the
original ISO/checksum/cmdline beside it. The active desktop ISO now contains the
C6-qualified private kernel
`f91d718ed95bba0e3cbb1e6505c58992dd693c56cb33f0b0a679a51c9a96411d`, including
correct registered DRM sysfs identities. No kernel source or host GPU policy
changed in this installation. Host preflight is healthy with no blockers.

Boot on the host with `make run-nouveau`. In the guest terminal:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nouveau_icd.json vulkaninfo --summary
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nouveau_icd.json MESA_LOADER_DRIVER_OVERRIDE=zink glxinfo -B
```

Keep these diagnostic overrides per-command. Require NVIDIA/NVK device identity; llvmpipe
or lavapipe is not a pass. Capture the full output or first failure. Discovery
alone is followed by deterministic offscreen Vulkan readback, Zink/OpenGL pixels,
then the selected desktop's open/close/restart and resource-lifetime checks.

## Executed shader and Xorg gates

`hardware-zink/` passes exact NVIDIA `10de:2803`/NVK Vulkan identity, twelve
64x64 RGBA shader frames and three independent EGL/OpenGL context lifetimes.
The software reference deliberately uses Zink/lavapipe and cannot qualify hardware.
`hardware-xorg-4/` also passes four exact GLX shader frames, NVIDIA NVK/Zink
glamor initialization, two 1280x800 full-frame QMP pixel captures, bounded Xorg
exit and ordered Nouveau removal/S5. These are real GPU shaders, not CE copies.

The server reports DRI3 1.0 and Present 1.2. Mesa's multibuffer Zink path needs
DRI3 1.2, so the selected Xorg route explicitly uses `LIBGL_KOPPER_DRI2=true`.
This is not a DRI3 1.2, zero-copy or atomic presentation claim. The conditional
session hook applies only to the generated NVIDIA profile and preserves explicit
overrides. AMD and native software profiles remain separate.

Automatic selection uses actual driver/BDF/render pairing, not card2 guesses.
`avory-nouveau-xorg` generates both GPU BusIDs and card paths from the inventory;
ambiguous/missing devices fall back to the native profile. `AVORY_XORG_PROFILE=native`
remains the explicit escape hatch. No NVIDIA connector is fabricated.

Failed private runs remain failed: initial Xorg packaging lacked its config
directory, GBM backend and XKB data. A subsequent failed startup exposed a
shutdown fault: retired address spaces still held GEM/file references while
Nouveau removed its kernel copy channel. Shutdown now drains queued and in-flight
deferred VMA releases before PCI teardown. The waiter sleeps rather than remaining
runnable, allowing an idle-CPU drainer to finish on one CPU. Concurrent lifetime
regressions cover that ordering. This does not forcibly revoke live clients or
claim hot-unplug recovery.

## Final installed candidate

`readiness-xorg/result.json` and `hardware-final/result.json` pass with kernel
`b860f9daf545e016b01425c8dcf9d54aa7a1111e07c7d4353968578ee8017493`.
The final run uses the production automatic selector (`xorg-profile: nouveau`),
not fixed card/PCI guesses, and removes forced ICD/Gallium/loader overrides
before GLX. Sixteen shader-frame readbacks and two full-frame captures pass,
followed by Xorg exit, deferred mapping completion and GSP removal/S5.
Post-run host preflight has no blockers. No host reboot or reset policy change
was needed. This candidate also fixes idle-worker starvation in the shutdown
barrier, which failed the previous concurrent software readiness attempts.

The normal ISO contains this kernel. `autodetect-patch/result.json` records
the verified disk changes and rollback location. User settings/accounts remain
on the existing disk; only the selector, NVIDIA profile and session policy are
added/updated. The previous disk/ISO are retained under
`build/nouveau-c7/rollback-before-autodetect/`; the pre-C7 originals remain under
`build/nouveau-c7/rollback/`.

Boot with `make run-nouveau`, open the normal desktop, and run `glxinfo -B`
without overrides. Require Zink/NVK/RTX 4060 Ti rather than llvmpipe. Full KDE
session open/close/restart and resource-baseline acceptance remain pending;
the isolated Xorg test is not a full desktop-session completion claim.

## Whole-desktop freeze investigation (2026-10-22)

The owner reports successful Minecraft and vkcube rendering but intermittent
whole-desktop freezes. The retained desktop serial log has no clear GPU fault
or fence timeout identifying the blocked path. This is not a fixed/closed issue.
The launcher had been changed to eight vCPUs, outside the one-vCPU qualification.
`NOUVEAU_SMP` was then defaulted to one; an explicit `NOUVEAU_SMP=8` retained a controlled
SMP comparison. That change is a diagnostic control, not proof of an SMP fix.

At the owner's subsequent request, the interactive `run-nouveau` target now
defaults to eight vCPUs. `make run-nouveau NOUVEAU_SMP=1` retains the diagnostic
fallback. This default change does not extend the one-vCPU hardware qualification
to eight vCPUs; private readiness/test targets keep their existing CPU counts.

The desktop serial channel now has a host Unix socket while preserving its
append logfile. During a freeze, leave QEMU and the app running and request:

```sh
python3 scripts/prime-dump.py --nouveau
```

Inspect the resulting `build/logs/nouveau-c4.log` for blocked threads/locks before
changing GPU fencing or presentation behavior. No disk, kernel image, host reset
policy or GPU binding is changed by this diagnostic launcher update.

### Reproduced channel failure (October 22)

The next one-vCPU reproduction does contain a GPU error, unlike the previous
snapshot. The preserved log is
`build/nouveau-c7/freeze-20261022/serial.log`. GSP reports
`rc engn:00000001 chid:4 gfid:0 level:2 type:31`, address
`0000003ff59d1000`, fault type `00000002`, followed by
`Xorg.wrap[207]: channel 4 killed!`. Its diagnostic payload includes
`PBDMA_HANG_DURING_HTE`. These raw fields do not establish which mapping or
submission operation caused the failure.

At the manual dump, Xorg tid 207 sleeps inside
`drm_syncobj_array_wait_timeout` (resolved against the installed kernel ELF),
and KWin tid 475 waits in libxcb for Xorg. The reporting CPU remains alive and
the tracked GPU locks are free. This establishes a channel failure followed by
a blocked display server, not a proven generic futex or SMP deadlock. Whether
the unsatisfied syncobj is directly associated with the killed channel remains
unverified. The owner closed QEMU after the capture; it did not exit itself.

C7 verification now rejects channel-kill, channel-disable and GSP recovery
reports even when pixel markers pass. This is a qualification correction,
not a freeze fix. GPU submission/mapping failure and recovery remain open;
no new kernel, desktop disk or host reset policy was installed for this finding.

### Stronger isolated reproduction (October 22)

`readiness-vkcube/result.json` and `hardware-vkcube/result.json` pass with the
same installed `b860f9da...` kernel, one vCPU and GPU function .0 only. The
hardware run executes the installed vkcube on the exact NVIDIA ICD for 600
frames, followed by 256 GLX swaps and Xorg pixmap allocation/fill/copy/free
iterations with pixel readback. Both final full-frame PRIME captures and
ordered guest teardown pass. The preceding `hardware-stress/` run also passes
the 256-update workload without vkcube. These runs use private images; the
installed desktop disk is unchanged.

The Xorg oracle now requires both workload markers, and the interactive C7
target uses the matching `readiness-vkcube` result. Earlier shorter test results
remain evidence for their original scope only. This reproduction still omits
KDE/KWin and Minecraft; passing it does not reproduce or fix the reported
desktop channel failure. A kernel change is not justified by these passes.

A full-desktop comparison was subsequently started through `run-nouveau`
with `-snapshot` and a private QMP socket. Its display is black, but the new
dump shows Xorg in epoll rather than the earlier syncobj wait; no new channel
kill appears in this capture. This is not yet a reproduction of the original
failure. The snapshot log is preserved as
`freeze-20261022/desktop-snapshot-serial.log`. Automated keyboard control was
blocked by safety review, so further input was stopped pending authorization.
The temporary VM remains open; no freeze-fix kernel has been installed.

### Reproduced desktop load and bind-wait candidate

The owner authorized controlling the disposable desktop VM. Running the
existing PRIME output helper restored its display. A single 3,600-frame vkcube
run completed; four concurrent 10,000-frame instances then reproduced the
same Xorg channel-4 failure at GPU address `0000003ffa400000`. Two comparison
dumps retain Xorg sleeping inside the syncobj ioctl while the kernel stays
alive. Evidence is in
`freeze-20261022/desktop-multicube-two-dumps.log`. This does not require
Minecraft. The owner subsequently closed the test VM.

The queued synchronous path in the pristine imported `nouveau_job_submit`
calls `dma_fence_wait(done_fence, true)` and ignores its result before returning
success. An actionable signal can therefore leave the mapping unfinished
while reporting successful synchronous completion. This contradicts the
[synchronous VM_BIND completion contract](https://docs.kernel.org/gpu/drm-vm-bind-async.html).

The candidate LinuxKPI adapter scopes its replacement wait to the Nouveau
scheduler object. It preserves the ordinary interruptible wait but, if that
wait returns `-ERESTARTSYS`, waits uninterruptibly for the already-queued job
before permitting success. Other drivers, finite waits and vendor source
files are unchanged. `test_nouveau_bind_wait.py` fault-injects the interruption
and verifies completion before return and preservation of other paths.

At this stage the contract bug was established, but not its relationship to
the desktop freeze. A private automatic
KDE image avoids keyboard interference and original-disk writes. Its first
VM-traced four-vkcube run completed without a fault, so timing-sensitive
baseline and candidate comparisons remain necessary before claiming a fix.

### Activated bind-wait correction (October 22)

The first candidate completed three batches of four concurrent 10,000-frame
vkcube instances in the full installed KDE stack, with 28 intercepted interrupted
Xorg bind waits and no channel failure. The final candidate limits recovery
messages to the first four, rather than depending on verbose logging to change
timing. It again completed all twelve runs without a channel failure. Its
mapped, viewable Plasma panel appeared 11,346 ms after the session launch; a QMP
screenshot independently shows the panel and four vkcube windows. This measures
the private desktop copy with its existing cache, not a cold-cache guarantee.

Final kernel SHA-256 is
`004b53a9b35b262382811dc18f69dd2f78fd76c0320861166ed29d95be142c59`.
`bind-wait-candidate/readiness-final/result.json` and
`bind-wait-candidate/hardware-final/result.json` pass on that exact kernel.
The latter also passes the 600-frame standalone Vulkan workload, 256 GLX/Xorg
copy/readback iterations, sixteen shader-frame checks, both exact full-frame
captures, bounded Xorg exit and ordered GSP removal/S5. The interrupt-injection
regression verifies recovery still works after the logging limit is reached.

`bind-wait-candidate/activation.json` records installation into the normal ISO
and `build/nouveau-618/kernel`. The old installed ISO, checksum, command line
and kernel are retained under `bind-wait-candidate/rollback/`. Neither disk.img
nor host reset/binding/power settings were modified. Post-test host preflight
is healthy. `make run-nouveau` boots the correction; the interactive C7 target
now selects its matching software readiness result. Earlier C6/C7 artifacts
remain scoped to their recorded kernel hashes and must not be relabeled.

This fixes the demonstrated premature-success synchronous bind path and passes
the reproduced desktop workload. Extended installed Minecraft play, cold-cache
startup comparisons and broader session/resource qualification remain separate;
the private test is not proof that every possible desktop freeze is eliminated.

## Rebuilt desktop disks

The subsequent kernel-side presentation audit and damage-aware candidate are
recorded in [PRIME performance investigation](prime-performance.md).

### SDDM greeter environment

The Qt6 SDDM greeter can abort in its GLX integration with `failed to load
driver: zink` even when logged-in applications render correctly. SDDM starts
the greeter through a separate helper environment; the login-shell and Xinit
hooks do not reliably configure it. The shared SDDM drop-in now explicitly
sets `GreeterEnvironment=NOUVEAU_USE_ZINK=1,LIBGL_KOPPER_DRI2=true` under
`[General]`. This uses SDDM's supported greeter-environment setting, not a
software Qt Quick backend or a global Mesa driver override. The Nouveau
selection flag does not select Zink for AMD's native driver.

Fresh desktop setup, cached disk-image overlays and the focused
`setup-arch.sh --nouveau-userspace` repair all install this drop-in. Custom
`GreeterEnvironment` entries in `/etc/sddm.conf` take precedence and must retain
the needed compatibility variables if overridden. Upstream setting and helper
handling: [SDDM configuration](https://github.com/sddm/sddm/blob/develop/src/common/Configuration.h),
[SDDM helper](https://github.com/sddm/sddm/blob/develop/src/helper/Backend.cpp).

The real SDDM daemon was tested with these variables removed from its parent
environment, using the installed kernel and a private snapshot. Its Qt6 greeter
remained the same process across two samples, ran as UID/GID 963, did not abort,
and was followed by clean Nouveau removal and S5. The first harness incorrectly
compared all shader-thread IDs; the corrected test compares process-group IDs.
Both logs are preserved under `build/nouveau-c7/prime-damage/greeter*`.

The validated drop-in was installed atomically into `disk.img` and the cached
Arch rootfs. Only its existing data blocks and inode were updated in a private
copy before replacement; a rollback disk and activation manifest are retained
under `build/nouveau-c7/sddm-greeter-fix/`. The kernel, normal ISO, GPU bindings
and host power/reset policy were not changed by this greeter fix.
Read-only filesystem checks of the installed disk and its rollback both return
4 with identical diagnostics after normalizing the image filename. Those
pre-existing filesystem inconsistencies were not introduced or repaired by
this configuration change; both reports accompany the activation manifest.

The standard `disk.img` firmware manifest now includes the four AD106 GSP
570.144 requests and the NVIDIA redistribution notice, alongside the existing
AMD firmware. The AD106 request names map to the canonical upstream GA102/AD102
files; every installed byte is checked against the pinned 20260916 hashes.
Sparse firmware caches are expanded to include the manifest's source paths.
This applies to fresh and cached Arch desktop builds and installer/live images
derived from that disk, without a separate post-build firmware injection.

Both Arch setup modes and the disk overlay install the conditional Nouveau
environment hook in `/etc/profile.d/avory-nouveau.sh` as well as Xinit's hook
directory. Manual Xorg launches from login shells therefore inherit
`NOUVEAU_USE_ZINK=1` and `LIBGL_KOPPER_DRI2=true` when early init selected
`NouveauPrimary`. Explicit values and AMD/native fallback remain unchanged.

A boot reporting `gsp-570.144.bin unavailable`, probe result `-2`, and
`xorg-profile: native` is missing firmware, not merely an Xorg override.
Rebuild with `make disk.img`, then boot the qualified Nouveau kernel with
`make run-nouveau`. Changing the disk does not enable Nouveau in a generic
kernel or add a passthrough GPU to a non-VFIO launch. The separate Alpine
minimal image is not the qualified Arch NVK/Zink desktop stack.
