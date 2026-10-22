# C2 Nouveau import and independent controls

Updated 2026-10-19. C2 started following the user's reported passing C1 AMD
desktop, games and `glxinfo` smoke test. Changes are in the active checkout.

## Implemented

The complete upstream `drivers/gpu/drm/nouveau/` tree is imported from the
same Linux v6.12.113 commit as shared DRM and AMD. Recursive upstream Kbuild
selects 715 Nouveau sources, including NVIF, NVKM and display code.

`KPI_NOUVEAU ?= 0` is independent of `KPI_AMDGPU ?= 1`. The generated
fragment gates each driver separately, includes display helpers once, and
scopes include/FPU flags to their owning objects. Separated `-I path` syntax
is handled. Both gates are recorded in the build-profile stamp. Repeated
generation is identical, and the retained 6.6 generator run still succeeds.

| AMD gate | Nouveau gate | AMD sources | Nouveau sources | Display helpers |
|---|---|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 |
| 1 | 0 | 776 | 0 | 8 |
| 0 | 1 | 0 | 715 | 8 |
| 1 | 1 | 776 | 715 | 8 |

These are generated driver-fragment counts. Generic shared DRM remains in
`Makefile.files` with both gates off. This table tests source selection and
flags. All four kernel links were subsequently verified in C3.

### Parameters and PCI gate

Static parameters carry `KBUILD_MODNAME` namespaces. AMD uses `amdgpu`,
Nouveau uses `nouveau`, and DRM's debug parameter uses `drm`. Qualified
`debug` parameters reach the correct string/numeric storage; `modeset` and
`runpm` no longer collide across drivers. Ambiguous bare names are rejected.
Legacy qualified aliases remain for unscoped parameters without redirecting
qualified driver parameters into another driver's storage. The module header
preserves compiler-supplied names.

The PCI bridge defaults to `kpi_nouveau=0`. Probing additionally requires an
explicit canonical **guest** BDF in `kpi_nouveau_bdf`, a matching NVIDIA
display-class device and a matching PCI ID. Empty, malformed and nonmatching
selectors never mean all devices. NVIDIA audio functions are rejected.
Native identity supplies the real BDF because Linux bus wrappers are currently
flattened. This selector is separate from the host BDF used by VFIO.

No NVIDIA device was assigned, rebound, reset or initialized. The real driver
was not yet linked at the C2 checkpoint; C3 subsequently verified real
registration with the hardware gate closed.

### Configuration

The Nouveau selection symbol and debug constants are defined. Existing
DRM/TTM/scheduler/exec/display/I2C facilities remain. Tegra/platform, SVM/HMM,
ACPI laptop integration, optional backlight and runtime PM stay disabled.
Remaining Kconfig selects are not blindly defined: GPUVM/maple-tree and
optional IOMMU, power-supply and related facilities need real C3 implementations
or correctly disabled paths. A symbol does not establish native support.

## Validation and remaining work

- `test_nouveau_build_matrix.py` passes all four source-selection combinations,
  uniqueness, display ownership and actual target-specific flag checks.
- `test_kpi_parameter_namespaces.py` runs the real parser and metadata macros
  with colliding driver names under ASan/UBSan and passes.
- `test_kpi_nouveau_probe_gate.py` runs actual PCI registration/probe functions
  with synthetic devices. Default registration performs zero probes. Missing
  targets, wrong domains/addresses and audio stay blocked. AMD stays independent.
- AMD-only kernel build and strict eight-CPU software boot pass:
  `build/nouveau-c2-amd-build.log`, `build/nouveau-c2-amd-boot8.log`.
  Its saved kernel is `build/nouveau-c2-amd-kernel`; no physical GPU is assigned.
- Shared-only kernel build passes with both driver gates off:
  `build/nouveau-c2-shared-build.log`, `build/nouveau-c2-shared-kernel`.
  Its strict one-CPU software boot also passes
  (`build/nouveau-c2-shared-boot1.log`).
- The compiled AMD candidate contains 109 unique qualified parameters; ELF
  inspection confirms the DRM/AMD namespaces and separate Nouveau controls.
- Import failure/interruption rollback and existing Kbuild regressions pass.
  The importer test selects a matching local clone without fetching.

The first keep-going Nouveau-only compile produced 262 object files and
failed before linking. It reports 52 distinct compiler diagnostics in 858
instances. Many repeat a few gaps: `kref`, maple-tree, I2C driver types and
registration, ELF types, DMA helpers and ACPI guards. See
[c2-compile-gaps.csv](c2-compile-gaps.csv) and
`build/nouveau-c2-nouveau-build.log`. This first C3 inventory is not exhaustive;
fixing a common header can expose further errors.

**C2 is complete.** C3 subsequently linked the real Nouveau driver and verified
its registration with the gate closed and zero probe attempts in a guest.
The initial compiler inventory above is retained as the C2 checkpoint. See
[c3-compatibility.md](c3-compatibility.md) for implementation and validation.
Hardware probing stays off by default.

C2 has not replaced the current boot kernel or ISO. Candidate builds use a
separate `kernel-c2` output via `build/nouveau-c2-kernel.make`; the saved
profile-specific kernels identify the configurations tested.
