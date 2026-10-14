# Failed-probe doorbell lifetime correction (2026-10-14)

## CONFIRMED BUG: owned kernel doorbell BO outlives its TTM manager

The user's failing guest records PSP ring creation at GPU address
`0xf400210000`, size 4096, timeout 100000 us. The failed wait at register
`0x16080` expects `0x80000000` under mask `0x8000ffff`, observes
`0x80020115`, and returns `-62`; upstream hw_init subsequently reports `-22`.
The meaning of status `0x0115` is unknown. Raphael PSP 13.0.5 uses
`psp_v13_0_funcs`, not `psp_v13_0_4_funcs`.

The later CPU fault is separately explained by the software ownership path:

1. `amdgpu_ttm_init()` allocates `doorbell.kernel_doorbells` in the doorbell
   manager (`amdgpu_ttm.c:2002`; `amdgpu_doorbell_mgr.c:165`). This is a pinned,
   mapped BO with an owned reference retained in the device.
2. Failed PCI probe releases managed DRM state and calls
   `amdgpu_driver_release_kms()` / `amdgpu_device_fini_sw()`.
3. `amdgpu_device_fini_sw()` first calls `amdgpu_device_ip_fini()`
   (`amdgpu_device.c:4145`). Reverse IP teardown reaches GMC
   `gmc_v10_0_sw_fini()` / `amdgpu_bo_fini()` / `amdgpu_ttm_fini()`.
4. The imported TTM finalizer destroys the doorbell manager and TTM device
   (`amdgpu_ttm.c:2080–2081`) without releasing the kernel doorbell BO.
   Pinned objects cannot be removed by ordinary LRU eviction.
5. Later `amdgpu_device_fini_sw()` calls `amdgpu_doorbell_fini()`
   (`amdgpu_device.c:4179`), which calls `amdgpu_bo_free_kernel()`.
   Unpin followed by unreserve moves the resource to its priority LRU.
   `ttm_resource_move_to_lru_tail()` dereferences the now-NULL manager
   (`ttm_resource.c:164–165`). Priority 2 explains the fault at address `0x60`.

The reported original ELF call at `0xffffffff801bbaa2`, return address
`0xffffffff801bbaa7`, is doorbell cleanup. Discovery cleanup is not this caller.
Addresses have changed in the rebuilt ELF; use its symbols for future traces.
This defect explains the observed post-failure CPU crash, not the preceding
PSP failure. No evidence connects this software cleanup defect specifically
to the physical host reboot.

## Applied correction

[amdgpu_teardown.c](/home/offihito/AvoryOS/kernel/linuxkpi/src/amdgpu_teardown.c:9)
provides the normal `amdgpu_ttm_fini` entry point. While `mman.initialized` is
true it releases kernel doorbells using the imported cleanup function, then
calls the imported TTM finalizer. The imported free operation clears both BO
ownership and the CPU mapping, so later/earlier/repeated doorbell cleanup is
safe. The boundary preserves IP teardown ordering and handles partial TTM
initialization after doorbell allocation.

[kernel/GNUmakefile](/home/offihito/AvoryOS/kernel/GNUmakefile:273) renames only
the imported TTM finalizer and enables the owned wrapper when AMDGPU is built.
No imported source was edited. Comparing 491 AMDGPU C/header files with the
cached upstream tree found 490 identical files and one pre-existing modified
`gfx_v10_0.c` containing older KIQ/HQD workarounds. That difference was left
untouched; the imported tree as a whole cannot be described as pristine.

## Validation and limits

- `test_amdgpu_doorbell_teardown.py`: PASS under ASan/UBSan. Compiles the owned
  wrapper and extracted imported doorbell/free BO functions, with modeled TTM
  dependencies. Checks priority-2 pinned BO cleanup while managers are live,
  partial init, repeat and early cleanup, and device isolation. Removing the
  wrapper's cleanup reproduces a live-BO manager-destruction assertion failure.
  AMD-disabled wrapper compilation also passes. This is a CPU simulation,
  not execution of AMDGPU probe on a real or emulated GPU.
- `test_kpi_psp_ring.py`: PASS under ASan/UBSan. Added the reported ring address
  and opaque `0x80020115` response as a failed-handshake case; preserves `-62`
  and the observed value. No status decoding, extra commands, or fake success.
- `test_gpu_resource_hooks.py`, `test_kpi_devres.py`, and
  `test_kpi_ttm_eviction.py`: PASS, including existing negative controls.
- Kernel and main ISO builds: PASS. Linked disassembly shows the ordinary
  finalizer calls doorbell cleanup before delegating to the imported finalizer;
  `amdgpu_bo_fini` relocates to the owned entry point.
- `ttm-rollback-boot.py --cpus 8`: PASS with real TTM, devres, firmware and sysfs
  self-tests and orderly guest shutdown in private QEMU/KVM. No VFIO device or
  main disk attached. This boot exercises generic TTM rollback, not the AMDGPU
  wrapper's hardware failure path.
- Main ISO kernel was extracted and compared byte-for-byte with the current
  stripped ELF: PASS (SHA-256
  `93fffabb0764d43f0183b2edbdba94450225410f3c2769d46a9fd8a1aee00730`).
- `git diff --check`: PASS.

Build logs: `build/logs/amdgpu-doorbell-build.log` and
`build/logs/amdgpu-doorbell-iso.log`. CPU regression log:
`build/logs/amdgpu-doorbell-regressions.log`. Private boot results:
`build/logs/amdgpu-doorbell-rollback.log` and
`build/ttm-rollback-boot/kvm8-serial.log`.

No physical GPU initialization/reset run was performed. No host bridge, VFIO,
power or firmware settings were changed. QEMU close/reopen PSP failure, physical
host reboots, pixel corruption and Minecraft FPS instability remain unresolved.
The cleanup fix needs validation in a separately authorized failed-probe hardware
run. Continue read-only host evidence collection before considering that run;
retain original PSP errors and full serial/register traces. Hardware reset-state
and firmware-state explanations remain UNVERIFIED HYPOTHESES.

The mandated Xerj refresh was attempted but aborted because the existing
`docs/amdgpu-testing.md` index family changed from `txt-lines` to `txt-prose`.
Investigation therefore used bounded current-source reads and exact `rg` searches.
