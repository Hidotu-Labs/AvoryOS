# Phase 4 — chunked execution plan

Audience: the agent that picks up Phase 4.  Paste the project SHARED CONTEXT
with this file.  Written 2026-09-12 after auditing the P0–P3 tree at commit
`45ee1af`, QEMU 11.1.0, Linux pin **v6.6.156 (`8b73de7`)**.

Phase goal (original): bring up **TTM + drm_sched**, add the **minimal Linux
PCI API**, and validate with a real TTM-based in-tree driver on QEMU (bochs),
without regressing the native desktop or the Phase 3 DRM canaries.

---

## 0. Readiness audit — what P0–P3 actually shipped

Already true and reused by this phase:

- Import plumbing end to end: `scripts/linux/subset.txt` (copy list) →
  `scripts/linux-import.sh` → `kernel/linux/Makefile.files` →
  `kernel/GNUmakefile` (`linux-obj-y` → `LINUX_OBJ`, imported-CFLAGS rule).
- DRM core + KMS helpers + `vgem`/`vkms`/`simpledrm` linked; `/dev/dri/card1`
  (vkms) and `/dev/dri/renderD128` (vgem) exist; native `ascentdrm` keeps
  `/dev/dri/card0`.
- dma-buf/fence/resv/sync_file imported; Linux `struct file`/fd/VFS/poll/mmap
  bridges; PRIME-style fd passing verified by `bin/test_kpi_dmabuf`.
- Dynamic class sysfs (`/sys/class/drm/card1`), devres core, platform bus,
  shmem GEM backing, gated initcalls in a kthread.
- Kernel test harness: `kernel/linuxkpi/src/boot_tests.c` runs
  `kernel/src/tests/linuxkpi/linux/test_phase*.c` from a `kpi/tests` kthread
  after initcalls; tests may map a scratch page in the user range of the PML4
  for `copy_from_user()` ioctl arguments (see `test_phase3_drm_modeset.c`).

Open items that Phase 4 **must** close first (C0):

- P1 exit: interactive desktop boot on `make run` was never recorded.
- P2 exit: the 10-minute soak re-run after the warm-up-baseline fix is pending;
  it must show `delta == 0` (previous runs: −13 and −11 pages, both one-time
  thread-stack/heap growth, not per-iteration leaks).
- P3 exit: met as documented, but `modetest` was never attempted and generic
  kobject attributes / `bin_attributes` remain inert (tracked gaps, not
  blockers).

Not yet present (this phase builds it): TTM, drm_sched, Linux PCI API,
`set_pages_*`, bochs, `run-linuxdrm` target.

---

## 1. Corrections to the Phase 4 sketch (read before coding)

1. **mgag200 is not available in QEMU 11.1.**  `qemu-system-x86_64 -device
   help` has no `mgag200`; it has `bochs-display`, `virtio-vga`, `qxl-vga`,
   `ati-vga`, `vmware-svga`, `VGA` (std).  The TTM canary is **bochs**.
   (If bochs misbehaves, `qxl` is the only other TTM driver with a QEMU model;
   treat as fallback only.)
2. **bochs is `drivers/gpu/drm/tiny/bochs.c` in 6.6, not
   `drivers/gpu/drm/bochs/`**, and it is already copied by the P3 subset
   (`drivers/gpu/drm/tiny`).  It is genuinely TTM-backed: it includes
   `<drm/drm_gem_vram_helper.h>` and calls
   `drmm_vram_helper_init(dev, fb_base, fb_size)` → `ttm_range_man_init` over
   BAR0.  It handles both the I/O-port path (`-vga std`) and the MMIO path
   (`bochs-display`).
3. **QEMU recipe.**  The default `QEMUFLAGS` is
   `-m 4G -vga none -device virtio-vga,xres=1280,yres=800 -display gtk`.
   Add `-device bochs-display` as a *secondary* display: it is not
   VGA-compatible, coexists with virtio-vga, and does not touch the Limine/GOP
   framebuffer the native path owns.  Do not replace virtio-vga.
4. **TTM 6.6 file list** (from `drivers/gpu/drm/ttm/Makefile`): `ttm_tt.c`,
   `ttm_bo.c`, `ttm_bo_util.c`, `ttm_bo_vm.c`, `ttm_module.c`,
   `ttm_execbuf_util.c`, `ttm_range_manager.c`, `ttm_resource.c`,
   `ttm_pool.c`, `ttm_device.c`, `ttm_sys_manager.c`.  There is no
   `ttm_memory.c`/`ttm_bo_manager.c` in 6.6.  Scheduler (`gpu-sched-y`):
   `sched_main.c`, `sched_fence.c`, `sched_entity.c`.
5. **Config names (verified against 6.6 Kconfig):** `CONFIG_DRM_TTM`,
   `CONFIG_DRM_TTM_HELPER`, `CONFIG_DRM_VRAM_HELPER` (not
   `DRM_GEM_VRAM_HELPER`), `CONFIG_DRM_SCHED`, `CONFIG_DRM_BOCHS`.  bochs
   selects KMS_HELPER + VRAM_HELPER + TTM + TTM_HELPER.
6. **Stock `include/linux/pci.h` already compiles here** (drm_pci.c built with
   it in P3).  Do **not** shadow it up front; implement its externs and only
   overlay if a specific construct proves impossible.  `module_pci_driver()` is
   in stock pci.h and expands through `module_driver()`, which the existing
   `linuxkpi/include/linux/device.h` overlay already provides.
7. **EDU is the PCI test device.**  QEMU 11.1 has `-device edu` (`1234:11e8`,
   BAR0 = 1 MB MMIO, MSI-capable).  No native driver claims it, so C4/C5 tests
   can bind/unbind a LinuxKPI test driver there without disturbing nvme,
   rtl8139, virtio, or the native GPU.
8. **virtio-gpu in 6.6 is GEM-shmem + dma-buf, not TTM** — the optional virtio
   path does not validate TTM.  It only proves PRIME + a second accelerated
   Linux card.  Keep it as a go/no-go stretch (C6).
9. **amdgpu compile signal is a bounded spike (C7), not a Phase 4 gate.**
   Its real deliverable is the Kbuild file-list generator that P6a needs.

---

## 2. Chunk status board

| Chunk | Title | Size | Depends on | Exit gate |
|-------|-------|------|------------|-----------|
| C0 | Baseline closeout (P1/P2/P3 still green) | S | — | desktop boot + soak delta 0 |
| C1 | Import/build TTM + drm_sched (compile-only) | M | C0 | kernel links, boot clean |
| C2 | TTM self-tests | L | C1 | TTM suite green, PMM stable |
| C3 | drm_sched self-tests | M | C1 | sched suite green |
| C4 | Minimal Linux PCI API + EDU lifecycle test | L | C1 | test driver probe/remove on edu |
| C5 | bochs canary: bind, modeset, userland | XL | C2–C4 | bochs modeset + userland pass |
| C6 | (optional) Linux virtio-gpu path | XL | C5 | conditional per original plan |
| C7 | (optional) amdgpu spike + Kbuild generator | M | C5 | generator + gap census |

Sizes are relative: C0 ≈ a session, C1 ≈ one session, C2/C3/C4 ≈ 1–2 sessions
each, C5 ≈ several.  Do chunks in order; every chunk ends with the full boot
regression (all `test_phase*` suites + desktop boot).

---

### C0 — Baseline closeout (gate; do not skip)

Goal: prove P1/P2/P3 exit criteria are still green and freeze the evidence
baseline before any Phase 4 code lands.

Tasks:
1. `make -C kernel` → clean; `make` → ISO up to date; `make run` interactive →
   reach the desktop/login (P1 + P3 regression).  Record the tail of the
   serial output.
2. Phase 2 soak: boot, log in, run `bin/test_kpi_dmabuf 600` (600 s at
   `-smp 4`).  Expected after the warm-up fix:
   `[SOAK] iterations=... errors=0 closes=...` and
   `[SOAK] PMM free pages baseline=... final=... delta=0`.
   If `delta != 0`, stop and diagnose (worker stacks are created before the
   baseline now) before continuing to C1.
3. Re-run the P3 checks: `nm kernel/bin-x86_64/kernel | grep ' T drm_'` shows
   only imported symbols; `/dev/dri` lists `card0`, `card1`, `renderD128`;
   `bin/test_kpi_drm` → `=== ALL TESTS PASSED ===`.
4. Update `docs/linuxkpi-progress.md`: flip the P1 desktop and P2 soak boxes,
   date them, note the baseline commit/tag.  Update `docs/linuxkpi-gaps.md` if
   the soak exposes anything new.

Evidence: serial excerpts + exact commands in the progress doc.

Exit: all P0–P3 kernel suites green at `-smp 4`; desktop boots; soak delta 0.

---

### C1 — Import/build TTM + drm_sched (compile-only)

Goal: link the upstream TTM and GPU scheduler objects plus the two DRM TTM GEM
helpers, with no runtime use yet.  This is intentionally behavior-free; it
surfaces the first wave of missing LinuxKPI symbols in a controlled way.

Tasks:
1. `scripts/linux/subset.txt`: add
   ```
   drivers/gpu/drm/ttm
   drivers/gpu/drm/scheduler
   ```
2. `scripts/linux/files.txt`: add, under a `# Phase 4` banner:
   ```
   drivers/gpu/drm/ttm/ttm_tt.c
   drivers/gpu/drm/ttm/ttm_bo.c
   drivers/gpu/drm/ttm/ttm_bo_util.c
   drivers/gpu/drm/ttm/ttm_bo_vm.c
   drivers/gpu/drm/ttm/ttm_module.c
   drivers/gpu/drm/ttm/ttm_execbuf_util.c
   drivers/gpu/drm/ttm/ttm_range_manager.c
   drivers/gpu/drm/ttm/ttm_resource.c
   drivers/gpu/drm/ttm/ttm_pool.c
   drivers/gpu/drm/ttm/ttm_device.c
   drivers/gpu/drm/ttm/ttm_sys_manager.c
   drivers/gpu/drm/scheduler/sched_main.c
   drivers/gpu/drm/scheduler/sched_fence.c
   drivers/gpu/drm/scheduler/sched_entity.c
   drivers/gpu/drm/drm_gem_ttm_helper.c
   drivers/gpu/drm/drm_gem_vram_helper.c
   ```
   (`tiny/bochs.c` waits for C5, after PCI lands; the vram helper has no PCI
   dependency.)
3. `kernel/linuxkpi/include/generated/autoconf.h`: add
   ```
   /* Phase 4: TTM + scheduler. */
   #define CONFIG_DRM_TTM 1
   #define CONFIG_DRM_TTM_HELPER 1
   #define CONFIG_DRM_VRAM_HELPER 1
   #define CONFIG_DRM_SCHED 1
   ```
4. Run `scripts/linux-import.sh`; confirm the pin still reports v6.6.156 and
   `git status` shows no changes under `kernel/linux/` (it is gitignored).
5. Build and fix the first wave.  Expected gaps (verify each against the
   imported source before stubbing):
   - `set_pages_wb()`, `set_pages_array_wc()`, `set_pages_array_uc()`
     (`asm/set_memory.h`, used by `ttm_pool.c`): add one-time-WARN no-op stubs
     (new `kernel/linuxkpi/src/memtype_stubs.c` or `link_stubs.c`).  Correct
     implementation needs PAT programming; document in gaps.md.
   - `synchronize_shrinkers()` already exists in `shrinker.c`.
   - `dma_resv_*`/`dma_fence_*`/`dma_alloc_attrs`/`sg_*` exist from P2.
   - `iosys_map` is upstream header-only; check it compiles.
   - `kmap_local_page` exists in the highmem overlay.
   - TTM debugfs paths are compiled out (`CONFIG_DEBUG_FS` unset) — no action.
   - `ttm_module.c` carries `module_init(ttm_init)`; verify `ttm_init` runs in
     the initcall kthread and logs cleanly.
6. Every new stub/divergence goes into `docs/linuxkpi-gaps.md` in the same
   change.

Tests: none new (compile-only), but the whole P0–P3 boot suite must stay green
on the canary target and on plain `make run`.

Evidence: `make -C kernel` clean; `nm kernel/bin-x86_64/kernel | grep -E ' T
(ttm_|drm_sched_|drm_gem_ttm_|drm_gem_vram_)'` non-empty; boot log shows the
usual suites + `ttm_init` with no WARN.

Exit: kernel links with the 16 new objects; boot to login unchanged; no TTM or
scheduler WARN during boot.

Risks: `ttm_pool.c` references `set_pages_*` unconditionally — the stubs are
link-required, not optional.  `gpu_scheduler_trace.h` is included under
`CONFIG_TRACEPOINTS` (unset) so tracepoints compile out.

---

### C2 — TTM self-tests

Goal: prove a `ttm_device` can allocate, pin, map, move/evict and free BOs on
AvoryOS before any real driver binds.

Test first: write `kernel/src/tests/linuxkpi/linux/test_phase4_ttm.c` against
the upstream API, then fix the implementation until green.  Model the minimal
harness on `drivers/gpu/drm/ttm/tests/ttm_kunit_helpers.c` (it defines a
`struct ttm_device_funcs ttm_dev_funcs` and calls `ttm_device_init`) — port the
*shape*, not KUnit.

Coverage:
1. `ttm_device_init()`/`ttm_device_fini()` with a fake `ttm_device_funcs`
   (no-op move/evict/delete; `ttm_tt_create` per upstream helper).
2. SYSTEM BO: `ttm_bo_init_reserved()` + `ttm_bo_validate()` (SYSTEM),
   reserve/unreserve, `ttm_bo_kmap()`/`ttm_bo_kunmap()`, `ttm_bo_vmap()`/
   `ttm_bo_vunmap()` (iosys_map), CPU write/read roundtrip, pin/unpin,
   `ttm_bo_wait` on a signaled `dma_resv`.
3. VRAM: `ttm_range_man_init(bdev, TTM_PL_VRAM, false, pages)` over a synthetic
   32 MB range (no real backing needed for offset allocation),
   `ttm_bo_mem_space` start/offset sanity, `ttm_bo_move_memcpy`
   SYSTEM↔VRAM, eviction: fill the manager, confirm unpinned BOs evict and
   pinned ones survive.
4. GEM helper smoke: `drm_gem_vram_create()` on a fake `drm_device`?  If that
   drags in too much of the DRM device model, do it in C5 via bochs instead and
   note the decision.
5. 1k-iteration allocate/map/pin/unpin/free loop with a PMM free-page baseline
   sampled **after** a warm-up loop (P2 lesson: first-touch slab/heap/thread
   growth is not a leak).
6. Shrinker path: `ttm_pool` registers `drm-ttm_pool`; `register_shrinker`/
   `synchronize_shrinkers` must not crash (scan is inert by design).

Wire the suite into `boot_tests.c` (extern + call after the Phase 3 suites).

Evidence: `[  OK  ]` lines for each numbered item, `PMM delta 0`, no WARN.

Exit: TTM suite green at `-smp 4`; rest of the suites unchanged; gaps doc
updated with any TTM divergence (e.g., `set_pages_*`).

Risks: TTM's reserve/eviction paths are the first real user of the ww_mutex
shim at depth (P3 made `-EALREADY` correct; multi-lock wound/wait is still
limited — if eviction loops stall, fix the ww_mutex wound path here and log
it).  `ttm_bo_vm.c` needs `vmf_insert_pfn_prot`/`unmap_mapping_range`, both
present from P2 but exercised only by userspace mmap until C5.

---

### C3 — drm_sched self-tests

Goal: prove the GPU scheduler runs jobs, signals fences and fires the timeout
path on AvoryOS.

Test first: `kernel/src/tests/linuxkpi/linux/test_phase4_sched.c`.

Coverage:
1. `drm_sched_init()` with fake `drm_sched_backend_ops`:
   `run_job` schedules a work item; the work signals
   `drm_sched_fence_create()`'s finished fence (via
   `drm_sched_fence_scheduled()` + `dma_fence_signal()`); `free_job` drops the
   job; `timedout_job` records.
2. Entities: create 2–3 `drm_sched_entity`s (kernel context), push N jobs per
   entity, wait each `s_fence->finished` with `dma_fence_wait_timeout`, verify
   per-entity ordering and completion.
3. Timeout: a job whose `run_job` never signals; set a short
   `sched->timeout` (e.g. 100 ms); confirm `work_tdr` fires and `timedout_job`
   runs; then stop/start cleanly (`drm_sched_stop`/`drm_sched_start`) and
   finish.
4. Stress: ~100 jobs across entities with workqueue-driven completion; PMM
   baseline after warm-up, delta 0; `drm_sched_fini` frees the kthread and
   fences (no leaked thread stacks).

Wire into `boot_tests.c`.

Exit: suite green at `-smp 4`; no WARN; PMM stable.

Risks: `kthread_park`/`kthread_unpark` and `drm_sched_rq` locking are the
sharp edges; scheduler uses `completion`/waitqueues already proven in P1.

---

### C4 — Minimal Linux PCI API (expanded properly in Phase 5)

Goal: enough of `include/linux/pci.h` for bochs (and a lifecycle test on EDU)
to bind, map BARs and use devres, without touching native PCI behavior.

Test first: `kernel/src/tests/linuxkpi/linux/test_phase4_pci.c` that (a)
exercises the accessors on the already-enumerated EDU and (b) registers,
probes and removes a tiny test driver.

Design (follow the existing native/Linux split):
1. Native side `kernel/src/linuxkpi/native_pci.c` +
   `kernel/linuxkpi/include/linuxkpi/native_pci.h`:
   - export iteration over the native `struct pci_device` table
     (`pci_get_device_count()/pci_get_device()` already exist), config 8/16/32
     access (native has 16/32; add `pci_config_read8/write8`), and
     `pci_bar_phys()`.
   - store `bar_size[6]` on the native `struct pci_device` during enumeration
     (`pci_check_function` already probes them) and export
     `pci_bar_size()`/`pci_bar_phys()` instead of re-probing BARs later.
   - enable/bus-mastering/decode already exist (`pci_set_bus_mastering`).
2. Linux API side `kernel/linuxkpi/src/pci.c` implementing stock
   `linux/pci.h` externs:
   - wrapper lifecycle: `linuxkpi_pci_scan()` creates one `struct pci_dev` per
     native device (`kzalloc`; `dev.kobj`, `dev.init_name` = BDF,
     `dev.bus = &pci_bus_type`, `dev.release`; resources decoded from BARs into
     `struct resource` with `IORESOURCE_MEM|IO|64BIT`).  Call it from
     `linuxkpi_run_initcalls()`'s caller path or a level-0 initcall **after
     native `pci_init()`** and before driver initcalls.
   - `pci_register_driver`/`pci_unregister_driver`: registry + direct matching
     (vendor/device/subvendor/subdevice/class/class_mask) against existing
     wrappers; call `.probe`; `.remove` on unregister.  Direct implementation,
     not `driver_register()` (no async/deferred probe yet) — document.
   - `pci_enable_device`, `pcim_enable_device` (devres action that disables),
     `pci_disable_device`, `pci_set_master`, `pci_request_region(s)`,
     `pci_release_region(s)`, `pci_resource_start/end/len/flags`,
     `pci_iomap`/`pci_iomap_range`/`pci_iounmap` (over linuxkpi `ioremap`),
     `pci_find_capability`, `pci_find_ext_capability` (native ECAM handles
     4 KB offsets), `pci_is_pcie`, `pcie_capability_read_*`,
     `pci_get_domain_bus_and_slot`, `pci_set_drvdata`/`pci_get_drvdata`,
     `pci_name`, `pci_set_dma_mask`/`pci_set_consistent_dma_mask`,
     `pci_map_rom`/`pci_unmap_rom` (basic; VFCT/VBIOS hash test is P5).
   - `request_region`/`release_region` (I/O ports) and
     `request_mem_region`/`release_mem_region` bookkeeping registries (native
     I/O is raw `in/out`, so this is a conflict registry); document.
   - Remove/keep weak: `drm_aperture_remove_conflicting_pci_framebuffers()` is
     not built in P3; add a weak returning-0 stub in `drm_compat.c` now (bochs
     calls it in probe) and document.
3. Tests on EDU (`-device edu`, `1234:11e8`):
   - wrapper exists; vendor/device/class correct; BAR0 is MEM and 1 MB;
     `pci_resource_start/len/flags` consistent with native resources.
   - `pci_enable_device` + `pci_set_master`, `pci_request_region(0)`,
     `pci_iomap` and read the identification register (log the value; assert
     the vendor/device and that the mapping is non-NULL; exact register
     constant can be added after the first run).
   - a tiny `struct pci_driver` with an id table matching EDU: register →
     probe called; `pci_get_drvdata` roundtrip; unregister → remove called.
   - regression: the wrapper scan must not disturb nvme / rtl8139 / virtio,
     which stay on native drivers.

Canary target: extend the QEMU flags with `-device edu` for this chunk (EDU
only; no bochs yet).

Exit: PCI test green at `-smp 4` on the canary target; plain `make run` still
green (EDU absent → test logs SKIP, must not fail).

Risks: BAR re-probing must never run on a live device if native already
recorded sizes — hence `bar_size[6]` at enumeration.  The PCI wrapper `struct
device` needs only enough kobject wiring for DRM sysfs (`kobj.parent` is what
P3's `create_compat_control_link` consumed); watch for a repeat of the vkms
`-EINVAL` if `sysfs_create_link` is handed a parent-less kobject.

---

### C5 — TTM canary: bochs binds and modesets

Goal: the first real TTM driver (upstream bochs) binds under LinuxKPI, does a
full atomic modeset on QEMU, and the result is visible from userland.

Tasks:
1. `files.txt` += `drivers/gpu/drm/tiny/bochs.c`; `autoconf.h` +=
   `CONFIG_DRM_BOCHS 1`.
2. New make target `run-linuxdrm` (top-level GNUmakefile), modeled on `run`:
   - adds `-device bochs-display` (secondary display; keep virtio-vga) and
     `-device edu`.
   - overridable `SERIAL ?= stdio` and `DISPLAY_OPT ?= -display gtk` so a
     headless capture is:
     `make run-linuxdrm SERIAL=file:build/logs/p4-c5.log DISPLAY_OPT=-display none`.
   - keep `make run` untouched.
3. Bring-up fixes expected (log every one in gaps.md):
   - `drm_aperture_remove_conflicting_pci_framebuffers` weak stub (C4).
   - `<video/vga.h>` from the imported include tree: verify it resolves and
     that only the `VGA_*` constants are needed.
   - `pcim_enable_device` devres path; `pci_request_region` on BAR0+BAR2;
     `ioremap` of both (BAR2 MMIO path is what `bochs-display` uses).
   - `drmm_vram_helper_init` over BAR0 → `ttm_range_man_init`; watch TTM
     memory-type accounting in `ttm_bo_validate` (WC/UC stubs are inert; VRAM
     BOs are CPU-accessible through the BAR mapping, so this works).
   - `drm_fbdev_generic_setup()` is a no-op inline (`CONFIG_DRM_FBDEV_EMULATION`
     unset) — no fbdev work needed.
   - `drm_module_pci_driver_if_modeset` → `module_pci_driver` →
     `module_driver` (already in the device.h overlay).
   - `create_compat_control_link` sysfs behavior when `dev->parent` is the PCI
     wrapper device (see C4 risk).
4. Kernel test `kernel/src/tests/linuxkpi/linux/test_phase4_bochs.c`:
   - discover the bochs DRM device by name (do not hard-code minor numbers;
     expected order: vgem 0, vkms 1, bochs 2).
   - SET_CLIENT_CAP(ATOMIC); GETRESOURCES/GETCONNECTOR/GETPLANERESOURCES;
     CREATE_DUMB (e.g. 64x64 XRGB8888) → ADDFB2 → MODE_ATOMIC enable; verify
     the VBE XRES/YRES registers through the BAR2 mapping match the committed
     mode; write a pixel pattern into the BO (kmap), read it back through the
     BAR0 mapping at the BO's VRAM offset (`drm_gem_vram_offset`) and compare;
     disable commit; free.
   - 128-iteration VRAM BO create/map/free loop with PMM invariant and VRAM
     manager free-space invariant.
5. Userland `userland/test_kpi_bochs.c` (+ the four standard GNUmakefile
   wiring points: `disk.img` dep line ~627, debugfs `rm/write` block ~805, the
   `set_inode_field ... 0100755` list ~1138, and a build rule next to
   `test_kpi_drm.elf` ~1453):
   - find the card whose `DRM_IOCTL_VERSION.name` is `bochs`; CREATE_DUMB,
     MAP_DUMB, mmap, page write/read, ADDFB2, atomic enable, poll for a flip
     event if bochs emits one (verify; if not, skip and say so), disable,
     clean up; `=== ALL TESTS PASSED ===`.
   - do not disturb the existing `test_kpi_drm` (vkms) flow.
6. `modetest` best effort: timebox an attempt to build Alpine's libdrm
   `modetest` (new `scripts/build-modetest.sh` following the musl pattern of
   the other userland builds) and run it against `/dev/dri/card2`.  If it does
   not converge, record the exact blocker and keep the hand-written test as the
   evidence (same fallback P3 used).

Evidence: `[drm] Initialized bochs 1.0.0 ...`; `[  OK  ]` lines for the kernel
suite; `=== ALL TESTS PASSED ===` from `bin/test_kpi_bochs`; `bin/test_kpi_drm`
still passes; `make run` desktop still boots.

Exit: bochs card registers and modesets; kernel + userland bochs suites green
with no WARN from DRM core; vkms/vgem suites unchanged; native desktop
regression-free; modetest attempted (pass or documented blocker).

Risks: sysfs link on the PCI-backed DRM device (P3 precedent); BAR0 VRAM
mapping size vs TTM range manager; clock/scanout reads at the wrong offset;
`-display gtk` opening a second window (expected, harmless; headless evidence
uses `DISPLAY_OPT=-display none`).

---

### C6 — (optional, go/no-go) Linux virtio-gpu path

Do only if C5 is green and there is budget; it is a separate build flag and is
**not** a Phase 4 gate.  Value: PRIME + accelerated Mesa virgl on a Linux stack
and a second `renderD128` consumer; cost ≈ another C4+C5-sized effort.

Sketch: import `drivers/virtio` + `drivers/gpu/drm/virtio`; build
`LINUX_VIRTIO_GPU=1` where the native virtio-gpu driver does not claim
`1af4:1050`; verify `virtio_gpu` probes `cardN`, GETPARAM, dumb buffers,
PRIME export/import (`test_kpi_dmabuf`-style), then `glxgears`/`kmscube` with
virgl on the host.  Note again: 6.6 virtio-gpu is GEM-shmem, not TTM, so this
does not replace C2's TTM evidence.  Decision: defer unless P5/PRIME needs it.

---

### C7 — (optional) amdgpu compile spike + Kbuild file-list generator

Bounded spike; not a Phase 4 gate.  Deliverable that P6a actually needs:
`scripts/linux/gen-file-lists.py` that parses a Kbuild `Makefile`'s
`<module>-y`/`<module>-$(CONFIG_X)` composition (amdgpu's `Makefile` has
`amdgpu-y` spread across many lines and `foo-$(CONFIG_...)` entries) and emits
a `files.txt` fragment.  Then add a small amdgpu subset and compile
`amdgpu_drv.c` only, recording the first ~50 missing-API errors by header into
`docs/linuxkpi-gaps.md`.  Stop there; runtime bring-up is P6a.

---

## 3. Cross-chunk rules (project working rules, restated)

- Tests before implementation: new API chunks start with the suite in
  `kernel/src/tests/linuxkpi/linux/`; run under `-smp 4`.
- Never modify `kernel/linux/**`; every divergence is an overlay
  (`kernel/linuxkpi/include/...`) or glue (`kernel/src/linuxkpi/...`,
  `kernel/linuxkpi/src/...`) change, recorded in `docs/linuxkpi-gaps.md` in the
  same change.
- Every chunk ends with the full regression: kernel suite + userland
  `test_kpi_dmabuf` + `test_kpi_drm` + interactive desktop boot.  A chunk is
  not done until the previous chunks' evidence is still green.
- PMM free-page invariants must use a warm-up before sampling the baseline
  (P2 soak lesson) and must not go backwards.
- Kernel tests run in a kthread via `boot_tests.c` with bounded waits; ioctl
  argument pages are mapped into the PML4 user range as in
  `test_phase3_drm_modeset.c`.
- Update `docs/linuxkpi-progress.md` after every working session (chunk,
  command, evidence, deviation, next step).
- Headless evidence harness (canonical form):
  ```
  qemu-system-x86_64 -M q35 -m 4G -cpu host -enable-kvm -smp 4 \
    -drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \
    -cdrom avoryos-x86_64.iso \
    -drive file=disk.img,format=raw,if=none,id=nvme0 -device nvme,serial=avoryos0,drive=nvme0 \
    -vga none -device virtio-vga -device bochs-display -device edu \
    -display none -serial file:build/logs/p4-cX.log
  ```
  (`-device bochs-display` only meaningful from C5; `-device edu` from C4.)

## 4. Mapping to the original Phase 4 exit criteria

| Original criterion | Covered by |
|---|---|
| TTM unit tests green (alloc/pin/map/evict/free, no leaks over 1k) | C2 |
| TTM canary binds, modesets; modetest shows CRTC/connector | C5 (+ fallback) |
| render-capable canary exposes renderD128 | already true (vgem, P3); bochs is not render-capable |
| kmscube/glxgears with kms_swrast on the Linux card | C5 best effort; record success or documented blocker |
| drm_sched unit tests green | C3 |
| Native desktop regression-free | C0 + every chunk |
| Optional virtio: virgl accelerated + PRIME | C6, conditional |
| Early amdgpu compile signal | C7, optional |

## 5. Decisions to confirm before C1

1. Canary target name `run-linuxdrm` and flags (`bochs-display` + `edu` only in
   that target; `make run` untouched) — recommended.
2. BAR sizes: store `bar_size[6]` on native `struct pci_device` at enumeration
   and export accessors (recommended) vs. re-probe in the bridge.
3. `modetest` attempt in C5, timeboxed, with the hand-written suite as the
   fallback evidence (recommended) vs. skip and document now.
4. C6/C7 only after C5; both are explicitly skippable without failing Phase 4.
