# Headless GPU pipeline oracle

## Next step after black-screen recovery

The user reports the native syscall-restart fix removed the black screens,
but intermittent 3D pixels remain. Earlier affected-workload PRIME copy
hashes matched while flicker continued; they do not tell whether rendering
had already produced incorrect pixels. The simple AMD constants test passed,
but the stronger pipeline test below has not yet executed on physical AMD:
both previous private hardware boots failed initialization before it ran.

Use the already initialized AMD GPU in the existing guest. No additional
physical GPU boot, X11 restart, application preset or driver patch is needed.
The host transfer directory contains the unchanged probe, served on loopback
port 9007 for the current debugging session. Inside AvoryOS:

```sh
curl -f http://10.0.2.2:9007/test_gpu_pipeline -o /tmp/test_gpu_pipeline
chmod +x /tmp/test_gpu_pipeline
/tmp/test_gpu_pipeline > /tmp/gpu-pipeline.log 2>&1
cat /tmp/gpu-pipeline.log
```

The probe renders offscreen; no window or on-screen pattern is expected. The
printed comparisons and renderer identity are the evidence. Leave driver
options at their normal values for this baseline. Existing debug overrides
in the shell would change the experiment; do not interpret an override run
as a default-path baseline.

| Observation | Investigation it motivates |
|---|---|
| Texture/mipmap/fog mismatches | Texture upload, descriptors, image layout/compression, sampling and shader execution |
| Uniform reads wrong, storage and CPU reads correct | Scalar/uniform-cache visibility or the uniform descriptor route |
| Asynchronous cases fail, serialized control passes | Submission dependencies, cache visibility or resource lifetime; serialization changes timing too |
| CPU input differs from uploaded values | CPU mapping, writes, allocation lifetime or memory corruption |
| All comparisons match with observed overlap | The tested operations passed; capture an affected application draw rather than claiming the whole renderer correct |
| No previous fence observed pending | Asynchronous coverage is inconclusive, regardless of matching values |

None of these results alone assigns blame to Mesa, LinuxKPI, firmware or
hardware. If the bounded oracle passes, the next useful artifact is an API
capture reproducing a bad application draw, following
[Mesa's misrendering debugging guidance](https://docs.mesa3d.org/graphics-debugging/debugging-misrenderings-crashes.html).
A capture that removes the symptom is also a timing observation; do not label
it a fix. Keep comparisons within the same reproducible scene and change only
one diagnostic variable per run.

## Earlier preparation and validation

The RCU correction did not resolve the user's remaining pixel corruption.
The latest affected-workload boot log contains the corrected RCU marker and
its passing native lifetime test, so this report is from that kernel. The
pixel corruption remains unresolved. No new speculative kernel correction is
presented as its fix.

## Test coverage

`userland/test_gpu_pipeline.c` uses the original staged Mesa runtime through
GBM/EGL/GLES 3.1. It requires RadeonSI by default and does not present through
X11 or PRIME. Its own textures/samplers are chosen for an oracle; it writes no
game preferences and changes no driver environment for the hardware case.

- Texture uploads and updates reuse eight textures while rendering a batch
  without a per-draw CPU readback or finish. Ten explicitly initialized mip
  levels contain known texels and an alpha mask. Every output pixel is checked
  after the complete batch. A quarter-texel bias avoids nearest-sampling ties.
- A second texture pass checks trilinear mip selection and EXP2 fog with a
  small rounding tolerance. Spatially constant mip levels make the expected
  filter result unambiguous. It does not test implicit LOD derivatives, full
  Minecraft geometry, or all fixed-function compatibility shaders.
- Compute trials submit dependent integer work, update an explicit UBO, then
  submit a follow-up dispatch. The same source buffer is also bound as a
  read-only SSBO with divergent indices. GPU uniform reads, GPU storage reads,
  the unchanged CPU source, and the earlier compute workload have separate
  exact integer oracles. Both reused-buffer and fresh-buffer cases run.
- A serialized compute control waits between the two submissions only inside
  this diagnostic. It is not inserted into game/driver execution. A zero-time
  fence query records whether the previous dispatch was still pending before
  issuing the follow-up dispatch. This is an observation at that point, not
  proof the fence was pending at the later kernel submission instruction.

The memory barrier and readback follow the [Khronos buffer-update barrier
contract](https://www.khronos.org/assets/uploads/developers/library/2014-gdc/Khronos-OpenGL-ES-GDC-Mar14.pdf).
The [zero-time fence query](https://wikis.khronos.org/opengl/Sync_Object) does
not wait for completion. A matching result with no pending previous fences
is marked inconclusive for the asynchronous cases.

An [upstream RadeonSI correction](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/57fd11256bdf0ec1ff1908277e4963180252491f)
describes stale shader-cache reads on gfx1036/Mesa 26.2.4 when submissions
overlap. This motivates the explicit UBO cases. It is not applied here and
does not establish that it explains this user's corruption. The previous
simple fragment-uniform diagnostic did not cover that exact workload.

The software mode reduces work and validates the test's expected values; it
cannot validate AMD execution or submission overlap. Original Mesa 26.2.4
passed all software oracles in the staged Linux rootfs and in a private
eight-CPU AvoryOS KVM boot. The latter completed automatically with exit=0.
Its lightweight software compute runs observed no pending previous fence and
are correctly labelled inconclusive for overlap.

## Automatic private boot

`scripts/gpu-pipeline-boot.py` builds an ext2 root image containing the probe,
a static init runner, the recursive ELF dependency set and Mesa's dlopen
entry points. The libraries are copied unchanged from the staged rootfs.
It creates a separate BIOS/UEFI ISO containing the current kernel. The runner
disables the flight recorder, launches the probe and prints an exit/signal
marker to a host serial file. It then requests native guest power-off so
bound PCI drivers receive their shutdown callbacks before QEMU exits.
No guest console commands are needed.

```sh
# Prepare only, without starting or opening a VFIO device:
python3 scripts/gpu-pipeline-boot.py

# Validate the automatic boot with software only, no VFIO:
python3 scripts/gpu-pipeline-boot.py --software --run

# Physical AMD run, only after authorization to initialize this GPU:
python3 scripts/gpu-pipeline-boot.py --run
```

The hardware run uses the existing VFIO-bound `0000:0e:00.0`, the existing
VBIOS image and the same AMD initialization parameters as `run-c7-prime`.
It does not open `disk.img`, rebind devices, request privileged setup, or
explicitly force a PCI/bus reset. QEMU's normal VFIO startup and shutdown
may reset the assigned device; AMD initialization itself has been reported
to sometimes reboot the host. That physical run requires separate approval.

Preparation/runs take an exclusive lock and refuse to overwrite a disk held
by a running diagnostic VM. Hardware execution refuses an existing QEMU owner
of the GPU. A timeout terminates only the private QEMU process started by this
script. The result is collected at `build/gpu-pipeline-boot/hardware-serial.log`
and `hardware-host.log` (or the corresponding `software-` files). The user's
existing VM and installed guest filesystem are not stopped or edited.

## Approved hardware attempt: initialization failure

The user authorized one automatic physical AMD boot on 2026-10-13. That run
completed with exit=2: `ERROR: no AMD render node; checks did not run`.
The kernel reached PSP initialization, logged `PSP create ring failed!`,
then `hw_init of IP block <psp> failed -22`, and tore down the device. The
probe executed no hardware pixel checks. The host remained running and the
script terminated its private QEMU process after the result marker. It did
not open the user's `disk.img` or rebind/reset the GPU explicitly.

Evidence:

- `build/logs/gpu-pipeline-native-amd.log`: automatic runner result.
- `build/gpu-pipeline-boot/attempt-1-psp-failure/hardware-serial.log`: complete guest boot, including
  the corrected RCU marker and passing native lifetime test.
- `build/gpu-pipeline-boot/attempt-1-psp-failure/hardware-host.log`: QEMU output and termination.

The `-22` does not identify the original failure: imported `psp_hw_init()`
converts every firmware-loading failure to `-EINVAL`. MP0 13.0.5 selects
`psp_v13_0_ring_create()`, whose non-SR-IOV path returns the result of
`psp_wait_for()`. That polling helper returns zero or `-ETIME` (-62), as
confirmed in the linked kernel's disassembly. The absent initial
`Failed to wait for trust OS ready for ring creation` message points to the
later ring-creation response wait, which requires `(C2PMSG_64 & 0x8000ffff)
== 0x80000000`. The log has no register value: it cannot distinguish an
unanswered command from a response containing nonzero status. Earlier logs
document similar failures between GPU boots; warm device state is a plausible
explanation, not an established cause of this attempt or the pixel corruption.

That turn performed no hardware retry, rebinding or forced reset. The
headless rendering oracle remained unexecuted on AMD, and the pixel corruption
remained unresolved.
Once hardware results are available, interpretation should compare the
specific failed route and its serialized control; a failure by itself does
not automatically assign blame to LinuxKPI, Mesa, or hardware.

## Follow-up retry and diagnostic shutdown correction

After the user requested another fix attempt, a single bounded hardware retry
passed PSP, SMU and DMUB initialization, then failed
`ring kiq_0.2.1.0 test failed (-110)` / `KCQ enable failed`. The probe again
reported no AMD render node and exit=2, so no texture/UBO checks ran. The host
remained running. Both initialization failures have appeared in prior
warm-device logs; this still does not establish the pixel-corruption cause.

The original automatic runner stopped QEMU immediately after the result
marker instead of asking the guest to shut down its drivers. It now invokes
`reboot(RB_POWER_OFF)` after collecting the child result, and the host waits
for QEMU's normal exit. A 30-second shutdown bound retains a fallback that
terminates only the private diagnostic VM. Hardware initialization errors are
also included in the host-side result so an absent render node is not confused
with a failed pixel oracle.

An automatic software-only AvoryOS boot passed all test oracles, requested
native power-off, completed NVMe shutdown and exited QEMU with status zero.
The actual native PCI shutdown regression also passes, including unbound
devices and callback reentrancy. This verifies diagnostic cleanup, not AMD
shutdown or pixel correctness. No vendor source or game settings were changed.

Evidence:

- `build/gpu-pipeline-boot/attempt-1-psp-failure/`: preserved first hardware logs.
- `build/gpu-pipeline-boot/attempt-2-kiq-failure/`: preserved retry hardware logs.
- `build/logs/gpu-pipeline-native-amd-retry.log`: retry result.
- `build/logs/gpu-pipeline-native-software-shutdown.log`: native power-off check.

The user requested continuing the native audit without another GPU boot.
No further physical GPU initialization, forced shared-bus reset or host
restart is performed. The subsequent completion correction is documented
separately in `linuxkpi-completion-lifetime.md`; it does not establish that
the pixel corruption is fixed.
