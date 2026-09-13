# Phase 6 — amdgpu bring-up: chunked execution plan

Audience: the agent that picks up Phase 6.  Paste the project SHARED CONTEXT
with this file.  Written 2026-09-13 after auditing P0–P5 at commit `2d5a0c0`
(tag `p6-baseline`), QEMU 11.1.0, host GPU `0000:0e:00.0` = `1002:164e`
rev c6 (Raphael iGPU, gfx1036 / DCN 3.1.x), Linux pin **v6.6.156
(`8b73de7`)**, 108 imported Linux objects, 595 imported `T drm_*` symbols.

Phase goal (original): run the unmodified upstream Linux 6.6 `amdgpu` driver
on the Raphael iGPU under `make run-vfio` — probe → firmware/PSP/SMU/GMC →
rings/VM → KMS → unmodified Mesa radeonsi 3D → stability.  Stages 6a–6g from
the original sketch are preserved here as chunks C2–C8.

**Status 2026-09-13: planned, nothing started.  Phase 5 exit is green
(`p6-baseline`) except the 24 h user soak, which is carried as the only open
Phase 5 item.**  Do the chunks in order; every chunk ends with the full boot
regression (all `test_phase*` suites + userland `bin/test_kpi_drm` /
`bin/test_kpi_dmabuf` + interactive desktop) and a
`docs/linuxkpi-progress.md` entry with serial evidence.

---

## 0. Readiness audit — what P0–P5 actually shipped (reused by P6)

Already true and reused by this phase:

- **Import pipeline**: `scripts/linux-import.sh` vendors the pinned subset
  into `kernel/linux/` and generates `kernel/linux/Makefile.files`;
  `scripts/linux/subset.txt` (paths copied) and `scripts/linux/files.txt`
  (upstream `.c` compiled) are hand-maintained.  `kernel/GNUmakefile` gives
  imported code its own include order (overlay first, Linux tree second,
  legacy `src/include` stubs deliberately absent) and a dedicated object
  rule; LinuxKPI sources/tests are auto-globbed, so only generated object
  lists need manifest changes.
- **Config**: `kernel/linuxkpi/include/generated/autoconf.h` is
  hand-maintained (absent symbol = disabled; never define `0`).
- **Initcalls**: `module_init()` → `.initcall6.init`; walker runs from a
  `kpi/initcalls` kthread after `linuxkpi_param_init()`, before
  `linuxkpi_run_boot_tests()` (`kernel/src/kernel.c:604-630`).  A built-in
  PCI driver therefore registers and probes at boot with no module loader.
- **FPU**: `kernel_fpu_begin()/end()` over per-CPU XSAVE, nesting-safe
  (`kernel/src/linuxkpi/fpu.c`); this is what `display/amdgpu_dm/dc_fpu.c`
  wraps for the DML float code.
- **PCI/IRQ/ACPI/i2c/sysfs/devres**: full Linux PCI surface including
  `pci_save/restore_state`, `pci_map_rom` (VFIO-validated), PCIe link
  helpers, `pci_alloc_irq_vectors` → MSI/MSI-X, hard/threaded/shared
  handlers, `devm_request_irq`, dynamic sysfs/device groups, minimal i2c
  core, ACPI table accessor (`CONFIG_ACPI` stays off), synchronous firmware
  loader from `/lib/firmware` (16 MiB cap).
- **Graphics stack**: DRM core + KMS helpers + `drivers/dma-buf` +
  TTM + drm_sched + GEM shmem + vgem/vkms/bochs, all imported; VFS/devfs
  bridge for DRM minors; native `ascentdrm` keeps `/dev/dri/card0`.
- **VFIO loop**: `make run-vfio` (BDF default `0000:0e:00.0`, `rombar=1`,
  `romfile=` auto-detected, `KERNEL_CMDLINE=` knobs, `VFIO_EXTRA=`,
  `SERIAL=`, `DISPLAY_OPT=`); `scripts/vfio-vbios.sh` extracts Raphael's
  VBIOS from the host VFCT (no ROM BAR on this APU), pads to 64 KiB;
  `scripts/vfio-check-rom.sh` compares the guest CRC32.
- **P5 C7 hardware facts** (recorded in `docs/linuxkpi-gaps.md` P5 C7 and
  `docs/amdgpu-testing.md`): BAR0 = 256 MB VRAM, BAR5 = 512 KiB MMIO,
  `msi=1 msix=4`, one MSI vector installs (`irq=101`), ROM
  `crc32=0x20ef861b size=65536` matching `build/vfio/vbios.rom`.
- **Test harness**: 27 kernel suites wired in `linuxkpi/src/boot_tests.c`
  (bounded 60 s), userland `bin/test_kpi_drm` / `bin/test_kpi_dmabuf`, all
  at `-smp 4`, PMM free-page invariants with warm-up baselines.

Open Phase 5 items that P6 inherits (see `docs/linuxkpi-progress.md`
"Open items carried into Phase 6"):

- 24 h desktop soak (user-run protocol; the 1 h agent soak was waived).
- `CONFIG_ACPI` stays off: under VFIO the VBIOS must come from the ROM BAR
  (`romfile=`), because VFCT exists only on the host.
- Firmware manifest is empty; SMU/PSP/DMCUB names are staged in P6c.
- i2c SMBus entry points only if a P6 census shows consumers (it does not:
  amdgpu DM uses `i2c_transfer` + `i2c-algo-bit`).
- PM (runtime/suspend/ASPM/D-states) is inert; `get_device`/`put_device`,
  `kobject_*`, `pci_dev_get/put` are documented no-ops; PCIe `err_handler`
  only needs to compile.
- IRQ dispatch holds the descriptor lock while hard handlers run; threaded
  handlers use the system workqueue; no `IRQF_ONESHOT` masking.
- Bochs canary needs `-device bochs-display`; run-vfio's default std VGA
  breaks its BAR0 readback (documented, avoided with `-vga none
  -device virtio-vga`).
- fastfetch already reports `AMD Raphael` from PCI IDs; the renderer line
  becomes meaningful at P6e/P6f.

Known P6-relevant divergences (full text in `docs/linuxkpi-gaps.md`):
`unmap_mapping_range()` is a no-op; mprotect/mremap re-adds drop the Linux
VMA wrapper; the shrinker is inert; ww_mutex cannot wound; percpu is
uniprocessor emulation; page cache-mode flips are accepted and ignored.

---

## 1. Corrections and decisions to settle before coding

Read this before C1; every item is grounded in the 6.6 tree or the P5
baseline.

1. **The file list must be generated, not hand-curated.**  The amd subtree
   has 749 `.c` files (240 `amdgpu/`, 391 `display/`), and with
   `CONFIG_DRM_AMD_DC=y` the effective `amdgpu-y` set is on the order of
   500 objects — vs. 108 imported objects today.  The 6.6 Makefiles evaluate
   variables, conditionals and `include`s (`amdgpu-y`,
   `AMD_POWERPLAY_FILES`, `AMD_DISPLAY_FILES`, `AMD_DAL_*`, `DML`,
   per-file `CFLAGS_<path>`, `CFLAGS_REMOVE_<path>`).  C1 therefore builds
   `scripts/linux/kbuild-subset.py` (Appendix D): it evaluates the subset of
   Kbuild syntax those trees use against `autoconf.h` and emits
   `kernel/linux/Makefile.kbuild` (object list + per-directory include paths
   + per-file flags) plus a report.  Hand-listing stays the fallback only if
   the generator fails its self-check.
2. **The whole `drivers/gpu/drm/amd/` tree must be imported.**  `include/`
   alone is fine, but `amdgpu/include/asic_reg` is 387 MB / 398 headers;
   copying it pushes `kernel/linux/` to roughly 400 MB.  That is acceptable
   (gitignored; never shipped in the ISO), but it must be documented, and
   the import script's runtime grows.  Pruning unused ASIC headers is a
   possible later optimization, not P6 scope.
3. **Kconfig `select`s do not happen automatically.**  Add the symbols in
   Appendix B and handle the ones we deliberately do not implement:
   - `HWMON`: `pm/amdgpu_pm.c` calls `hwmon_device_register_with_groups()`
     /`hwmon_device_unregister()` unguarded (upstream always selects HWMON).
     Add weak stubs returning `ERR_PTR(-ENODEV)` / no-op in
     `linuxkpi/src/link_stubs.c` or `drm_compat.c`; real hwmon is out of
     scope.
   - `POWER_SUPPLY`: stock `<linux/power_supply.h>` already stubs
     `power_supply_is_system_supplied()` to `-ENOSYS`; no work needed.
   - `INTERVAL_TREE`: `amdgpu_vm.c` uses `INTERVAL_TREE_DEFINE` locally, so
     `lib/interval_tree.c` is **not** required; do not add the symbol until
     a compiled file reads it.
   - `I2C_ALGOBIT`: required (item 7).
   - `PROC_FS`, `PERF_EVENTS`, `COMPAT`, `VGA_SWITCHEROO`, `HSA_AMD`,
     `DRM_AMD_ACP`, `DRM_AMDGPU_SI/CIK`, `HMM_MIRROR`,
     `DRM_AMD_SECURE_DISPLAY`: stay off; those `.c` files are not in the
     generated list.
   - `DRM_FBDEV_EMULATION`: stays off; stock `<drm/drm_fb_helper.h>` then
     provides `drm_fb_helper_lastclose()` as an inline no-op, which is what
     `amdgpu_kms.c` needs (verified).
4. **PSP 13.0.5 is TOC-based, not SOS.**  In `psp_v13_0.c` the
   `IP_VERSION(13,0,5)` case calls `psp_init_toc_microcode()` +
   `psp_init_ta_microcode()`, i.e. it requests
   `amdgpu/psp_13_0_5_toc.bin` + `amdgpu/psp_13_0_5_ta.bin`.  The `_sos`
   names belong to 13.0.0/6/7/10.  Fix the manifest comment in
   `scripts/linux/firmware-manifest.txt` before 6c.
5. **VCN firmware is requested during early_init and is mandatory for
   probe.**  `amdgpu_vcn_early_init()` builds `amdgpu/%s.bin` from the UVD
   IP version (`vcn_3_1_2.bin` for Raphael) and fails the block's early
   init when missing — so the manifest must include it from 6b on even
   though video decode is out of scope.
6. **The DMUB filename depends on the DCN IP version.**  `amdgpu_dm.c` maps
   `IP_VERSION(3,1,4/5/6)` to `dcn_3_1_{4,5,6}_dmcub.bin`; stage all three
   candidates until 6b logs the actual DCN revision, then keep the one.
7. **`amdgpu_i2c.c` needs `i2c-algo-bit`.**  It calls
   `i2c_bit_add_bus()` (line 206); our minimal i2c core has no bit-banger,
   and without `CONFIG_I2C_ALGOBIT` the stock header compiles the call to
   `-ENODEV` (DDC would silently fail and EDID would never read).  Import
   `drivers/i2c/algos/i2c-algo-bit.c` (header already present via the
   `include` copy) and check the overlay `struct i2c_adapter` fields it uses
   (`algo_data`, `timeout`, `retries`, `bus_lock`, `quirks`).  This is
   C6's (6e) DDC prerequisite; do it in C1 so it links early.
8. **DC FPU uses per-CPU scratch.**  `display/amdgpu_dm/dc_fpu.c` wraps DML
   in `kernel_fpu_begin/end` (P0) and keeps a recursion depth via
   `DEFINE_PER_CPU(int, fpu_recursion_depth)` + `get_cpu_ptr()` /
   `put_cpu_ptr()`.  Our `percpu.h` is uniprocessor emulation and lacks the
   `get_cpu_ptr`/`put_cpu_ptr` pair; add them (single instance is safe under
   preempt-disable) and audit other per-CPU uses in the compiled subset
   (counters/arrays may need `this_cpu_*`/`per_cpu_ptr` semantics).
9. **DML files need SSE re-enabled per file.**  The 6.6 x86 build compiles
   DML with `-mhard-float -msse -msse2`; our global CFLAGS carry
   `-mno-80387 -mno-mmx -mno-sse -mno-sse2`, so the generated per-file
   `LINUX_CFLAGS` for the `display/dc/dml/` objects must append those flags
   (order matters: they come after the global ones).  Only those files may
   have SSE; verify with `objdump -d` that a non-DML object contains no SSE
   instructions.
10. **A probe gate is needed to keep VFIO boots safe while 6a–6c iterate.**
    `amdgpu_init()` runs at initcall time and our PCI registry probes
    synchronously at registration, so a `run-vfio` boot under `kpi_amdgpu=1`
    hands the GPU to half-initialized code.  Add a documented boot-safety
    gate: a `kpi_amdgpu` module parameter (default 0 until C3, flipped to 1
    in C3) checked by the LinuxKPI PCI probe registry for the `amdgpu`
    driver only.  This is a deliberate non-upstream knob; record it in
    `docs/linuxkpi-gaps.md`.  Keep the default-0 path green: a gate-off
    `run-vfio` boot must behave exactly like today.
11. **Minor numbering shifts with amdgpu linked in.**  Native `ascentdrm`
    keeps `card0`; vkms is `card1` (driver_features has no `DRIVER_RENDER`);
    vgem owns `renderD128`.  amdgpu therefore lands on `card2` /
    `renderD129` in the current build.  All P6 tests must discover the node
    by `DRM_IOCTL_VERSION` name (as `test_phase4_bochs.c` discovers
    `card2`), never hard-code the number; P7 flips the product defaults.
12. **VM invalidation gaps must be closed before CS tests (C5).**
    `unmap_mapping_range()` is a no-op and `vma_mprotect()`/`sys_mremap()`
    re-adds bypass `vma_attach_linux()`, so amdgpu BO eviction/moves and
    mmap teardown can leave stale PTEs.  Implement a real
    `unmap_mapping_range()` for Linux-bridged VMAs and route every VMA
    re-add through the bridge before trusting 6d results.
13. **ww_mutex has no wound.**  amdgpu CS chains ww_mutex acquisitions for
    BO reservations; the current FIFO fallback survived TTM tests but
    multi-lock eviction can deadlock.  C5 runs a pinned-vs-unpinned
    eviction/contention stress and either implements wound/wait or
    documents the strict-ordering assumption with a WARN.
14. **IRQ model limits are acceptable but must be respected.**  Hard
    handlers run with the descriptor lock held (P5 C2), threaded handlers
    run on system_wq, `IRQF_ONESHOT` does not mask in hardware.  amdgpu's
    IH handler only schedules work, so P6 is expected to be safe; C4 must
    still prove IH delivery and clean teardown, and any handler that calls
    `request_irq`/`free_irq`/`disable_irq` on itself is a bug to fix by
    design, not by locking changes.
15. **ACPI stays off; VBIOS only via the ROM BAR.**  Under VFIO,
    `romfile=` supplies it; `amdgpu_acpi.c`/ATPX are not built.  Bare-metal
    VFCT/ACPI is P8.  `amdgpu_discovery` defaults to the VBIOS/memory path
    (`amdgpu.discovery != 2`), so `ip_discovery.bin` is an optional
    fallback only.
16. **Debug output comes through `KERNEL_CMDLINE`.**  `drm.debug=0x1ff`,
    `amdgpu.runpm=0`, `amdgpu.modeset=0`, `kpi_amdgpu=1` are all parsed by
    the P1 module-param/`__setup` machinery from the Limine `cmdline:`.
17. **Keep the bootable-at-all-times rule with a one-line revert.**  Each
    risky stage gets a documented revert: the probe gate for C2/C3,
    `amdgpu.modeset=0` for C4/C5, `amdgpu.runpm=0` always, and (if ever
    needed) dropping the amdgpu object list from `Makefile.kbuild` to
    return to `p6-baseline`.
18. **`--gc-sections` + `-ffunction-sections` still apply.**  The
    `module_init()` reference anchors the initcall, but data reachable only
    through PCI id tables / `module_pci_driver`-style tables must be
    verified with `nm` after 6a (no silently dropped id table or ops
    struct).

---

## 2. Chunk status board

| Chunk | Title | Size | Depends on | Exit gate |
|-------|-------|------|------------|-----------|
| C0 | Baseline + environment freeze | S | — | P5 matrix re-run green; linux-firmware pinned; VBIOS refreshed; `p6-start` tagged |
| C1 | Import tree + Kbuild evaluator + config census | L | C0 | generator deterministic; helper imports link; first amdgpu TU compiles |
| C2 | **6a** compile/link (long tail) | XL | C1 | kernel links all amdgpu objects; gate keeps `make run`/`run-vfio` bootable; desktop regression |
| C3 | **6b** early init on hardware | L | C2 | full IP dump + early init in serial; clean failure path; no hang |
| C4 | **6c** firmware + PSP/SMU/GMC/IH + rings | XL | C3 | headless `modeset=0` ring tests pass; firmware versions logged; IH IRQ works |
| C5 | **6d** queues/VM/BOs/CS | XL | C4 | userland CS (SDMA copy) works; no VA faults; leak-free loop |
| C6 | **6e** KMS/DCN | XL | C5 | real monitor shows AvoryOS; modetest atomic; flips/vblank; no WARN |
| C7 | **6f** Mesa radeonsi 3D | L | C6 | radeonsi renderer string; kmscube/glxgears fps; PRIME |
| C8 | **6g** stability | L | C7 | reset/injection/pressure soak; known-issues list |
| C9 | Closeout + P7 handoff | S | all | docs + full regression + `p6-done` tag |

Sizes are relative: C0/C9 ≈ a short session; C1/C3/C7/C8 ≈ one session each;
C4/C5/C6 ≈ one to two sessions each; **C2 is expected to be the largest
single chunk of the project** — do it in waves and never leave the tree
unbootable.

Coverage ordering note: C2 and C3 are deliberately separate.  C2 proves the
build/link/bootability without ever letting amdgpu touch hardware; C3 then
enables the gate on the real device.  C4 is headless (no display engine);
C5 is compute/copy only; C6 turns on KMS; C7 is userspace 3D.

---

### C0 — Baseline closeout + environment freeze (gate; do not skip)

Goal: prove P5 exit is still green, pin the external inputs, and freeze the
decisions in §1 before any Phase 6 code lands.

Tasks:

1. Full P5 regression on the baseline:
   `make run-linuxdrm SERIAL=file:build/logs/p6-c0.log
   DISPLAY_OPT='-display none'`; expect all `test_phase*` suites green
   (252 `[  OK  ]` class), no `[FAIL]`, boot to login.
2. Interactive `make run` desktop check and the userland suites
   (`bin/test_kpi_drm`, `bin/test_kpi_dmabuf 60`) — maintainer-run if
   needed, recorded in the progress doc.
3. Refresh the hardware inputs:
   - `sudo scripts/vfio-vbios.sh 0000:0e:00.0`; expected 44,544-byte image
     padded to 64 KiB; record the CRC (`0x20ef861b` at P5 C7).
   - Clone/pin linux-firmware: `LINUX_FIRMWARE_REF=<sha>
     scripts/linux-firmware-install.sh`; record the SHA in
     `docs/linuxkpi-progress.md`.  (Use a known-good ref from around the
     6.6 release; do not float master.)
4. Record the baseline numbers for later comparison: 108 imported objects,
   595 `T drm_*` symbols, `kernel/linux/.pin` = v6.6.156 (`8b73de7`).
5. Freeze the §1 decisions in the progress doc: whole-tree import, Kbuild
   generator, probe gate name/default, i2c-algo-bit import, hwmon stubs,
   minor-numbering acceptance.
6. Tag `p6-start`; add the Phase 6 section pointer to
   `docs/linuxkpi-progress.md`.

Evidence: serial excerpt + exact commands in the progress doc; VBIOS CRC;
linux-firmware SHA.

Exit: P5 matrix re-verified; external inputs pinned; decisions recorded.

Risks: none technical; the only trap is starting C1 before the VBIOS/firmware
pins exist, which makes C3/C4 evidence non-reproducible.

---

### C1 — Import tree + Kbuild evaluator + config census (6a prep)

Goal: make it possible to (re)generate the amdgpu object list from the
pinned tree, with the helper libraries amdgpu selects, and prove a single
amdgpu TU compiles before the full wave.

Test/evidence first: the generator's report is the first deliverable; add a
tiny host-side self-check (a known object such as `amdgpu/gfx_v10_0.o`,
`amdgpu/psp_v13_0.o`, `display/dc/dcn31/dcn31_hwseq.o` must be in the
output), and compile `amdgpu/amdgpu_drv.c` alone before enabling the rest.

Tasks:

1. `scripts/linux/subset.txt` additions:
   - `drivers/gpu/drm/amd` (whole tree; see §1.2 for the size).
   - `drivers/gpu/drm/display` (the `drivers/gpu/drm/*.c` glob is
     non-recursive; the `drm_display_helper` set lives here).
   - `drivers/i2c/algos/i2c-algo-bit.c`.
   Note the root glob already copied `drm_buddy.c`, `drm_exec.c`,
   `drm_suballoc.c` sources; only their `files.txt` entries are new.
2. Write `scripts/linux/kbuild-subset.py` (spec in Appendix D).  Inputs:
   the imported tree, the config map parsed from `autoconf.h`, the top
   directories (`amd/amdgpu`, `amd/display`, `amd/pm`, `drm/display`,
   `i2c/algos`).  Outputs:
   - `kernel/linux/Makefile.kbuild`: `linux-obj-y += <paths>` plus
     target-specific `CPPFLAGS` (per-directory include dirs from
     `ccflags-y`/`subdir-ccflags-y`) and `LINUX_CFLAGS` (per-file
     `CFLAGS_<path>`/`CFLAGS_REMOVE_<path>` — the DML SSE2 set).
   - `kernel/linux/kbuild-report.txt`: object count per directory, config
     symbols seen, unresolved constructs (must be empty), skipped files.
   The generator must be deterministic, fail loudly on unresolved `$(...)`
   or missing `include`s, and never write into the imported tree.
3. `kernel/GNUmakefile`: include `linux/Makefile.kbuild` after
   `linux/Makefile.files`; keep `LINUX_OBJ`/`LINUX_CPPFLAGS`/`LINUX_CFLAGS`
   as the consumption points and verify target-specific overrides apply
   (compile one DML object and check SSE in `objdump`).
4. `autoconf.h`: apply the Appendix B delta.  Keep the "absent = off" rule;
   justify each symbol with the file that reads it (generator report).
5. `scripts/linux/files.txt` additions (hand list, helper libraries):
   `drivers/gpu/drm/drm_buddy.c`, `drivers/gpu/drm/drm_exec.c`,
   `drivers/gpu/drm/drm_suballoc.c`,
   `drivers/gpu/drm/display/drm_display_helper_mod.c`,
   `drm_dp_dual_mode_helper.c`, `drm_dp_helper.c`,
   `drm_dp_mst_topology.c`, `drm_dsc_helper.c`, `drm_hdcp_helper.c`,
   `drm_hdmi_helper.c`, `drm_scdc_helper.c`,
   `drivers/i2c/algos/i2c-algo-bit.c`.
6. First-TU smoke: temporarily list `amdgpu/amdgpu_drv.c` alone, run
   `make -C kernel`, and fix include-path plumbing (per-dir `-I` from the
   generator) before the wave.  Then remove the temporary entry and let the
   generator drive.
7. Record in `docs/linuxkpi-gaps.md` (P6 C1): whole-tree import size,
   generator scope, config selects not implemented (HWMON/POWER_SUPPLY/
   INTERVAL_TREE), and the probe-gate design from §1.10.

Evidence: generator report (object counts, empty unresolved list);
`nm` shows `drm_buddy_*`, `drm_exec_*`, `drm_suballoc_*`, display-helper and
`i2c_bit_add_bus` symbols; first TU compiles; `make -C kernel` clean; ISO
boots with all P0–P5 suites green.

Exit: deterministic generation; helper objects link; one amdgpu TU compiles
against the generated include paths; no regression.

Risks: Kbuild syntax beyond the documented subset (fail loudly, extend the
grammar deliberately); include-dir shadowing across the amd tree (start with
per-directory sets, never one global union); `-I` paths are relative to the
imported tree, so the generator must anchor them to `$(ARCH)`-independent
build paths.

---

### C2 — 6a: compile and link the full amdgpu object set

Goal: the kernel links every object the generator selects, boots with
amdgpu registered but not bound, and keeps the desktop and all prior
evidence green.  No hardware ownership in this chunk.

Test first: `kernel/src/tests/linuxkpi/linux/test_phase6_link.c` wired into
`boot_tests.c` after `test_phase5_vfio`:
- `pci_get_device(0x1002, 0x164e, NULL)` finds the Raphael under `run-vfio`
  (and nothing on plain `make run`);
- with the gate off, `pdev->dev.driver == NULL` (amdgpu did not bind) and
  the PCI driver registry contains an `amdgpu` entry;
- with the gate on but no device, nothing binds and no AMD-specific log
  appears;
- the native desktop path is untouched (`ascentdrm` card0 still owns the
  framebuffer).

Tasks:

1. Tooling: `scripts/kpi-triage.sh` runs `make -C kernel -k` and buckets
   unique diagnostics (`fatal error: … No such file`, `#error`, incomplete
   types, implicit declarations, undefined references), writing a draft
   list for `docs/linuxkpi-gaps.md`.  This is the wave worklist.
2. Compile waves, in this order:
   - **W1 headers/includes/config**: missing stock headers, overlay
     conflicts, `#error` from `autoconf.h`.
   - **W2 types/macros**: incomplete types, layout conflicts, macro
     redefinitions (batch by subsystem).
   - **W3 functions**: missing entry points and inline dependencies; real
     implementations first, weak stubs only where behavior is out of P6
     scope and always marked with the imported file that needs them.
   - **W4 link/rodata**: unresolved symbols, table reachability,
     `--gc-sections` checks.
   Work in descending occurrence count; one gap-log entry per batch (not
   per compiler error), each naming the imported file(s) that surfaced it.
3. Expected clusters (verify, do not assume): per-CPU
   (`get_cpu_ptr`/`put_cpu_ptr`, `this_cpu_*`), hwmon weak stubs,
   debugfs/seq_file off-arms, notifier, DMI quirks, `linux/iosys-map.h`,
   `dma_fence` chains (present), `kref`, `mmu_notifier` off-arms,
   `drm_aperture` weak stub, `vga_switcheroo` inline no-ops, `clk`/`reset`
   no-ops, `pm_runtime` no-ops, `asm/io.h` accessor completeness for the
   `RREG32` macros, `linux/io-64-nonatomic-lo-hi.h`, `linux/math64.h`,
   `linux/overflow.h`, `linux/minmax.h`, `linux/dmi.h`, `linux/profile.h`,
   `linux/reboot.h`, `sysfs` groups, `linux/firmware.h` (present).
4. Implement the probe gate (§1.10) in the LinuxKPI PCI registry, with the
   `kpi_amdgpu` parameter defaulting to 0.  Keep the gate check at the
   registry boundary so gate-off code paths do not even call amdgpu probe.
5. After each wave: `make -C kernel`, boot headless, full suite regression
   (all `test_phase*`, `bin/test_kpi_drm`, `bin/test_kpi_dmabuf`), desktop
   boot when interactive.  A wave that breaks the boot is reverted before
   continuing.
6. Final checks: `nm kernel/bin-x86_64/kernel | grep ' T amdgpu_' | wc -l`
   (record), confirm `amdgpu_pci_driver`'s id table survived `gc-sections`
   (`nm`/`objdump` on the table symbol or a probe-log canary), image size
   before/after, and `make -C kernel clean` from scratch.
7. `docs/linuxkpi-gaps.md` P6 C2 section: one batch per wave entry;
   `docs/linuxkpi-progress.md` with the wave counts, the exact commands,
   and the gate behavior.

Evidence: link clean; `test_phase6_link` green in both gate states; serial
boot to login on `make run` and `run-vfio` (gate off); desktop regression;
symbol count; no `[FAIL]`.

Exit: all amdgpu objects linked; boots always; amdgpu never binds without
`kpi_amdgpu=1`; every P0–P5 suite still green.

Risks: the long tail is the schedule risk — budget waves, not days;
overlay/stock header conflicts (`device.h`, `i2c.h`, `percpu.h`,
`mm_types.h` are the known fragile ones); link time/memory with ~600 extra
objects; `--gc-sections` dropping table-only data; a "temporary" weak stub
outliving its phase (the gap log entry must name the removal phase).

---

### C3 — 6b: early init on the real GPU

Goal: bind amdgpu to the passed-through Raphael, map the BARs/VBIOS, run IP
discovery, and show a complete early-init log without a hang.

Test first: the serial log is the primary evidence; add the gate-on
assertions to `test_phase6_link.c` (bound driver name `amdgpu`, BAR5
mapped, no native device disturbed) as the machine-checkable half.

Tasks:

1. Boot with the gate on and debug enabled:
   ```
   make run-vfio \
     KERNEL_CMDLINE='kpi_amdgpu=1 drm.debug=0x1ff amdgpu.runpm=0' \
     SERIAL=file:build/logs/p6-c3.log DISPLAY_OPT='-display none' \
     VFIO_EXTRA='-device edu'
   ```
2. Verify the bind path end to end: `amdgpu_pci_probe` (1002:164e rev c6),
   `pci_enable_device`, region requests, `pci_iomap` BAR5 (512 KiB),
   `amdgpu_device_init`, `amdgpu_atombios_init` parsing the VBIOS from the
   ROM image, `amdgpu_discovery` IP dump.
3. Record the exact IP versions from the log — expected around GC 10.3.6,
   PSP 13.0.5, SMU 13.0.5, DCN 3.1.x, SDMA 6.0.x, VCN 3.1.2, plus
   NBIO/DF/UMC/SMUIO.  The DCN and SDMA revisions pin the DMUB and SDMA
   firmware names for C4.
4. Failure-path polish: force at least one clean early failure (e.g. boot
   without the ROM file or with `pci=noacpi`-style tweaks irrelevant here —
   use a deliberate gate/missing-file case) and verify `-ENODEV` with a
   clear log and no hang; all waits bounded.
5. Keep `amdgpu.modeset=0` for this chunk and until C6; confirm the native
   desktop on card0 and all prior suites are green in the same boot (the
   gate-on boot is additive).
6. `docs/linuxkpi-gaps.md` P6 C3: anything early init needed at runtime
   (not compile time), e.g. percpu semantics, `readl` paths, discovery
   parsing.

Evidence: serial excerpt with the full IP dump and early-init progress;
exact commands; the clean-failure run; gate-off regression in the same
session.

Exit: complete IP dump + early init in serial without hang; failure path
returns cleanly; gate-off behavior unchanged.

Risks: ATOM BIOS parse failures (record the VBIOS version); IP discovery
mismatch vs the expected Raphael table; BAR access faults if the aperture
mapping is wrong; VFIO ROM visibility during early boot (the ROM BAR is
enabled by `pci_map_rom` — validated at P5 C7).

---

### C4 — 6c: firmware + PSP/SMU/GMC/IH + ring tests (headless)

Goal: load the real firmware set, bring up PSP/SMU/GMC/IH, and pass the
SDMA and GFX ring tests with `amdgpu.modeset=0`.

Test first: extend the firmware self-test (`test_phase5_firmware.c`) to
CRC-check every staged amdgpu blob against the host manifest; add a
gate-on kernel assertion for "amdgpu device initialized" (device count,
firmware versions readable via sysfs/DRM_INFO if exposed) so a silent
failure is caught by the suite, not only by eyeballing.

Tasks:

1. Firmware manifest + staging:
   - Fill `scripts/linux/firmware-manifest.txt` with Appendix C.
   - Wire `scripts/linux-firmware-install.sh` output
     (`build/firmware/lib/firmware`) into the disk image build in the top
     `GNUmakefile`, replacing the `test_fw.bin`-only rule; keep the
     `test_fw.bin` test blob so P5 C3 stays green.
   - Boot-time check service: the loader logs any missing name it is asked
     for; add the manifest names to the test that verifies each file is
     readable and matches the host CRC.
2. Headless run with `kpi_amdgpu=1 amdgpu.modeset=0 amdgpu.runpm=0
   drm.debug=0x1ff`; follow the init chain:
   - PSP: `psp_13_0_5_toc.bin` → SOS alive → TA load; log the firmware
     versions.
   - SMU: firmware load and message handshake (`amdgpu.runpm=0` keeps
     runtime PM out of the picture; do not disable DPM outright unless the
     handshake blocks).
   - GMC/GART/VRAM managers: coherent allocations, page tables, TLB flush.
   - IH ring + one MSI-X vector through `request_irq` (P5 C2 path): prove
     delivery with a counter/log, then confirm clean teardown.
   - SDMA ring test, then GFX ring test (RLC/CP microcode).
   - `amdgpu_device_ip_init` completion and `amdgpu_ib_ring_tests`.
3. IRQ/coherence sanity: interrupt counts exact, no storm; PMM/TTM/dma
   counters stable after init; `dma_fence` timeouts clean.
4. Failure handling: each stage logs its firmware name and error; blocks
   unwind (`amdgpu_device_ip_fini`) without panic; a missing firmware file
   produces a single clear message, not a cascade.
5. `docs/linuxkpi-gaps.md` P6 C4: firmware loader behavior, IH/IRQ model
   interactions, per-CPU needs, SMU message paths.

Evidence: serial excerpt per stage — firmware versions, PSP/SMU status,
`ring test pass` for SDMA and GFX, IH irq and fire count, init complete;
no timeout WARN; gate-off and `make run` regressions in the same session;
firmware CRC test green.

Exit: headless device init completes with ring tests passing; firmware
provenance reproducible from the pinned linux-firmware ref.

Risks: firmware name/version mismatches (log-driven, pin the ref);
PSP mailbox timeouts; SMU message handshake quirks on APU; IH/MSI-X under
VFIO; GMC/GART memory-type handling (cache-mode flips are inert in our
layer — record whether that matters).

---

### C5 — 6d: queues, VM, BOs, command submission

Goal: userland allocates BOs, maps them, submits SDMA work through
drm_sched, and waits on fences — no VA faults, no leaks.

Test first: `userland/test_kpi_amdgpu.c` → `bin/test_kpi_amdgpu` in
`disk.img` (dependency + `debugfs write`, mode 0755), plus kernel-side
prerequisites below.  Raw ioctls (as `test_kpi_drm.c` does) are the
fallback; link `libdrm_amdgpu` from the Alpine libdrm package if it is
present.

Tasks:

1. Kernel prerequisites, before any CS test:
   - real `unmap_mapping_range()` over native VMAs for Linux-bridged
     mappings;
   - route `vma_mprotect()`/`sys_mremap()` re-adds through
     `vma_attach_linux()` (P2 gap);
   - TTM eviction stress (pinned vs unpinned) and a ww_mutex contention
     test; implement wound/wait or document + WARN (§1.13);
   - audit `kref`/refcount/`amdgpu_bo` teardown against PMM counters.
2. Userland suite:
   - discover amdgpu by `DRM_IOCTL_VERSION` name (`card2`, `renderD129`);
   - GEM create in VRAM and GTT, mmap, write/read pattern, close;
   - PRIME export/import (`dma_buf` fd roundtrip) between two fds;
   - `AMDGPU_CTX` create/destroy; `AMDGPU_VM` map/unmap;
   - CS submission with an SDMA copy (pattern source/target in two BOs)
     through `AMDGPU_CS`/`DRM_IOCTL_AMDGPU_CS`; fence `WAIT_CS` with
     timeout; confirm target bytes;
   - timeout path: submit an intentionally bad CS and observe the fence
     timeout/GPU-recovery behavior;
   - `AMDGPU_INFO` version/memory queries; BO list.
3. Kernel tests/evidence: 1k BO create/map/free loop with PMM and TTM
   invariants (warm-up baseline), fence signal/wait stress, VA fault
   counters zero.
4. `docs/linuxkpi-gaps.md` P6 C5: every divergence (userptr off, eviction,
   mapping invalidation semantics, ww_mutex policy).

Evidence: `bin/test_kpi_amdgpu` pass lines (copy verified, fence signaled,
timeout observed, leak-free); kernel loop deltas; exact commands.

Exit: userland CS through SDMA works; no VA faults; leak-free 1k loop;
all prior suites green.

Risks: ww_mutex deadlock under eviction; `unmap_mapping_range` semantics
(native VMA tree vs page cache); CS parser/IB validation gaps; fence
timeout behavior without working reset (C8); userptr-off fallbacks in
libdrm paths.

---

### C6 — 6e: KMS/DCN — connectors, EDID, atomic modeset

Goal: a real monitor connected to the passed GPU shows AvoryOS, driven by
amdgpu's DCN 3.1.x, with atomic modeset, page flip, vblank and cursor.

Test first: extend the raw-ioctl userland test with a KMS sequence (or use
`modetest` from Alpine's `libdrm-tests`, installed by `setup-alpine.sh`)
against the amdgpu card; kernel-side, keep the boot log evidence primary.

Tasks:

1. i2c/DDC: land the `i2c-algo-bit` import from C1; verify `amdgpu_i2c`
   adapters register and DDC transfers complete; log the monitor's EDID
   product string once.
2. DMUB firmware: stage the right `dcn_3_1_{4,5,6}_dmcub.bin` (pinned in
   C3); `dc_hw_init`; `amdgpu_dm` init; connector detection.
3. KMS: EDID modes, link training for the attached connector, atomic
   enable at 1920x1080@60 where supported, page-flip event, vblank
   (`WAIT_VBLANK`), cursor plane; HPD IRQ handling; then a second display
   if available.
4. Userspace: `modetest` connects/lists resources and performs an atomic
   modeset; run XFCE/KDE on the amdgpu card as a stretch goal; the native
   card0 desktop keeps working throughout.
5. `docs/linuxkpi-gaps.md` P6 C6: DDC timing, HPD/IRQ model, any DCN
   divergence; `docs/amdgpu-testing.md` gets the monitor setup note (output
   appears only on the GPU's physical connectors; the host must not depend
   on the passed GPU for its own display).

Evidence: monitor showing AvoryOS (description/photo), `modetest`
connector/mode/CRTC output, flip/vblank counters, EDID string, no WARN.

Exit: real monitor output through amdgpu; single 1080p60 stable; page
flip/vblank/cursor exercised; multi-display attempted and recorded.

Risks: DMUB revision; bit-banged DDC timing on this board; link training
under VFIO; HPD through IH; DCN register access/coherence; a monitor and
cable physically attached to the passed GPU are required for this chunk.

---

### C7 — 6f: 3D through unmodified Mesa radeonsi

Goal: unmodified Mesa radeonsi renders through amdgpu's render node with
real fps numbers, proving the uAPI surface matches 6.6.

Test first: `glxinfo -B` / `eglinfo` renderer string must say
`AMD ... (radeonsi, ...)`; then kmscube/glxgears/weston-simple-egl, then a
game.

Tasks:

1. Userspace inventory: confirm `mesa-dri-gallium` (radeonsi), libdrm and
   the test binaries in the Alpine rootfs; point the session at amdgpu's
   render node (card2/renderD129 expected) without breaking the native
   card0 path.
2. Run: kmscube, glxgears, weston-simple-egl (Wayland), then a game;
   record fps numbers and the renderer string; PRIME/dma-buf roundtrips.
3. Gap closure by diffing: `strace`/libdrm calls vs the amdgpu uAPI
   (`include/uapi/drm/amdgpu_drm.h`); implement any missing
   `AMDGPU_*`/DRM ioctls; userptr/HMM stay off and are documented as a
   known limitation.
4. `docs/linuxkpi-gaps.md` P6 C7 and a performance note in
   `docs/amdgpu-testing.md`.

Evidence: renderer string, fps numbers, PRIME results, no hangs; native
desktop regression green.

Exit: accelerated rendering with numbers; known limitations listed.

Risks: missing ioctls surfaced only by Mesa (syncobj/VM/INFO paths);
Mesa/libdrm version vs the 6.6 uAPI; missing test packages (fall back to a
small GBM/EGL harness).

---

### C8 — 6g: stability — reset, error injection, soak

Goal: the stack survives hangs, faults and memory pressure, and the
remaining rough edges are documented rather than discovered later.

Tasks:

1. Reset paths: force a GPU hang (bad CS / long-running IB) and observe
   mode1/mode2 reset via PSP/SMU; confirm recovery; exercise the P5 C1
   reduced PCI state save/restore and record whether it is sufficient
   under VFIO.
2. Error injection: bad CS, fence timeout, VM fault (invalid VA), repeated
   alloc/free; TTM under constrained RAM (`VFIO_MEM=2G`); BO eviction
   churn.
3. 8-hour soak with the desktop on amdgpu plus periodic 3D and copies;
   watch PMM/TTM counters, WARNs, IRQ counts, fence timeouts.
4. Leak audit across the suite; update `docs/amdgpu-testing.md` with a
   known-issues list; `docs/linuxkpi-gaps.md` gets the reset/error-path
   divergences.

Evidence: reset run logs, injection results, 8 h soak summary (counters,
zero WARN/hang), known-issues list.

Exit: no hangs under stress; recovery paths exercised and documented;
full regression green.

Risks: reset support depends on PSP/SMU and the reduced PCI state
save/restore; error paths may surface latent TTM/IRQ assumptions; time
budget for the 8 h run (schedule it as the chunk's tail).

---

### C9 — Closeout + P7 handoff

Goal: make Phase 6 reproducible for the next agent and hand off a clean P7
baseline.

Tasks:

1. `docs/linuxkpi.md`: amdgpu architecture section, the generated Kbuild
   workflow, the probe gate, porting notes learned here.
2. `docs/linuxkpi-gaps.md`: final P6 section (all divergences in one
   place, each with the imported file that needed it).
3. `docs/linuxkpi-progress.md`: Phase 6 exit matrix with serial excerpts
   and exact commands; open items; the 24 h soak status.
4. `docs/amdgpu-testing.md`: complete VFIO/VBIOS/firmware/monitor workflow,
   known issues, performance numbers.
5. Full regression on a clean `make -C kernel clean` build; symbol checks;
   fresh-clone import check (`scripts/linux-import.sh` + `make`).
6. Tag `p6-done`; P7 handoff notes (productization: default card0,
   firmware packaging, igt, docs).

Exit: docs complete; clean rebuild reproducible; P7 can start with no open
P6 item except the carried 24 h user soak.

---

## 3. Cross-chunk rules (restated + P6 specifics)

- Tests before implementation: new API chunks start with the suite in
  `kernel/src/tests/linuxkpi/linux/`; run at `-smp 4`.
- Never modify `kernel/linux/**`.  Every divergence is an overlay
  (`kernel/linuxkpi/include/...`), a shim (`kernel/linuxkpi/src/...`) or a
  native bridge (`kernel/src/linuxkpi/...`), recorded in
  `docs/linuxkpi-gaps.md` in the same change.  The Kbuild generator only
  reads the imported tree.
- Every chunk ends with the full regression: all `test_phase*` suites,
  userland `bin/test_kpi_drm` / `bin/test_kpi_dmabuf`, desktop boot
  (interactive when available), `make -C kernel` clean.
- Bootable at all times: before C3 the probe gate is 0; after C3 the
  gate-off path must remain regression-clean.  Each stage's revert path is
  written down (gate, `amdgpu.modeset=0`, `amdgpu.runpm=0`, generated list).
- PMM/TTM invariants use a warm-up before sampling the baseline and must
  not go backwards.
- Kernel tests run from `boot_tests.c` with bounded waits; hardware-gated
  tests use `KERNEL_CMDLINE=` module parameters (as `kpi_vfio_test` does).
- Evidence is serial excerpts + exact commands; headless canonical form:
  ```
  make run-linuxdrm SERIAL=file:build/logs/p6-cX.log \
       DISPLAY_OPT='-display none'
  ```
  and for VFIO:
  ```
  make run-vfio SERIAL=file:build/logs/p6-cX.log \
       DISPLAY_OPT='-display none' VFIO_EXTRA='-device edu' \
       KERNEL_CMDLINE='kpi_amdgpu=1 drm.debug=0x1ff amdgpu.runpm=0'
  ```
- Update `docs/linuxkpi-progress.md` after every working session (chunk,
  command, evidence, deviation, next step).
- Stage tags: `p6-start` (C0), `p6-6a` (C2), `p6-6b` (C3), `p6-6c` (C4),
  `p6-6d` (C5), `p6-6e` (C6), `p6-6f` (C7), `p6-6g` (C8), `p6-done` (C9).
  A tag moves only when the chunk's exit evidence is in the progress doc.

---

## 4. Mapping to the original Phase 6 exit criteria

| Original criterion | Covered by |
|---|---|
| Monitor driven by Raphael under AvoryOS | C3–C6 (bind, firmware, KMS); evidence at C6 |
| Unmodified Mesa radeonsi | C7 (plus C5's uAPI CS tests) |
| Stable multi-hour session | C8 (8 h soak) + C5–C7 regressions |
| Userptr/HMM still off and documented | C2/C5/C9 gap log; userptr disabled by config |
| Compile-gap avalanche managed | C1 generator + C2 wave process + gap log |
| Firmware name/version mismatches | C0 pin + C3 IP dump + C4 log-driven manifest |
| IRQ/IH under VFIO | C4 (IH delivery + teardown) |
| Output only at physical connectors | C6 note in `amdgpu-testing.md` |
| DML float paths | C1 flags + P0 FPU; exercised at C6 (DML) |

Note: the original P6 sketch's "Phase exit criteria" also inherit P5's
24 h soak; treat it as a user-run item carried forward, not a P6 blocker.

---

## 5. Decisions to confirm before C1

1. Import scope: whole `drivers/gpu/drm/amd` tree (~400 MB, simple) vs
   pruned ASIC headers (smaller, more generator work).  Recommended: whole
   tree now, prune later if the copy time hurts.
2. Probe gate: name (`kpi_amdgpu`), default (0 until C3), and location
   (LinuxKPI PCI registry) per §1.10.  Alternative: gate inside a small
   wrapper around `amdgpu_pci_probe` — more invasive; not recommended.
3. Kbuild generator language: Python 3 (host has it; `vfio-check-rom.sh`
   already depends on python3) vs awk.  Recommended: Python 3.
4. `i2c-algo-bit`: import the upstream file (recommended) vs self-author a
   bit-banger (~150 LOC).  Import keeps DDC behavior upstream-exact.
5. HWMON: weak stubs (recommended) vs a minimal hwmon device.  Weak stubs
   until a real consumer of the values appears.
6. Minor numbering: accept `card2`/`renderD129` for P6 and discover node
   names in tests (recommended) vs adding a build knob to drop vgem/vkms
   from P6 images.
7. DML flags: generated per-file `LINUX_CFLAGS` (recommended) vs a
   directory-wide rule in `kernel/GNUmakefile` (simpler but broader).
8. 6c boot params baseline: `amdgpu.runpm=0` + `amdgpu.modeset=0` only
   (recommended) vs disabling DPM (`amdgpu.dpm=0`) if the SMU handshake
   misbehaves.

---

## 6. Out of scope (documented deferrals)

- VCN video decode / VA-API, JPEG (firmware is staged only because
  early_init requires it).
- HDMI/DP audio (host HDA path, or snd_hda_intel port).
- DP MST, FreeSync/VRR, overlay planes, writeback.
- Runtime PM, system suspend/resume, ASPM, real clock/power gating.
- RAS/ECC, debugfs, tracepoints/perf counters.
- PRIME multi-GPU, dGPU support, AMD-Vi/IOMMU.
- Bare-metal boot without VFIO (ACPI/VFCT/ATPX), Secure Boot.
- HMM/userptr (`DRM_AMDGPU_USERPTR=n`), HSA/KFD, P2P.
- Real `hwmon`/`power_supply` implementations.

---

## Appendix A — command reference

Baseline / regression:

```sh
make -C kernel
make run-linuxdrm SERIAL=file:build/logs/p6-cX.log DISPLAY_OPT='-display none'
```

VFIO evidence (gate on from C3):

```sh
make run-vfio \
     KERNEL_CMDLINE='kpi_amdgpu=1 drm.debug=0x1ff amdgpu.runpm=0' \
     SERIAL=file:build/logs/p6-cX.log DISPLAY_OPT='-display none' \
     VFIO_EXTRA='-device edu'
```

VBIOS + firmware inputs:

```sh
sudo scripts/vfio-vbios.sh 0000:0e:00.0
scripts/vfio-check-rom.sh build/logs/p6-cX.log
LINUX_FIRMWARE_REF=<sha> scripts/linux-firmware-install.sh
```

Symbol / link checks:

```sh
nm kernel/bin-x86_64/kernel | grep ' T amdgpu_' | wc -l
nm kernel/bin-x86_64/kernel | grep -E ' (drm_buddy|drm_exec|drm_suballoc|i2c_bit_add_bus)'
make -C kernel clean && make -C kernel
```

Note: one QEMU per disk image; close an interactive session first or use a
scratch copy (`cp --reflink=auto disk.img build/disk-p6.img`).  Keep
`-vga none -device virtio-vga` when a virtual desktop is wanted next to
the passed GPU.

## Appendix B — `autoconf.h` delta for 6a

Add (absent = off; do not define `0`):

```c
/* Phase 6: amdgpu + the display helper and DRM helper libraries it selects. */
#define CONFIG_DRM_AMDGPU 1
#define CONFIG_DRM_AMD_DC 1
#define CONFIG_DRM_AMD_DC_FP 1
#define CONFIG_DRM_DISPLAY_HELPER 1
#define CONFIG_DRM_DISPLAY_DP_HELPER 1
#define CONFIG_DRM_DISPLAY_HDMI_HELPER 1
#define CONFIG_DRM_DISPLAY_HDCP_HELPER 1
#define CONFIG_DRM_BUDDY 1
#define CONFIG_DRM_EXEC 1
#define CONFIG_DRM_SUBALLOC_HELPER 1
#define CONFIG_I2C_ALGOBIT 1
```

Deliberately absent (with the reason): `HWMON` (weak stubs),
`POWER_SUPPLY` (stock stub), `INTERVAL_TREE` (local define), `PROC_FS`,
`PERF_EVENTS`, `COMPAT`, `VGA_SWITCHEROO`, `HSA_AMD`, `DRM_AMD_ACP`,
`DRM_AMDGPU_SI/CIK`, `HMM_MIRROR`, `DRM_AMD_SECURE_DISPLAY`,
`DRM_FBDEV_EMULATION`, `DRM_DP_AUX_BUS`, `DRM_DP_AUX_CHARDEV`,
`DRM_DP_CEC`, `ACPI`, `PM*`, `DEBUG_FS`, `TRACEPOINTS`, `JUMP_LABEL`.

## Appendix C — firmware manifest candidates (pin after C3's IP dump)

```
amdgpu/gc_10_3_6_ce.bin
amdgpu/gc_10_3_6_me.bin
amdgpu/gc_10_3_6_mec.bin
amdgpu/gc_10_3_6_mec2.bin
amdgpu/gc_10_3_6_pfp.bin
amdgpu/gc_10_3_6_rlc.bin
amdgpu/psp_13_0_5_toc.bin
amdgpu/psp_13_0_5_ta.bin
amdgpu/smu_13_0_5.bin
amdgpu/vcn_3_1_2.bin          # required by amdgpu_vcn_early_init()
amdgpu/dcn_3_1_4_dmcub.bin    # stage until C3 pins the DCN revision
amdgpu/dcn_3_1_5_dmcub.bin
amdgpu/dcn_3_1_6_dmcub.bin
amdgpu/sdma_6_0_*.bin         # name from C3/C4 log (IP version built)
amdgpu/ip_discovery.bin       # optional: amdgpu.discovery=2 fallback
```

Rules: add names in the same change as the boot that requests them; verify
each staged file against the host CRC in the firmware self-test; never ship
the whole linux-firmware tree.

## Appendix D — Kbuild subset evaluator specification

Grammar to support (everything else is an error, not a guess):

- comments (`#`), blank lines, backslash continuations;
- assignments `VAR :=`, `VAR =`, `VAR +=`, `VAR ?=` (no recursive
  expansion beyond one level needed by these trees);
- references `$(VAR)`, `$(addprefix P,L)`, `$(addsuffix S,L)`,
  `$(filter ...,L)`, `$(patsubst ...)` only if encountered;
- conditionals `ifdef`, `ifndef`, `ifeq`, `ifneq`, `else`, `endif`, with
  `CONFIG_*` values from `autoconf.h` (`y` present, absent = n);
- `include $(PATH)/Makefile` relative to the importing file or the tree
  root;
- object-list variables: `obj-$(CONFIG_X) +=`, `*-y +=`, `*-$(CONFIG_X) +=`;
- flags: `ccflags-y`, `subdir-ccflags-y`, `CFLAGS_<path>`,
  `CFLAGS_REMOVE_<path>`, `dml_ccflags`, `dml_rcflags`.

Output contract:

- `linux-obj-y := <sorted .c paths>` (only files that exist in the
  imported tree; a missing object is an error);
- for every source directory with non-empty `ccflags`, a target-specific
  rule appending the (filtered) `-I` list to `LINUX_CPPFLAGS`;
- for every source with `CFLAGS_<path>`/`CFLAGS_REMOVE_<path>`, a
  target-specific rule modifying `LINUX_CFLAGS` (DML gets
  `-mhard-float -msse -msse2`);
- keep `-Wframe-larger-than` out (CONFIG_FRAME_WARN is not defined).

Self-checks: object count is stable across runs; unresolved `$(...)` list
is empty; known-object presence (gfx_v10_0, psp_v13_0, smu_v13_0_5_ppt,
dcn31/dcn31_hwseq, dcn314/dcn314_fpu, dmub/src/*); report written next to
the Makefile fragment.  The generator never edits `kernel/linux/**`.
