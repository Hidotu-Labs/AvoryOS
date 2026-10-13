# Native VirtIO GPU presentation rewrite

The native DRM display backend now has one presentation engine for native dumb
buffers and imported PRIME framebuffers. It replaces the per-GEM host-resource
model, shared PRIME staging and native asynchronous in-place uploads. PCI,
virtqueue, EDID and cursor transport interfaces are preserved.

## Ownership and publication

Each output owns a current pair of 2D host resources with two independent
coherent DMA allocations. A producer GEM supplies pixels, never host DMA
backing. The backend retains its GEM reference and copies its framebuffer
metadata under the KMS lock before any fence wait. It prepares the inactive
image, waits for producer CPU access, copies the requested view into tightly
packed storage, and ends CPU access before interacting with the host.

The engine acknowledges a full transfer before selecting that resource, then
flushes it. This uses the standard [VirtIO 2D resource operations](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.pdf).
The selected image and its backing stay intact throughout the next upload.
The task-context presentation gate covers selection and publication for all
heads. Completion/cursor draining can proceed independently. Existing
single-output PRIME full-snapshot coalescing is retained for uncapped
producers; multi-output commits do not discard another head's updates.

Framebuffer offsets belong to the framebuffer layout rather than an imported
GEM's driver-private allocation. The copy validates the actual last row,
accounts for viewport coordinates and source pitch, and excludes row padding.
The ADDFB2 base metadata retains its plane-zero byte offset. Native and imported
sources therefore use the same copy and publication rules.

A resize builds a pending resource pair while the prior selected pair remains
visible. The old generation retires only after successful selection of the new
image. UNREF acknowledgement, including confirmed absence of an uncertain ID,
releases host ownership before DMA storage is freed. Partial creation and
attachment have cleanup ownership before the first submitted command. A
late or unsuccessful unref retains its storage; further allocation is bounded
until cleanup succeeds. Worker housekeeping reclaims retired generations after
late acknowledgements even when the producer has stopped committing.

A timed-out transfer prevents staging reuse until the transport returns its
submitted descriptor. A failed SET_SCANOUT has an unknown selection until a
confirmed disable re-establishes ownership. An immutable snapshot can be
retried once; a flush retry does not transfer into the selected image.

## Validation

* `scripts/tests/test_virtio_scanout_engine.py` compiles the actual replacement
  engine and the actual native task gate. It checks 300 padded/cropped frames,
  two heads, geometry changes, producer repaint/release before host upload,
  independent allocations, and rejected bounds. It injects delayed create,
  attach, transfer, selection, flush and unref replies, as well as partial
  creation and allocation failures. One hundred forced cross-producer ACK
  interleavings pass; bypassing the gate fails the negative control.
* The same engine tests pass AddressSanitizer, UndefinedBehaviorSanitizer and
  LeakSanitizer. LeakSanitizer needs execution outside the ptrace sandbox.
* `test_prime_scanout_lifetime.py` runs actual native snapshot/commit/free
  functions, including RMFB during fence wait, begin failure, offsets,
  unsupported source formats, and host-owned staging. It asserts that CPU
  access has ended before transport or vblank handling. Removing source
  references or KMS unbinding fails its negative controls.
* The retained command-ownership regression verifies delayed acknowledgements,
  malformed fences and lengths, retained DMA after timeout and safe retries.
  Existing CPU snapshot, DMA/cache-alias, VMAP, and display-startup tests pass.
* `scripts/tests/test_prime_qemu_scanout.py` builds a temporary root disk and
  boots the actual ISO kernel under two-CPU TCG, without VFIO or the user disk.
  The guest alternates a native dumb framebuffer with an imported PRIME
  ADDFB2 buffer whose pitch is 5152 bytes and offset is 64 bytes. Two threads
  complete 400 flips and 400 damage commits. In the final ISO run, QMP checked
  70 frame-marked active host images, plus the held final frame, against all
  1,024,000 expected RGB pixels at 1280x800. The ISO kernel matches the
  stripped current build ELF.

The private minimal root omits firmware and reports those unrelated firmware
suite failures; the existing sysfs PMM accounting failure also remains. These
are not passes of every boot suite.

## What this establishes

The replacement engine has explicit ownership and validated publication for
both framebuffer sources. These tests establish those invariants and host
image agreement in controlled workloads. They do not establish that the
original RadeonSI/Minecraft/tunnel artifact has disappeared.

The report's clean Minecraft F2 PNG is evidence favoring investigation of the
presentation path, not proof of its location: it was not paired with the same
displayed frame. Fog-dependent tunnel corruption still leaves shader inputs,
GPU buffer synchronization and X11 composition in scope. Hardware workloads
and their performance must be checked after booting the rebuilt ISO. No Mesa
or imported AMDGPU source is changed by this rewrite, and no application
feature or preset is disabled.

The existing read-only `scripts/prime-capture.py` diagnostic also understands
the new selected scanout images and independent source/staging strides. Its
new layout support is restricted to an uncropped head-zero framebuffer.
A producer screenshot may be newer than the selected snapshot; its report
records the selected resource and publication counter to expose that limit.
