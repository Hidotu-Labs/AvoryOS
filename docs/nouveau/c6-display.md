# C6 — physical display and PRIME

Started and core closed 2026-10-21 for the selected **NVIDIA-to-native-VirtIO
linear PRIME / legacy KMS route**. Hardware passes sharing/lifetime, GPU-produced
pixels, six repeated flip events, pending-work nonblocking synchronization,
pending-event teardown and identity-based device selection with overrides/fallback.
This is not completion of every physical-display or desktop-acceleration profile.
C5 core closure and its deferred known gaps remain unchanged. The project
owner selected **PRIME to VirtIO** instead of physical NVIDIA scanout after
the initial inventory. Direct NVIDIA output is not a prerequisite for this
chosen route and remains explicitly untested.

This is a historical phase record. Its phase-specific interactive Make targets
have been retired; use `make run-nouveau` for the current installed desktop.

## First inventory

`userland/test_nouveau_display.c` scans primary nodes rather than assuming a
minor number. It requires driver `nouveau` and PCI identity `10de:2803`, and
rejects ambiguous multiple matches. This is not yet general BDF-based desktop
selection; the final N49 helper below adds guest sysfs identity/render pairing
and explicit overrides. The runner separately selects the reviewed host BDF
and records guest BDF.

The oracle queries resources, CRTCs, encoders, connectors, modes, EDID blobs and
universal-plane linear formats using the private imported DRM UAPI. A first
GETCONNECTOR with zero mode count requests detection before the populated
query. Oversized results fail before array traversal. Present EDID must have a
valid header, exact extension count/length and every 128-byte block checksum;
its complete bytes are retained in serial. Missing EDID is reported, not invented.
The oracle does not allocate a framebuffer or modeset.

The software oracle exercises EDID corruption/length/header rejection and
inventory bounds without opening a GPU. Host regression additionally runs the
actual inventory function against mocked ioctl responses, injecting nine
resource/connector/EDID/plane failures and checking detection is requested.
Neither is physical-display evidence.

## Evidence

The following initial runs use private kernel SHA256
`c5e8c9fed4ca8610ef038a898b3e38a543e80caa6ff083c14a185c39d49d7780`,
one vCPU, verified 570.144 firmware, GPU-only PCIe assignment and explicit GSP
removal before S5. No host reset policy, binding or power setting was changed.
Normal C4 ISO checksum remains valid; the installed desktop disk is untouched.

- `build/nouveau-c6/readiness-inventory/` and `hardware-inventory/` pass strict
  boot/oracle gates. The first implementation queried cached connector state:
  all four connectors were unknown (3), with no modes/EDID. It is not evidence
  of an actual disconnected detection result.
- `build/nouveau-c6/readiness-detect/` and `hardware-detect/` pass strict gates
  after adding the explicit detection query. Nouveau is `/dev/dri/card2`:
  four CRTCs, four connectors (three DisplayPort, one HDMI), eleven encoders,
  twelve universal planes. All four connectors are disconnected (2), with
  zero modes and no EDID. Plane format lists are retained in serial, but
  modifier negotiation and scanout remain untested.
- `build/nouveau-c6/host-evidence/` retains host preflight and kernel journals
  before/after both hardware runs. Both functions remain VFIO-owned and
  accessible, x8 link width, no ownership blockers after shutdown.
- `build/nouveau-c6/readiness-prime/` and `hardware-prime/` pass strict gates
  for the selected PRIME route. Two sequential independent NVIDIA clients
  export 32 KiB GART objects to native VirtIO (`card0`, guest `00:06.0`). Full
  byte patterns are checked through both mappings, including importer writes
  seen by the exporter. Repeated imports must return the same handle; after
  the NVIDIA mapping, GEM handle, dma-buf fd and client fd close, the imported
  object must still re-export and retain every byte. CPU access is bracketed
  by exporter PREP/FINI or dma-buf SYNC; cleanup results are mandatory.
  This proves CPU byte coherence and ownership lifetime for this direction,
  not GPU-produced pixels, busy-fence synchronization, VRAM export,
  reverse-direction sharing or captured presentation. The QEMU host warning
  about VFIO dma-buf mmap fallback is retained in the host log; it is not a
  failure of the separately tested guest PRIME path. Host health after this
  third hardware run also has no blockers.
- `build/nouveau-c6/hardware-present/` is a retained **failed** first capture
  attempt: graphics console takeover suppressed stdout READY markers. Guest
  work and shutdown completed, but missing screenshots/markers correctly
  prevented a pass. No pixels are claimed from this attempt.
- `build/nouveau-c6/readiness-present-final/` qualifies the corrected oracle
  (previous readiness is archived in its `history/`). Presentation telemetry
  uses `/dev/kmsg`, whose serial logging remains available during graphics
  takeover, rather than re-enabling console painting over the test pattern.
- `build/nouveau-c6/hardware-present-telemetry/` passes strict boot, guest
  presentation, **both full-frame QMP pixel comparisons**, cleanup and GSP
  removal/S5 on the same fixed kernel. Captures are 1280x800, 1,024,000 pixels
  each, with distinct frame hashes in `capture.json`. The first frame has a
  lossless PNG preview derived from the actual QMP PPM capture. Afterward,
  both PCI functions remain accessible, VFIO-owned, x8, with no preflight
  blockers. Host before/after evidence has the `present-telemetry` suffix.
- `build/nouveau-c6/readiness-gpu-present/` qualifies the GPU producer and
  all reused sources/class headers. `hardware-gpu-present/` passes strict
  boot, two CE EXEC copies of 4,096,000 bytes, timeline wait/query at points
  1 and 2, future-point ETIME checks, **two exact full-frame captures**,
  checked cleanup and GSP removal/S5. The capture scope explicitly records
  CE production with CPU-side timeline waiting, not shader rendering or
  pending implicit-fence qualification. Host before/after evidence uses the
  `gpu-present` suffix; both functions remain VFIO-owned, accessible, x8,
  and the post-run preflight has no blockers. The normal C4 ISO checksum is
  unchanged. CPU-only readiness is refreshed for the shared capture-helper
  change; its earlier results are retained in `history/`.

These hardware boots also pass the fixed allocator's strict VKMS diagnostic;
they do not re-run C5 GPU-copy or deferred pressure/concurrency oracles.

## Reproduction

```sh
python3 scripts/tests/test_nouveau_display_oracle.py
python3 scripts/tests/test_nouveau_prime_oracle.py
python3 scripts/nouveau-boot.py --c6-inventory --software --run \
  --pcie-root-port --gpu-only --cpus 1 --timeout 180 \
  --kernel build/nouveau-618/kernel --firmware-dir build/nouveau-618/firmware \
  --output-dir build/nouveau-c6/readiness-new
```

After a read-only host preflight confirms health and no existing VFIO owner,
hardware uses the same oracle with `--run --gpu 0000:01:00.0 --host-on-igpu
--pcie-root-port --gpu-only --remove-on-shutdown`, matching kernel, firmware,
CPU count and `--readiness .../result.json`. Select a new output directory.
Readiness includes the exact oracle source and DRM header hashes, including
`drm_mode.h`; C4/C5 readiness cannot qualify this oracle.
For the selected sharing route use `--c6-prime` in both readiness and hardware
runs instead of `--c6-inventory`. Its qualification includes the reused C5
memory helpers and imported `linux/dma-buf.h`, not just the top-level source.

## Captured linear PRIME presentation

`userland/test_nouveau_present.c` adds the visible test, distinct from the
sharing oracle. It discovers the native VirtIO sink by driver/bus identity,
selects an advertised 1280x800 mode and a compatible encoder/CRTC, creates a
Nouveau GART BO and imports its dma-buf on VirtIO. Legacy ADDFB describes an
explicit linear XRGB8888 buffer with 5120-byte pitch. Two CPU-filled color-bar
frames have a white border and a changing upper-left frame marker; each frame
is presented with SETCRTC and held for ten seconds. CPU_PREP/FINI brackets the
exporter writes. Framebuffer, import, mapping, exporter handle and descriptors
are released with checked cleanup results. This isolated diagnostic shuts
down afterward; it does not reconfigure the installed desktop or Xorg.

`scripts/nouveau-prime-capture.py` reads actual VirtIO scanout through a private
QMP screendump socket. Each frame must match all 1,024,000 RGB pixels exactly,
including borders and the frame marker. A black window, stale first frame,
incorrect geometry, truncation or a single changed byte cannot pass. A bounded
six-second capture deadline allows asynchronous VirtIO upload after SETCRTC;
the guest holds the frame for ten seconds. Both captured frames and their
hashes must be present before `capture.json` and the overall result can pass.
Guest READY/END markers alone are insufficient.

Readiness binds the presentation source, reused sharing/memory sources and
the host capture verifier to one source digest. `--c6-present` is separate
from `--c6-prime`; old sharing-only readiness cannot qualify presentation.
The software regression compiles the actual C pixel generator and compares
its complete output with the independent host generator, injects corrupt
captures, and tests missing captures/markers and source/ABI readiness changes.

The retired `run-nouveau-c6-prime` target opened a **separate diagnostic QEMU**,
not the installed desktop. It expected
two changing color-bar frames for ten seconds each, followed by automatic
shutdown. The terminal must print two `NOUVEAU_C6_CAPTURE: PASS` lines; inspect
`build/nouveau-c6/interactive-present/result.json` and `capture.json` afterward.
The target uses `build/nouveau-c6/readiness-present-final/result.json`;
`NOUVEAU_C6_READINESS` and `NOUVEAU_C6_OUT` override these private paths.
It preserves the normal ISO, desktop disk, GPU-only topology and host reset
policy. It refuses an existing GPU owner rather than stopping another VM.

This tests **CPU-filled NVIDIA-exported buffers through PRIME to VirtIO**.
It does not prove GPU rendering, busy-fence synchronization, page-flip events,
atomic commits, modifiers, physical NVIDIA output or an accelerated desktop.

## GPU-produced PRIME presentation

`userland/test_nouveau_gpu_present.c` adds a separate copy-engine producer
profile (`--c6-gpu-present`), reusing the C5 CE class/push encoding and C6
pattern/mode discovery. It creates an explicit VM, a reported
`AMPERE_DMA_COPY_B` object and a timeline syncobj. Three GART BOs hold the
CPU-written upload source, GPU-written scanout destination and command stream.
**Only source and command BOs are mapped in userspace.** The destination is
exported/imported by PRIME and never CPU-filled by the oracle. Native VirtIO
still maps and uploads the imported destination internally; this is not a
zero-copy or GPU-accelerated VirtIO backend claim.

For each of two frames, CPU_PREP/FINI brackets source and command writes.
NVIDIA CE EXEC copies all 4,096,000 bytes into the scanout destination and
signals timeline point 1, then point 2. Each frame requires an absolute
ten-second bounded timeline wait and an exact completed-point query. The
next, not-yet-submitted point must return ETIME with a zero timeout. Only
after these checks does SETCRTC present the imported destination. The existing
QMP verifier independently compares every RGB pixel in both actual scanouts.
Channel, VM bindings, framebuffer, import, BOs, user mappings, syncobj and
descriptors have checked teardown.

The selected synchronization profile is **explicit producer-fence waiting
before presentation**. It does not demonstrate a pending implicit-fence wait,
nonblocking presentation, tear-free flips, shader rendering, NVK/Zink or an
accelerated desktop. CPU-only presentation logs/readiness cannot qualify the
GPU profile. Its digest also includes the reused copy implementation, cached
Mesa CE/NVIF headers, presentation/sharing/memory helpers and capture verifier.

Host regressions include `scripts/tests/test_nouveau_gpu_present_oracle.py`,
which compiles the actual GPU oracle, checks software push/pattern tests,
rejects CPU-only/fence-incomplete markers and mismatched readiness, and
checks the two user mappings exclude the destination. The existing full-pixel
corruption/stale-frame tests apply unchanged.

The retired `run-nouveau-c6-gpu-prime` target watched the GPU profile with the
other QEMU closed.
`NOUVEAU_C6_GPU_READINESS` defaults to
`build/nouveau-c6/readiness-gpu-present/result.json`; `NOUVEAU_C6_GPU_OUT`
defaults to `build/nouveau-c6/interactive-gpu-present`. Expect two ten-second
color-bar frames, two `NOUVEAU_C6_GPU: PASS EXEC` records and two
`NOUVEAU_C6_CAPTURE: PASS` records, then clean automatic shutdown. The CPU
presentation target remains separate and does not prove GPU production.

## Final three gates and core closure

`build/nouveau-c6/readiness-flips-final/result.json` and
`build/nouveau-c6/hardware-c6-final/result.json` both pass with private kernel
SHA256 `f91d718ed95bba0e3cbb1e6505c58992dd693c56cb33f0b0a679a51c9a96411d`.
The source/header-qualified profile is `--c6-flips`. It preserves one vCPU,
GPU-only assignment, verified firmware and strict ordered GSP removal/S5.

1. **Events and lifetime:** six legacy PAGE_FLIP_EVENT updates pass exact
   cookie/CRTC/type/length, strictly increasing sequence/timestamps and empty
   queue checks. Each captured 1280x800 frame matches all 1,024,000 RGB pixels.
   A seventh pending job tests RMFB, imported-handle close and display-fd close
   before producer completion. A separate client receives no leaked old event;
   a fresh import/flip receives its own correct event and recovers presentation.
2. **Real pending producer:** each actual NVIDIA CE EXEC waits on a deliberately
   held sync-file dependency and signals a timeline. Zero-timeout waits return
   ETIME before and after PAGE_FLIP returns; the final hardware ioctl latencies
   are 2–5 microseconds. No event is readable during the initial 100ms pending
   window, and another flip returns EBUSY. A child releases the dependency after
   750ms. The independent native presentation worker waits on the implicit
   producer fence, stages bytes and publishes before sending the vblank event.
   This proves genuinely queued GPU work, not an actively engine-busy shader
   workload, atomic NONBLOCK or hang recovery. The destination is never mapped
   in userspace; native VirtIO presentation still uses CPU staging/transport.
3. **Selection:** the real guest inventory pairs Nouveau card2 and renderD129
   with `pci:0000:01:00.0`, while default display remains connected native card0.
   Actual selector tests exercise valid card/BDF/driver overrides and unavailable
   card/BDF/render fallback. Software readiness proves the no-NVIDIA/gate-off
   route. Host fixtures also cover shuffled minors, multiple drivers, ambiguous
   pairings, KWin override preservation and native Xorg fallback.

The N49 failure was a kernel sysfs identity bug, not permission to guess a minor:
static AMD card2/renderD129 aliases pointed at the native VirtIO PCI parent when
AMD was absent. LinuxKPI now publishes each registered DRM minor under its actual
PCI ancestor (or the virtual DRM tree), including class, subsystem, device and
character-device links. Removal drops its aliases and attributes without deleting
other GPU siblings. Dynamic symlink targets and duplicate lookup references have
checked ownership. The selector still refuses mismatched nonempty BDFs.

`/bin/drm-identify` supplies real inventory to `/bin/drm-pick.sh --display`,
`--render` or `--pair`. Supported overrides are `ASCENT_DRM_CARD`,
`ASCENT_DRM_BDF`, `ASCENT_DRM_DRIVER` and `ASCENT_DRM_RENDER`; unavailable
requests warn and fall back. Existing `KWIN_DRM_DEVICES` remains authoritative.
AMD's emulated-sink exception never fabricates a connected NVIDIA display.

The first combined pass, `hardware-c6-close/`, used kernel
`241e1e622f3340b6f46305aa325cb14c2a39820e6b859f41c4ccc9d876d87695`.
The final pass adds symlink lifetime cleanup and succeeds on the next hardware
boot without a host reboot. `host-evidence/` preserves before/after preflight
and journals with `c6-close` and `c6-final` suffixes. Final preflight has no
blockers and both physical functions remain accessible and VFIO-owned. The
allocator page free-count canary passes. No host policy was modified; normal
C4 ISO and installed desktop disk remain unchanged.

New focused regressions execute the actual sysfs minor bridge, symlink lifetime,
asynchronous flip worker and S5 serial-drain implementation. The shutdown drain
prevents queued diagnostic output being lost at power removal; missing END markers
are not tolerated. Pending-profile compilation/strict-marker tests and the existing
selection, capture-corruption, PRIME lifetime, devres and boot classifiers pass.
Historical failed runs remain failed and archived, not relabeled as evidence.

The retired `run-nouveau-c6-flips` target watched the final qualified private
profile with other QEMU instances closed.
Expect six `NOUVEAU_C6_CAPTURE: PASS` records and clean automatic shutdown.
Earlier CPU/explicit-wait targets require their own readiness to be regenerated
after a private kernel change; they cannot reuse a mismatched result.

## Explicitly outside this closure

- Physical NVIDIA EDID, modes, scanout, cursor and flip qualification (N44–N46).
- Atomic commits/NONBLOCK, other formats/modifiers and reverse-direction PRIME.
- Actively engine-busy workloads, reset/hang recovery and SMP stress.
- NVK/Zink shader rendering, accelerated desktop and full compositor integration
  (C7), plus new AMD hardware qualification.

No connected sink is fabricated to get past discovery. No display mode may be
selected from the zero-mode disconnected result above.
