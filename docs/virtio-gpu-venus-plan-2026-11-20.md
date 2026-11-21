# Venus (Vulkan over virtio-gpu) implementation plan

Goal: give guest Vulkan applications (DXVK, so Proton D3D9/10/11 games) a real
Vulkan driver without GPU passthrough. Today Dark Souls Remastered runs
D3D11 -> wined3d -> OpenGL -> virgl -> host GL at 10-15 FPS; with Venus it can
run D3D11 -> DXVK -> Vulkan -> Venus -> host Vulkan.

## Current state (2026-11-20)

Present:
- QEMU `virtio-vga-gl` supports `venus=`, `blob=`, `hostmem=`.
- Host virglrenderer 1.3.0 appears built with Venus (`vkr_` symbols present).
- `virtio_gpu_proto.h` defines `F_RESOURCE_BLOB`, `F_CONTEXT_INIT`,
  `CMD_RESOURCE_CREATE_BLOB`, capset commands.
- virgl contexts, fences and DRM ioctls `0x41`-`0x49` work
  (`virtio_gpu_virgl.c`).

Missing:
- virtio-pci shared memory capability: `virtio_pci.c` parses cap types 1-5
  only; Venus needs type 8 (`VIRTIO_PCI_CAP_SHARED_MEMORY_CFG`, host-visible
  window, shmid 1).
- Feature negotiation for blob and context-init; capset 4 (Venus) unused.
- DRM uapi: `GETPARAM` returns 0 for params 3-9; no `CONTEXT_INIT` (`0x4b`)
  or `RESOURCE_CREATE_BLOB` (`0x4a`); `EXECBUFFER` rejects `ring_idx` and
  in/out syncobjs; `RESOURCE_INFO` reports `blob_mem=0`.
- A global `ioctl_owner` gate serializes all GPU ioctls across processes.
- Guest has no Venus ICD (only lvp, radeon, nouveau, nvidia in
  `/usr/share/vulkan/icd.d`).

## Phase 1 - plumbing (small)

1. `GNUmakefile`: `RUN_VENUS=1` ->
   `-device virtio-vga-gl,blob=true,hostmem=4G,venus=true` plus a shared memfd
   memory backend (required by guest-backed blobs):
   `-object memory-backend-memfd,id=mem,size=$(RAM),share=on -machine memory-backend=mem`.
2. `virtio_pci.c`: parse cap type 8 (64-bit offset/length variant), map the
   host-visible window (shmid 1) as device memory.
3. `virtio_gpu.c`: negotiate `F_RESOURCE_BLOB` and `F_CONTEXT_INIT`; fetch
   capset 4 (Venus) next to virgl/virgl2.

Exit check: boot log shows the shm window size and capset 4 info.

## Phase 2 - wire protocol (medium)

- `CTX_CREATE` with `VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK` flags (capset id,
  number of rings).
- `RESOURCE_CREATE_BLOB`: `blob_mem` GUEST / HOST3D / HOST3D_GUEST;
  `blob_flags` USE_MAPPABLE / USE_SHAREABLE / USE_CROSS_DEVICE; `blob_id`.
- `RESOURCE_MAP_BLOB` / `RESOURCE_UNMAP_BLOB`: allocate offsets in the shm
  window, honour the returned `map_info` (caching).
- Per-ring fences: `VIRTIO_GPU_FLAG_INFO_RING_IDX` + `ring_idx`, one fence
  timeline per (context, ring).

Status (2026-11-20): implemented; host self-test passes with RUN_VENUS=1
(`[VENUS] PASS`, two HOST3D shm blobs mapped at 0x0/0x10000, map_info 0x1).
The self-test must use blob_id 0 with USE_MAPPABLE: a nonzero blob_id names an
existing VkDeviceMemory and the host rejects it with ERR_UNSPEC.
- Done: `ring_idx` field and `FLAG_INFO_RING_IDX` in the ctrl header (value
  unused, still 0); `virtio_gpu_venus_ctx_create` (context_init = capset 4);
  `virtio_gpu_blob_create` for HOST3D / GUEST / HOST3D_GUEST with mem entries
  and ctx attach; `virtio_gpu_blob_map` / `_unmap` on a page-granular shm
  allocator, with map_info cache type validated and a range kept reserved on
  any control timeout.
- Boot self-test `virtio_gpu_venus_wire_selftest` runs under `boot_selftest=1`
  when capset 4 is present; otherwise it logs a skip.
- Deferred: per-ring fence timelines (the completion logic assumes one global
  prefix, so rings > 0 must not be enabled yet); shm reservations are not
  reset on device reset; guest-backed blobs are protocol-only and untested.

## Phase 3 - DRM uapi for Mesa venus (medium-large)

- `GETPARAM`: real values for RESOURCE_BLOB(3), HOST_VISIBLE(4),
  CROSS_DEVICE(5), CONTEXT_INIT(6), SUPPORTED_CAPSET_IDs(7), and the rest.
- `GET_CAPS` for capset 4.
- `DRM_IOCTL_VIRTGPU_CONTEXT_INIT` (0x4b): CAPSET_ID, NUM_RINGS,
  POLL_RINGS_MASK.
- `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB` (0x4a).
- `EXECBUFFER`: accept `ring_idx`, in/out syncobjs, fence fds.
- `RESOURCE_INFO`: report real `blob_mem`; `MAP`: mmap offsets for blobs.
- Ring-fence completion events readable/pollable on the DRM fd for rings in
  POLL_RINGS_MASK.
- Replace the global `ioctl_owner` gate with per-context locking.

Status (2026-11-20), slice 1 done, builds clean, not yet run on a host:
- Done: GETPARAM 6 (CONTEXT_INIT) and 7 (capset mask) report real values, and
  only when capset 4 exists; GET_CAPS accepts any capset the host exposes;
  CONTEXT_INIT (0x4b) accepts capset 4 (Venus, one ring, no poll mask) and
  records the capset per file; the Venus host context uses context_init 4 and
  survives reset restore. Legacy create and transfer are refused on Venus files.
- Compatibility: gallium's virgl winsys calls CONTEXT_INIT with the virgl capset
  whenever param 6 is set and gives up on error, so virgl capsets are accepted
  as a no-op that keeps the legacy context. Without this, a RUN_VENUS=1 boot
  would lose GL.
Status, slice 2 (builds clean, not yet run on a host):
- Done: GETPARAM 3 (RESOURCE_BLOB) reports 1 when Venus and the blob feature
  exist; RESOURCE_CREATE_BLOB (0x4a) for GUEST blobs, with GEM pages listed in
  the host command, a per-file context attach, and a handle; RESOURCE_INFO
  reports blob_mem; S3 restore recreates guest blobs from their pages; legacy
  transfers refuse blob resources.
- Still open, and why:
  - HOST3D blobs and blob MAP (GETPARAM 4 stays 0): need host-visible memory
    mapped into userspace. That is the Phase 4 work.
  - Shareable and cross-device blobs (GETPARAM 5 stays 0): same Phase 4 export path.
  - EXECBUFFER in/out syncobjs: still rejected. Needs syncobj semantics.
  - POLL_RINGS_MASK and ring fence events: rejected and not built.
  - Per-context locking to replace the global ioctl_owner gate: not built. It
    changes the desktop's locking, so it needs its own run on a host.

## Phase 4 - memory management (highest risk)

- mmap of host-visible blobs as PFN/device mappings: no page refcounts, never
  eligible for zram, KSM or reclaim (same failure class as the vDSO page that
  zram freed on 2026-11-20).
- Unmap/destroy: tear down every user mapping and complete the TLB shootdown
  before sending `UNMAP_BLOB`.
- Guest blobs: pinned pages attached as a scatter list.
- Caching per `map_info` (WC vs WB) through PAT.

## Phase 5 - userspace and validation

- Add Mesa `vulkan-virtio` (venus ICD) to the disk image.
- Ladder: `vulkaninfo` -> `vkcube` -> `vkmark` -> DXVK. For Dark Souls drop
  `PROTON_USE_WINED3D=1`, check with `DXVK_HUD=fps`.
- KWin stays on virgl OpenGL; Venus is only for Vulkan clients.

## Risks

- Phase 4 lifetime/caching bugs (stale mappings after blob free).
- Fence/event semantics must match Linux closely or venus stalls.
- Host-side blob + memfd backend must work with this QEMU/KVM build: verify in
  phase 1 before larger work.

Expected outcome: DXVK on Venus should reach roughly 60 FPS up to near-host
framerate in Dark Souls Remastered; Venus adds per-call overhead but far less
than wined3d + virgl.

## Phase 4 status (2026-11-20): implemented, builds clean, untested on a host

Lifetime model (option A): each user PTE that maps a frame inside a registered
device range holds one reference on the owning GEM object. Teardown
(UNMAP_BLOB, UNREF, shm release) runs on the `devframe` worker after the last
reference drops and a TLB shootdown completes.

- `kernel/src/mm/devframe.{c,h}`: range registry (sorted, binary search), job
  queue, worker thread, install/remove hooks.
- Hooks: `vmm_map_page_nolock` (install and replace), `vmm_unmap_page`,
  `vmm_unmap_page_locked` (munmap/madvise sweep), `vmm_free` (exit), `vmm_clone`
  (fork). Hooks ignore PMM-managed frames and frames outside any range.
- HOST3D blobs (blob_id 0, USE_MAPPABLE only): create, MAP (shm offset, cache
  mode from map_info: CACHED and NONE to WB, WC to WC; UNCACHED refused), and
  the existing DRM mmap path maps the range.
- GETPARAM 3 and 4 report 1 with Venus present.
- Not built: shareable and cross-device blobs (GETPARAM 5 stays 0), blob_id
  import, uncached maps, device-blob restore after S3 (reported as failure).

Not exercised by any current test: the registry and worker run only once a
DRM client maps a HOST3D blob. The boot self-test uses the Venus helpers
directly, and the image has no Venus client until Phase 5. Boot tests exercise
only the no-op hook path.

## Phase 5 status (2026-11-20): image and kernel fixes in, windowed WSI not built

- Guest ICD: Arch `vulkan-virtio` added to `scripts/rootfs-packages.txt`.
  Installed into the rootfs, and `disk.img` rebuilt; the image contains
  `/usr/share/vulkan/icd.d/virtio_icd.json`, `/usr/lib/libvulkan_virtio.so`,
  `/usr/bin/vulkaninfo`, and `/usr/bin/vkcube`. vkgears is not in the Arch repos.
- Kernel fixes for what Mesa venus sends: CONTEXT_INIT accepts NUM_RINGS up to
  64 (ring 0 only); EXECBUFFER accepts VIRTGPU_EXECBUF_RING_IDX with ring 0;
  HOST3D blobs accept a nonzero blob_id (import of host VkDeviceMemory). The
  HOST3D create also passes its context, which an earlier change had left as 0.
- Sync: DRM_CAP_SYNCOBJ is not advertised, so Mesa uses the sim provider and
  FENCE_FD_OUT. That path is already implemented.
- Expected blocker: device memory allocated with external handles
  (MAPPABLE | SHAREABLE) is refused. Windowed WSI (vkcube on X11 or Wayland)
  needs that path, so a windowed run is expected to fail until dma-buf export
  of device blobs exists. vulkaninfo does not need it.

## Phase 5 option A: dma-buf export findings (2026-11-20)

Blockers found, not yet implemented:
1. Guest export: `linuxkpi_prime_export` (kernel/linuxkpi/src/native_prime.c)
   requires every page to be PMM-managed RAM and builds `struct page` arrays.
   A host shm range has neither, so device blobs need a page-less export
   (sg from phys ranges, mmap via remap_pfn_range on the phys range, vmap).
2. Guest create: SHAREABLE HOST3D blobs are refused (-95) in `blob_create_host`.
3. Importer: Mesa's virgl winsys imports the fd as a new GEM object with no
   virgl resource, so RESOURCE_INFO returns res 0. Stage 3 must map the dma-buf
   back to the originating host resource id.
4. Host GL: virglrenderer makes a GL texture from a DMABUF-fd resource in
   `vrend_renderer_pipe_resource_set_type` (src/vrend/vrend_renderer.c ~13430),
   which needs EGL plus GBM. There is no RESOURCE_ASSIGN_UUID in this virglrenderer,
   so the res_id mapping is the only link. Whether this build has GBM is unchecked.

Staged plan: (1) page-less export and SHAREABLE create, (2) importer tagging,
(3) host set_type check with the real compositor. Each stage needs a boot test.

## Option A stage 1 status (2026-11-20): implemented, builds clean, untested

- linuxkpi `native_prime.c`: range objects (no struct pages). `native_map` builds
  an sg from the phys base, `native_mmap` uses remap_pfn_range on the range,
  `native_vmap` returns EOPNOTSUPP, `native_unmap` skips DMA unmapping.
  New entry `linuxkpi_prime_export_range`.
- `drm.h` / `drm_prime.c`: `device_range` and `prepare_export` on GEM objects.
  Export runs `prepare_export` (maps the blob on demand), then exports the
  range. Range objects are WB only.
- `virtio_gpu_virgl.c`: HOST3D blobs accept SHAREABLE (CROSS_DEVICE still
  refused). Mapping sets `device_range`.
- Stage 2 may already be mostly done. An importer in the same kernel gets the
  original GEM object from `linuxkpi_prime_native_object`, so it registers the
  same object and its RESOURCE_INFO reports the host res id. Stage 3 (host GL
  sampling) is still open. Check it with a windowed vkcube run.

## Windowed vkcube shear (2026-11-21): host NVIDIA pitch bug, patched

Windowed vkcube reached KWin through the dma-buf path but showed diagonal stripes.
Cause: host NVIDIA EGL (620.70) ignores `EGL_DMA_BUF_PLANE0_PITCH_EXT` for
linear dma-bufs and uses `align(width * cpp, 32)`. The prime-blit WSI pads rows to
256 bytes, so a 500-px window (2048-byte rows) was sampled at 2016 bytes per row.
This was confirmed with a standalone host Vulkan-export plus EGL-import test.

Fix: `scripts/virgl/nvidia-dmabuf-pitch.patch` (host virglrenderer). For a
linear single-plane dma-buf whose stride is wider than width*cpp, set_type
imports the dma-buf `stride/cpp` texels wide and gives the resource a plain
texture. The visible region is copied with glCopyImageSubData before sampler
binds, blits, copy_region and readback. On by GL vendor NVIDIA;
`VIRGL_DMABUF_PITCH_COPY=0/1` overrides. Gaps: host GL writes to such a
resource don't reach the dma-buf, and direct scanout of an odd-width buffer
isn't refreshed.

## Venus per-ring fences (2026-11-21)

Vulkan apps sometimes showed half-drawn frames. EXECBUFFER ignored
`VIRTGPU_EXECBUF_RING_IDX` and sent every fence without
`VIRTIO_GPU_FLAG_INFO_RING_IDX`. QEMU then treated it as a global fence, and
virglrenderer's Venus retires ring 0 / global fences at once on the CPU
timeline (`vkr_context_submit_fence`). Venus's async present thread waits on
that fence, so frames reached KWin before the GPU finished. Venus contexts now
tag the submit header with the ring, and the host retires the fence on that
VkQueue's sync queue. Out-of-order completion across rings is fine: render jobs
complete per cookie, and QEMU matches ring fences per ctx and ring.
