# Virgl on Ascension

`make run` selects `virtio-vga-gl` and SDL with `gl=on`. The host needs a QEMU
build with virglrenderer and a working OpenGL/EGL display backend. Use
`make run RUN_VIRGL=0` for the original 2D device and a display without OpenGL.
Virgl boots use the same xHCI keyboard and relative USB mouse as `run-nouveau`,
with Xorg drawing its software cursor into the framebuffer. Click inside the
SDL window to capture the relative pointer; Ctrl+Alt+G releases it. The host
cursor is hidden by default. SDL avoids the reproduced GTK/Wayland GL failure
where acquiring the relative-pointer grab stops further host mouse reports.
`RUN_DISPLAY=gtk` selects the former frontend for comparison; `RUN_VIRGL=0`
continues to use GTK by default.
`make run` also selects a locally built host renderer with a guard for the
reproduced NVIDIA indexed-draw crash. It skips draws whose active vertex inputs
have no backing buffers, allowing valid later draws to continue. System QEMU
and graphics packages are unchanged. The first launch builds the pinned host
library; see [host crash diagnosis and build requirements](virgl-host-crash.md).
Use `make run RUN_POINTER=usb-tablet` for absolute input. VFIO launch
targets retain their existing devices and display settings.

## Kernel and desktop integration

The native virtio-gpu backend negotiates `VIRTIO_GPU_F_VIRGL`, enumerates virgl
capsets and implements the legacy Linux virtgpu DRM interface used by Mesa:
GETPARAM, GET_CAPS, RESOURCE_CREATE/INFO, MAP, transfers, EXECBUFFER and WAIT.
Each open has a host context; resources use the existing per-file GEM handles
and PRIME sharing. Render nodes reserve a minor from the imported DRM core's
allocator after LinuxKPI initialization, so they can coexist with VGEM, VKMS,
and passed-through GPUs. The minor is discovered rather than fixed at 128.
A ready virgl device reports DRM driver `virtio_gpu`, version 0.1. Without
virgl it keeps the `ascentdrm` identity and software desktop profile.

Render submissions can keep up to 16 fenced batches in flight. Each batch
retains its own immutable command DMA and GEM references until the host returns
a matching completion fence. Maximum-size command buffers are reserved at boot
and reused, avoiding contiguous DMA allocations after desktop memory becomes
fragmented. The pool reserves at most 32 MiB with the current buddy allocator;
limited boot memory selects fewer slots, or the synchronous path if none fit.
Queue pressure waits for space; CPU WAIT calls
wait for the requested buffer's submitted fence prefix, so unrelated work and later frames from another client cannot
extend that wait indefinitely. Malformed fence replies quarantine their buffers.
Sharing a texture with another GL context, transferring its pixels, or exposing
it to the display frontend waits for its queued producer before accessing it.
Sync-file export requests use the queued path and return live Linux DMA fences
without waiting for GPU completion. Each job retains its own fence reference;
the completion worker signals it outside the native IRQ lock after a matching
successful host reply. Invalid replies signal an error while keeping BOs and
DMA quarantined. Closing a sync-file early cannot destroy the job's fence;
completion before fd installation is protected by a separate local reference.
The synchronous fallback exports a completed fence. Input sync-files are waited
before submission and propagate completion errors;
resource backing is retained if host resource destruction fails. Blob
resources, Venus, context-init, multiple rings and syncobj submission are not
advertised. Virgl framebuffers are presented directly as host textures with
SET_SCANOUT and RESOURCE_FLUSH. Desktop frames no longer travel through a
host readback, guest CPU copy and second upload. Native 2D and imported
non-virgl framebuffers retain the immutable staging path. Selected textures
remain referenced until a confirmed replacement, including after a timed-out
selection. Resources attach once per context rather than at every submission.
Cursor textures still use a small readback when uploaded.
Texture selection and display flush share one queue kick and completion wait.
Queued command payloads are copied directly from the validated user range into
the reserved immutable DMA slot. This removes the temporary command allocation
and second payload copy; header and payload length checks still enforce the
maximum packet size. The synchronous fallback retains its snapshot allocation.

Each BO tracks its pending render slots in a 32-bit mask protected by the GPU
queue lock. Dependency queries union those masks and inspect the 16 slots;
they allocate no lookup table and do not search unrelated batches' BO lists.
When only the submitting context has pending work, the query skips the BO
array entirely; it also stops once every candidate dependency has been found.
Validated completion clears only that job's bit before its slot can be reused.
Multiple pending users, duplicate handles and reordered replies preserve the
remaining bits. A malformed reply retains its resources and slot in quarantine.
The mask fits the native GEM refcount's former alignment padding. Handle
snapshots retain up to 32 BOs per file-lock acquisition; attachments and host
waits run after releasing that lock. WAIT retains its BO after serializing with
earlier metadata/transfer operations, then releases the global virgl ioctl gate
before waiting for the fence. Other clients can submit while it sleeps. Buffer
references, fence-prefix waits and producer synchronization remain in force.

Completed flips release their submission slot before notifying the compositor,
so submitting the next frame from the completion event cannot spuriously fail
with `EBUSY`. Worker cleanup retains its own job metadata across that handoff.
Each DRM client can hold up to 4095 buffer handles; submission snapshots use
heap storage and final close frees handles in small batches to avoid exhausting
the kernel stack. This supports desktops retaining hundreds of textures.

Xorg's boot selector enables the new `virgl` modesetting profile, with glamor
and DRI3, only when DRM identifies an active `virtio_gpu` renderer. The profile
does not impose a fixed virtual size, so GTK window size changes cannot exclude
the preferred mode during Xorg startup. The native
software and AMD/Nouveau profiles remain available. `AVORY_XORG_PROFILE=virgl`
can explicitly select the accelerated profile.

`drm-pick.sh` includes `virtio_gpu` when choosing a display and matching render
node. The session environment sets KWin and wlroots DRM device variables while
preserving explicit overrides. Weston uses the selected DRM card through its
existing session launcher; Mesa's EGL/GBM driver supplies virgl rendering.
There is no global `GALLIUM_DRIVER` or software-renderer override.

## Updating an existing desktop image

The normal image build installs `10-modesetting-virgl.conf`, the updated Xorg
selector, and the session scripts. **`make run` deliberately reuses an existing
`disk.img`; it does not update its desktop files.** An older image needs those
files installed before Xorg can automatically enable glamor. Rebuilding a disk
image also replaces its filesystem, so use the normal image workflow with
appropriate preservation of existing guest data, or install the updated files
in the guest:

| Source | Guest destination |
| --- | --- |
| `config/xorg/10-virgl.conf` | `/usr/share/avory-xorg/10-modesetting-virgl.conf` |
| `config/xorg/avory-xorg-profile.sh` | `/usr/bin/avory-xorg-profile` (executable) |
| `config/xorg/avory-nouveau-xorg.sh` | `/usr/bin/avory-nouveau-xorg` (executable) |
| `initrd/drm-pick.sh` | `/bin/drm-pick.sh` (executable) |
| `initrd/avory-drm.sh` | `/etc/profile.d/avory-drm.sh` and `/etc/xdg/plasma-workspace/env/avory-drm.sh` |

Older virgl profiles may still contain `Option "SWcursor" "false"`. Update
both `/usr/share/avory-xorg/10-modesetting-virgl.conf` and the active
`/etc/X11/xorg.conf.d/10-modesetting.conf` to `Option "SWcursor" "true"`.
Also remove the `Virtual 1280 800` line from both installed profiles. These
profile changes require a new Xorg session. The direct texture presentation
change requires rebuilding the kernel and boot ISO; `make run` does this.

Run `/usr/bin/avory-xorg-profile` before starting a new Xorg session. Restart
Wayland sessions to pick up the session environment. The Mesa package already
included in the desktop rootfs supplies the virgl driver.

## Verification

Build with `make -C kernel -j6`. Run the existing display regressions and the
new private VM checks:

```sh
python3 scripts/tests/test_virtio_gpu_command_ownership.py
python3 scripts/tests/test_virtio_scanout_engine.py
python3 scripts/tests/test_virgl_scanout.py
python3 scripts/tests/test_virgl_render_queue.py
python3 scripts/tests/test_virgl_reclaim.py
python3 scripts/tests/test_virgl_resource_lookup.py
python3 scripts/tests/test_virgl_wait_concurrency.py
python3 scripts/tests/test_virgl_exec_fence.py
python3 scripts/tests/test_drm_device_selection.py
python3 scripts/tests/test_xorg_profile_selection.py
python3 scripts/tests/test_virgl_boot.py
python3 scripts/tests/test_virgl_boot.py --2d
python3 scripts/tests/test_virgl_mesa_boot.py --desktop
python3 scripts/tests/test_virgl_mesa_boot.py --compat
python3 scripts/tests/test_virgl_mesa_boot.py --mouse
python3 scripts/tests/test_virgl_gtk_boot.py
python3 scripts/tests/test_virgl_gtk_boot.py --full-rootfs --pointer=usb-mouse --stress
python3 scripts/tests/test_run_virgl_pointer.py
```

The boot tests use private images and SDL's offscreen OpenGL backend; they
never open the desktop `disk.img`. Set `AVORY_TEST_ACCEL=tcg` without KVM.
The ABI test needs a musl compiler. The Mesa test also needs the existing
desktop rootfs and glibc cross-compiler; `AVORY_TEST_ROOTFS` can select another compatible rootfs.

The ABI test checks capsets, render-node discovery, a texture upload/readback,
submission, input/output sync-files, PRIME sharing, invalid user arguments,
and final close with 300 simultaneously live resources.
The Mesa test requires a renderer string containing `virgl` and checks the
pixel produced by an OpenGL ES framebuffer clear, then retains 512 textures
at once to check resource capacity. With `--desktop`, it starts
Xorg with glamor and Weston with a seatd service, then checks EGL rendering
under both X11 and Wayland as well as direct EGL/GBM. `--mouse` attaches the
launcher's xHCI keyboard and USB tablet, injects real QMP pointer reports, and
requires repeated Xorg hover motion, button presses and releases throughout
150 animated EGL window swaps, as well as evdev input and an X11 pixel check.
It also reports the elapsed time for 120 additional uncapped swaps.
`--stress` extends the mouse check to 600 animated frames over roughly one
minute, with repeated resizing and fullscreen transitions, periodic movement
checkpoints, and pixel readback checks during presentation.
`--mouse --pointer=usb-mouse` checks the relative-input override. A host using llvmpipe still
exercises the virgl guest transport; it does not prove host hardware speed.

In a desktop guest, `drm-identify` should show `virtio_gpu` and its paired render
node. `glxinfo -B` under Xorg and an EGL renderer probe under Wayland should
report `virgl`. The Xorg log should show glamor enabled, and the Weston log
should identify its GL renderer.

Protocol references: [Linux virtgpu DRM UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/drm/virtgpu_drm.h),
[virtio-gpu wire ABI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/virtio_gpu.h),
and [Mesa's virgl documentation](https://docs.mesa3d.org/drivers/virgl.html).

The frontend test (`test_virgl_gtk_boot.py`, retaining its original filename)
starts a private headless Weston compositor with a virtual pointer,
then sends input through SDL's native Wayland event handling, rather than
bypassing the frontend with QMP. It uses 8 CPUs and 4 GiB, matching `make run`,
and captures a live frame to check the displayed RGB pattern. It requires
host libweston 15 headers, `pkg-config`, `weston-screenshooter`, and access to a
GL render node. The compositor is terminated when the test finishes.
`--full-rootfs` tests a private Btrfs reflink of the existing `disk.img` with
boot services and the SDDM greeter. The original image is only read.
`--frontend=gtk` compares the previous frontend with the same guest and input.
`AVORY_TEST_INPUT_TRACE=1` records QEMU frontend and input-delivery events.

Add `--benchmark --full-rootfs --pointer=usb-mouse` to the frontend test for
two five-second uncapped glxgears samples at 800x600, alongside the input and
pixel checks. For a comparison on the actual host compositor, use
`test_virgl_mesa_boot.py --mouse --full-rootfs --pointer=usb-mouse --benchmark
--host-wayland-display=wayland-0`. This opens a temporary SDL VM window on the
specified display, injects input through QMP, and closes after the test. Compare
the same display, resolution and host load; uncapped FPS does not measure
desktop animation latency or guarantee an application-specific speedup.

Add `--scene-benchmark` alongside `--benchmark` for a 1024-buffer mesh scene
instead of glxgears. It reports two five-second uncapped samples with the same
desktop/input checks. This exercises resource-heavy submission; it is not a
Minecraft benchmark. The lookup microbenchmark reports only dependency-search
CPU cost for 16 pending batches with 1024 BOs each, not whole-frame speed.

On 2026-10-25, the 1024-mesh scene on the RTX 4060 Ti host, private SDL/Wayland
display, 8-vCPU/4-GiB guest and 800x600 GL window measured:

| Kernel | Two five-second samples | Mean FPS |
| --- | --- | --- |
| Before membership lookup and concurrent WAIT | 263.305, 232.740 | 248.023 |
| Both optimizations, preserving metadata/transfer ordering | 292.590, 307.312 | 299.951 |

This pair shows approximately 21% higher scene throughput. Each run also passed
150 animated mouse-test frames, sustained movement, button press/release and
live host RGB checks. These are synthetic measurements; Minecraft in-world FPS
has not been measured with the updated kernel. The installed guest options
already disable vsync, so the update does not change game quality or frame caps.

The subsequent live sync-file and direct command snapshot update was compared
against that kernel in two fresh runs per version, with the same host GPU,
scene and desktop/input checks. Each run contains two five-second samples:

| Kernel | Run 1 mean FPS | Run 2 mean FPS | Combined mean FPS |
| --- | --- | --- | --- |
| Membership lookup and concurrent WAIT baseline | 286.707 | 298.598 | 292.653 |
| Live output fences and direct command snapshots | 300.422 | 342.353 | 321.388 |

The combined scene mean is approximately 10% higher. The individual results
vary, so these data do not establish the speedup for Minecraft or equivalence
to GPU passthrough. Both final runs passed the sustained mouse and live pixel
checks. Native ABI tests also poll an exported sync-file before using it as an
input fence. Source-backed regressions cover delayed/reordered completion,
error signaling with retained quarantine, early fd close, completion before fd
installation, allocation/submission/export failure and synchronous fallback.

The following allocation-free slot-mask and grouped-handle update reduced the
source-backed 16-batch/1024-BO dependency query from about 32 microseconds with
the preceding hash table to under 1 microsecond on the host. This measures query
CPU cost, not total rendering time. A final 1024-mesh desktop run averaged
348.852 FPS and passed sustained SDL mouse input and live RGB scanout checks.
Baseline runs varied from 212.230 to 344.897 FPS; the final warmed result is
close to the warmed baseline, so these runs do not establish a further game
FPS speedup. The update reduces allocation, locking and dependency-search work
without changing game settings. Shared-BO tests cover cross-context batches,
duplicate handles, reordered completions, slot reuse and retained quarantine;
clearing another batch's bit is detected by the regression.

The 2026-10-26 completion cleanup update detaches all completed jobs under one
queue-lock acquisition, then signals fences and releases BO references outside
the lock. An empty pass previously acquired the lock 16 times; a fully completed
16-slot pass acquired it 32 times. Both now acquire it once. Validated slots are
available before callbacks run, and cleanup retains a private metadata snapshot
so an immediate new submission cannot have its references or slot overwritten
by the preceding cleanup. Invalid replies still retain DMA and BOs in quarantine
and signal their error callback exactly once.

`test_virgl_reclaim.py` checks lock counts, callback reentry, reuse of all slots,
independent BO lifetimes and mixed successful/malformed replies under address
and undefined-behavior sanitizers. The kernel build, queue/dependency/fence/WAIT
regressions, scanout/command-ownership regressions, private VirGL ABI boot and
Mesa EGL rendering/512-texture checks passed. Those initial boots used TCG and a software host renderer. Subsequent tests
used KVM and the RTX 4060 Ti host; see the Minecraft investigation below.
This verifies transport correctness;
Minecraft FPS and host-GPU performance have not been measured for this update.
Use the same saved world, camera position, resolution, render distance and
graphics settings for a before/after game comparison, allowing chunk loading
and shader compilation to settle before recording frame times.


## Minecraft GPU allocation ceiling

The native DRM handle table now supports 65536 slots, reserving zero and the
legacy framebuffer handle. The old 4095-live-resource limit reproduced
Minecraft glBufferData allocation failures and the missing-buffer guard.
The fixed kernel passed 6000 simultaneous resources and a Minecraft stress
scene with over 5900 live handles. Normal host vertex binding is enabled by
default; the earlier legacy NVIDIA path is opt-in. Details and evidence are in
[minecraft-rendering-2026-10-26.md](minecraft-rendering-2026-10-26.md).
