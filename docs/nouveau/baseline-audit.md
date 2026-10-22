# Nouveau C0 baseline and dependency audit

Date: 2026-10-18, Europe/Istanbul.
AvoryOS source: `883a44c9cee338836b15b96db9eb8c5362ed1eec`.

## Outcome

C0's investigation and input selection are delivered. Select **Linux v6.12.113**
for the first coherent shared-graphics migration, **GSP-RM 535.113.01** from the
existing pinned firmware revision, and **Mesa/NVK/Zink 26.2.5** with the existing
Arch/glibc userspace ABI. These are source-audited implementation inputs, not a
claim of working NVIDIA support or a passing migration.

The active Linux import, kernel configuration and GPU ownership were not changed.
The candidate Linux tree, extracted firmware, inspected NVK package and Mesa
source are isolated under `build/nouveau-c0/`. No candidate driver was compiled
into AvoryOS, installed into its rootfs or run on either physical GPU.

The regression starting point is **partially verified**, not green. Seven targeted
host test programs pass. An eight-CPU software-rendering fixture returns zero
oracle failures and shuts down cleanly, but its kernel log contains a semaphore
self-test failure and a missing firmware-test fixture. A one-CPU variant fails
to launch its requested init and times out. Preserve these results before C1.

Machine-readable records:

- [Version, hardware, artifact and result lock](c0-lock.json).
- [Dependency ledger: 22 initial entries](dependency-ledger.csv).
- [Exact AD106 firmware SHA-256 manifest](firmware-ad106-535.113.01.sha256).
- [Full implementation plan](../linuxkpi-nouveau-plan.md).

## 1. Hardware and host ownership

| Item | Observed value |
|---|---|
| First target | GeForce RTX 4060 Ti, AD106, 16 GB class; driver reports 16,380 MiB |
| GPU host BDF / PCI revision | `0000:01:00.0`, `10de:2803`, revision `a1` |
| Subsystem | CardExpert Technology, `10b0:f314` |
| VBIOS | `95.06.34.00.63` |
| Current host GPU driver | NVIDIA `610.57.04` |
| Audio function | `0000:01:00.1`, `10de:22bd`, host `snd_hda_intel` |
| IOMMU group | `12`; exactly GPU and audio functions |
| GPU reset methods advertised | `flr bus`; availability is not proof of successful reset |
| Boot VGA | NVIDIA reports `boot_vga=1` |
| Connected host output | `card1-DP-3`, 1920×1080 available |
| Other NVIDIA connectors | DP-1, DP-2 and HDMI-A-1 disconnected |
| Future host display iGPU | Raphael, `0000:0e:00.0`, `1002:164e` |
| Current Raphael owner | `vfio-pci`, group `24` containing only `0000:0e:00.0` |
| Raphael reset methods advertised | `bus` |
| Host kernel | `6.18.42-1-cachyos-lts`, x86_64 |

The NVIDIA GPU currently exposes a 16 MiB non-prefetchable register BAR, a
16 GiB 64-bit prefetchable aperture, a 32 MiB 64-bit prefetchable BAR, a 128-byte
I/O region and a disabled 512 KiB ROM window. Preserve the 64-bit resource ranges
in the lock file; audit native/guest resource handling rather than assuming an
AMD-sized aperture. Guest allocations may differ from host addresses and sizes.

The user intends to move the host display to Raphael during NVIDIA testing.
That has **not happened yet**: Raphael is still assigned to VFIO and the host
display is on NVIDIA. A later session must establish host iGPU ownership and
working display output before releasing NVIDIA. Do not use `run-c7-prime` for
this handoff; it targets Raphael and has AMD-specific reset/ROM assumptions.

Firmware-menu settings such as Above 4G/ReBAR toggles were not read. The observed
apertures are evidence of current resource exposure, not a report of BIOS menu
values. No ROM enable, reset, PCI unbind, driver bind or power-state write occurred.
A Nouveau Linux reference boot and actual NVIDIA reset reliability remain pending.

## 2. Reproducible source and userspace choice

| Component | Current baseline | Selected C1/port input |
|---|---|---|
| Linux | v6.6.156, `8b73de7da85fde281a385e0b26eda9bffd3ca477` | v6.12.113, `7fdb7c1824a1f9979d648d7ae28ac446bcee9c6e` |
| Firmware repository | ref `20260916`, commit `ab23307cfe7f9366c819025ca3e4778299bc2db2` | Retain the same exact commit; add selected NVIDIA files later |
| Main rootfs | Arch/glibc under the historical `build/alpine/rootfs` path | Keep Arch/glibc |
| Mesa | Main Mesa/Zink 26.2.4; Radeon Vulkan split already 26.2.5 | Align relevant Mesa splits to 26.2.5 |
| NVIDIA Vulkan | Absent | `vulkan-nouveau` `1:26.2.5-1`, package inspected only |
| libdrm | `2.4.134-1`, including `libdrm_nouveau.so.2` | Retain pinned version initially |
| Vulkan loader | `1.4.363.0-1` | Retain pinned version initially |
| Main rootfs libc | `2.44+r50+g1848099f063e-1` | Preserve main-rootfs ABI |

The candidate's annotated tag object is
`0f6a69bb2f85d6da4dc81c32685143ee6c255348`; the peeled commit is recorded above.
The separate clone is `build/nouveau-c0/linux-candidate`. The live import pin
still identifies v6.6.156. Kernel.org's release metadata listed v6.12.113 as a
longterm release on 2026-10-15 when this audit ran.

Reasons for this selection:

1. The existing 6.6 source has early GSP/Falcon code but lacks the required Ada
   GSP-RM boot path. Its VM_BIND UAPI does not establish AD106 hardware support.
2. The exact candidate maps chipset `0x196` to AD106 and uses `ad102_gsp_new`
   with the `535.113.01` interface. This is checked in current candidate source,
   not inferred from the RTX marketing name.
3. Nouveau, DRM/TTM, scheduler, GPUVM and their headers can migrate together.
   The current AMD source patch dry-runs successfully against this candidate,
   although broader source/API compatibility still needs C1 work.
4. Mesa 26.2.5's Nouveau winsys accepts Nouveau versions at least 1.3.1. The
   candidate advertises 1.4.0 and contains VM_INIT/VM_BIND/EXEC and the inspected
   GETPARAM interfaces. This establishes a viable source-level pairing; correct
   ioctl semantics, capability handling and rendering must still be tested.
5. Keeping the existing firmware pin avoids an unrelated firmware refresh during
   the kernel migration. Required GSP bytes already exist in that Git revision.

A 6.18 migration is not selected merely because the host runs that family: the
audit identified no AD106 requirement that forces that additional change. A
Nouveau-only backport onto 6.6 is not selected because its shared scheduler/GPUVM
and header dependency closure would become a second maintained graphics baseline.
Revisit this decision if C1 or actual hardware tests expose a concrete requirement.

## 3. Userspace findings and packaging contract

`scripts/setup-arch.sh` now uses pacman and upstream Arch packages. Its
`install_apk` function and Alpine branch arguments are historical compatibility
names. The optional minimal image remains Alpine/musl with Mesa 24.2.8; it is
not the selected first NVIDIA userspace. The previous plan's blanket Alpine/musl
assumption was incorrect for the main image.

The main rootfs contains Zink and Nouveau Gallium/VA assets, plus Radeon and LVP
Vulkan ICD JSON files. It contains neither `libvulkan_nouveau.so` nor a Nouveau
Vulkan ICD. Merely including `mesa-dri-gallium` in old installation calls does
not provide the selected NVK stack.

The exact NVK package was downloaded and unpacked under the audit directory.
Its SHA-256 matches the cached pacman repository metadata:

```text
b5f2b6e4519e7068f71c017855d42d606e3ae393d18c50209c16d4bf74f4c87c
```

It provides `usr/share/vulkan/icd.d/nouveau_icd.json`, which names
`libvulkan_nouveau.so` and declares Vulkan API version `1.4.354`. This metadata
does not establish conformance or working API exposure on AvoryOS.

The inspected Mesa 26.2.5 source archive has SHA-256
`794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5`.
Source archive identity is recorded; an upstream detached-signature validation
was not performed. The NVK package was not installed or executed.

The winsys source checks driver identity/version, initializes explicit VM state,
queries PCI/device/engine information, creates a context and uses VM_BIND/EXEC.
Some newer GETPARAM queries have explicit fallbacks; preserve those distinctions
instead of exposing unsupported capabilities. Native DRM node enumeration and
libdrm's PCI/sysfs discovery also need runtime verification.

For a source build, this Mesa revision requires Rust >=1.85, Meson >=1.7 and
bindgen >=0.71.1, rejecting its documented bad versions. Host Rust is 1.94.1;
the full build environment is recorded in the lock. Prefer the inspected prebuilt
package path initially, while aligning Mesa split versions. Do not install a
rolling set of unrelated package upgrades as part of the audit.

## 4. Exact firmware closure

The selected AD106 GSP path requests these four plain files:

| Requested suffix under `nvidia/ad106/gsp/` | Bytes | Canonical repository location |
|---|---:|---|
| `booter_load-535.113.01.bin` | 55,928 | `nvidia/ad102/gsp/` |
| `booter_unload-535.113.01.bin` | 39,800 | `nvidia/ad102/gsp/` |
| `bootloader-535.113.01.bin` | 32,876 | `nvidia/ad102/gsp/` |
| `gsp-535.113.01.bin` | 38,061,600 | `nvidia/ga102/gsp/` |

WHENCE aliases AD106 to AD102, and AD102's GSP image to GA102. The firmware
repository stores some aliases as WHENCE links rather than files in Git. Thus
listing Git paths under `nvidia/ad106` alone misleadingly finds no blobs. The
audit extracts canonical blobs into the exact request paths in its private cache.
The installer will need to preserve/materialize those aliases when staging later.

WHENCE identifies these as redistributable under the NVIDIA firmware license;
the pinned license file is `LICENSES/LICENCE.nvidia`. Retain its notice when
shipping the firmware. The cache includes the license and WHENCE for inspection.
Hashes and canonical source paths are in the lock and firmware hash manifest.

Two concrete LinuxKPI gaps occur before any hardware experiment:

- `firmware.c` rejects files over 16 MiB, so the 38,061,600-byte GSP image cannot
  load. A larger bounded file buffer and matching release semantics are needed.
  Do not assume the file buffer itself must be DMA-coherent; GSP DMA buffers have
  separate upstream allocation and mapping contracts.
- `nvkm_firmware_get` calls `firmware_request_nowarn`. The current import declares
  it, but no LinuxKPI implementation was found. Provide its synchronous semantics
  and correct failure behavior before enabling the driver.

The installer already decompresses `.zst`/`.xz` inputs into plain staged files.
Kernel-side decompression is not a requirement for this planned staging path.
The existing AMD-only checksum file must remain enforced; add a separately pinned
NVIDIA closure instead of weakening that check. No active manifest was changed.

## 5. Shared migration census

The audit invoked the existing Kbuild evaluator against isolated source trees
with temporary in-memory Nouveau/GPUVM/debug settings. This did not change
`autoconf.h` or generate a new active `Makefile.kbuild`.

| Check | Observed result |
|---|---|
| Existing AMD evaluation | 656 objects, 33 per-file rules |
| Existing display helper evaluation | 8 objects |
| Old Nouveau evaluation for comparison | 691 objects |
| Candidate Nouveau evaluation | 715 objects; no missing selected sources or unsupported constructs reported |
| Candidate display helpers | 8 objects |
| Candidate AMD evaluation | Stops at unsupported `$(call test-lt,...)` in DML build fragments |
| Manual imported source manifest | 113 translation units; all paths exist in candidate; 78 differ |
| Overlay headers matching an old upstream `include/` path | 97 |
| Those upstream counterparts changed/removed in candidate | 76 |
| Changed counterparts wrapped with `include_next` | 10 |
| Active AMD patch | All three hunks dry-run successfully with 280-line offsets |

These counts are a source/build-grammar census. They are not the number of
missing LinuxKPI APIs, a full compilation result or a guarantee that all new
Kconfig dependencies are enabled correctly.

Specific migration dependencies already established:

- Candidate `drm_sched_init` adds submission-workqueue, runqueue and credit
  parameters. `drm_sched_job_init` adds credits and client identity. Import the
  scheduler and adapt callers/overlays together, including AMD.
- Candidate Nouveau selects DRM_GPUVM and DRM_CLIENT_SELECTION, among its other
  Kconfig dependencies. Audit their helper/source closure and intended client/fbdev
  behavior; do not define config symbols just to silence missing declarations.
- Current build generation collects AMD-local flags and places combined output
  under `KPI_AMDGPU`. Independent Nouveau/shared groups are necessary.
- Current `KBUILD_MODNAME` is one constant. Parameter dispatch retries a dotted
  argument using only its suffix. Driver qualification and duplicate names need
  real ownership before both drivers are enabled.
- The active source patch is
  `scripts/linux/patches/0002-gfx10-kiq-hqd-workarounds.patch`. Disabled/debug
  patches are not active. Successful application does not prove the workaround
  is still necessary or correct on the new baseline.
- `kernel/GNUmakefile` also renames selected imported functions and injects
  AMD-specific compatibility hooks. Audit these call boundaries and the retained
  TTM/fence overrides during C1; source patch success alone is insufficient.

The detailed caller/API/behavior/owner/test ledger is the CSV alongside this
report. Its initial entries distinguish missing implementations, migration work,
partially verified primitives, optional scope and physical paths still untested.

## 6. Baseline validation and preserved artifacts

Passing host test programs, with raw results under `build/nouveau-c0/tests/`:

1. `test_kpi_dma_masks.py`: high memory, mask/overflow checks, SG population/rollback.
2. `test_kpi_completion.py`: zero-time and competing waits, publication lifetime,
   all wait APIs, signals and 32,000 completion credits.
3. `test_kpi_mmio_ordering.py`: MMIO widths and normal-memory ordering contracts.
4. `test_firmware_bundle_pin.py`: source hash enforcement and failed-bundle isolation.
5. `test_kpi_rcu_lifetime.py`: grace periods, callbacks, concurrent waiters/barriers.
6. `test_kpi_pci_access_lifecycle.py`: config bounds and managed PCI lifecycle.
7. `test_kpi_ttm_eviction.py`: empty/occupied managers, fences, rollback and balance.

The unmodified `gpu-pipeline-boot.py --software --run` preparation fails because
its hardcoded musl compiler is absent. An audit-only imported-module harness
substituted host `cc` for the small statically linked init program and redirected
output into the audit cache. The repository helper was not edited. An attempted
substitution using the existing glibc cross wrapper also failed; neither failed
attempt booted a guest. Only the host-cc adaptation reached the eight-CPU boot.

Eight-CPU results:

- Software oracle: `failures=0`, process exit 0, normal guest shutdown.
- Texture/mipmap/alpha and fog readbacks pass; UBO cases report no mismatches.
- Two overlap cases are inconclusive because no pending prior fence was observed.
- Kernel boot suite: `LinuxKPI: semaphore wrong result`.
- Firmware suite: private image lacks `test_fw.bin`; its 11 staged AMD blobs match
  their expected CRCs. This missing test file is a fixture failure, not evidence
  that the GSP loader is working or broken.
- Some device-dependent suites skip because the fixture has no matching hardware.
  Missing SATA/ATA messages are not treated as GPU regression evidence.

The one-CPU variant used the same ISO and a reflink copy of the private disk.
It reported `/bin/gpu-probe-init` not found, fell back to the kernel shell and
was terminated by the harness after 100 seconds without an oracle result. Its
cause is uninvestigated; do not label one-CPU baseline boot as passing. This
audit does not identify the semaphore failure's root cause either: the test
requires both successful wakeup and a minimum elapsed-jiffies threshold.

Preserved under `build/nouveau-c0/preserved/`:

- Kernel, ISO, Linux pin, config, import/build manifests and firmware manifest.
- Disk image reflink, retaining captured bytes without a full 12 GiB copy.
- Manifest of file sizes, hashes and preservation paths.

Kernel SHA-256:
`27d3e903323a57706ec06e71fa3a4e3c89b5fdf06109eaeaf6a7b2615f8d1144`.
ISO SHA-256:
`76c2a73aaa4b8f321d9f9b3fd63e75442f9e2c9d32e61689c5152731277b8d3e`.
Both active files still match these preserved hashes after the audit tests.
The disk reflink is not a guest-quiesced or filesystem-consistency guarantee.

No interactive desktop, physical AMD regression, physical NVIDIA test, Vulkan
NVK runtime or candidate-kernel boot was performed. These remain separate gates.

## 7. C1 entry work and decision

C0 has enough source evidence to select a concrete migration input and define
the next work. It does not justify declaring the existing regression suite green
or enabling NVIDIA probing.

The first C1 changes should be:

1. Make the private software regression fixture reproducible with the current
   toolchain, include its required test firmware and diagnose the one-CPU fixture
   startup. Reproduce/classify the semaphore result without a graphics migration.
2. Isolate selectable imports/generated configuration and retain the old artifacts.
   Extend Kbuild evaluation for the demonstrated candidate syntax with validation.
3. Migrate shared source/headers and version-sensitive shims in bounded groups;
   validate scheduler interfaces and existing AMD compatibility hooks first.
4. Build and boot the candidate with Nouveau disabled; compare against the recorded
   baseline. Schedule AMD physical regression separately from host iGPU use.

Keep the later Nouveau build/gate work distinct from this shared migration.
The four-file firmware closure and missing loader API are already actionable
inputs for C3/C4, but no driver binding is needed to implement or test them.

The source patch policy remains the existing repository policy plus its observed
tracked exception: prefer overlays/native bridges and never hand-edit generated
imports. C0 records the discrepancy between older “unmodified source” prose and
the active tracked patch; it does not authorize arbitrary new vendor-source edits.

## 8. Reproduction and evidence locations

Read-only hardware commands used include `lspci -nnk`, `lspci -vv`,
`nvidia-smi --query-gpu=...`, and reads of PCI/DRM sysfs identity, resource,
group, reset-method, status and mode files. PCI capability access was restricted
for some fields. No private Xerj key was read or printed.

Source/evidence locations:

```text
build/nouveau-c0/linux-candidate/        isolated v6.12.113 source clone
build/nouveau-c0/mesa-source/            Mesa 26.2.5 source archive contents
build/nouveau-c0/nvk-package/            inspected package, not installed
build/nouveau-c0/firmware/               extracted canonical blobs at request paths
build/nouveau-c0/source-comparison.json  evaluator/header/source census
build/nouveau-c0/patch-dry-run.log       AMD patch applicability, no source writes
build/nouveau-c0/*-*-objects.txt         selected source manifests for compared roots
build/nouveau-c0/tests/                 host test logs and harness output
build/nouveau-c0/software-pipeline/      eight-CPU private image and serial/host logs
build/nouveau-c0/software-pipeline-1cpu/ one-CPU private fixture and logs
build/nouveau-c0/preserved/              old artifacts and preservation manifest
```

The JSON lock retains the census and test summaries so the conclusions remain
reviewable if ignored build-cache files are later removed. Re-run the documented
host tests on the exact recorded commit and compare fixture-specific boot evidence
before treating any newly built image as an equivalent baseline.

Xerj refresh again failed on `docs/amdgpu-testing.md` changing document family;
the audit used bounded source reads, exact `rg` searches and isolated source
evaluation rather than relying on a stale index.

Upstream sources:

- [Kernel.org release metadata](https://www.kernel.org/releases.json).
- [Pinned Linux stable source](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tag/?h=v6.12.113).
- [Mesa 26.2.5 source archive](https://archive.mesa3d.org/mesa-26.2.5.tar.xz).
- [Mesa NVK documentation](https://docs.mesa3d.org/drivers/nvk.html).
