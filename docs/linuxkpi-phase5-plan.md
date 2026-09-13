# Phase 5 — chunked execution plan

Audience: the agent that picks up Phase 5.  Paste the project SHARED CONTEXT
with this file.  Written 2026-09-12 after auditing P0–P4 at commit `4e7dd65`
plus the C5 working tree (bochs canary), QEMU 11.1.0, host GPU
`0000:0e:00.0` = `1002:164e` rev c6, Linux pin **v6.6.156 (`8b73de7`)**.

Phase goal (original): complete the LinuxKPI surface a real PCIe GPU needs —
full Linux PCI API, MSI/MSI-X with threaded handlers, ACPI table access, an
I2C core for DDC/EDID, dynamic sysfs/devres/device completion, PM/clk/reset
stubs, IRQs enabled during syscalls — and harden the VFIO loop until the
Raphael iGPU binds, maps BARs, allocates MSI-X, and serves a Linux
`request_irq` handler, with the VBIOS hash matching the host ROM.

**Status 2026-09-13: C0–C7 verified (headless VFIO validation green: bind,
BARs, MSI-X, request_irq, VBIOS CRC MATCH; the 1 h soak was waived and the
24 h protocol is pending/user-run).  C8 (phase closeout) in progress.**

---

## 0. Readiness audit — what P0–P4 actually shipped

Already true and reused by this phase:

- Import pipeline, config (`kernel/linuxkpi/include/generated/autoconf.h`),
  initcalls, kernel FPU, firmware loader, `run-vfio` (P0).
- Minimal Linux PCI API (Phase 4 C4): `kernel/linuxkpi/src/pci.c` +
  `kernel/src/linuxkpi/native_pci.c` / `native_pci.h`.  One `struct pci_dev`
  wrapper per native device, built by `linuxkpi_pci_scan()` before initcalls;
  config 8/16/32, standard + extended caps, PCIe cap R/W, region conflict
  registry, `pci_iomap`, direct driver registry with synchronous probe/remove,
  BARs decoded into `struct resource`.  The wrapper already populates
  `bus`, `devfn`, `revision`, `pcie_cap`, `msi_cap`, `msix_cap`,
  `pcie_flags_reg`, `irq`, `pin` in `kpi_pci_dev_new()`.
- Native IRQ machinery this phase bridges to:
  `interrupt_vector_alloc()/free()` over vectors `0x60..0xDF`
  (`kernel/src/cpu/isr.c`); `isr_t` handlers receive `struct registers *`
  whose `int_no` is the vector; `pci_irq_request_modes_routed()`
  (`kernel/src/drivers/pci/pci_irq.c`) allocates CPU vectors, programs
  MSI/MSI-X/INTx and routes per-vector destination APICs (up to 32 MSI-X
  vectors, `PCI_IRQ_MAX_VECTORS`).
- IRQ context tracking: `linuxkpi_irq_enter/exit` hooks are already called
  from the native ISR (`in_interrupt()`/`in_hardirq()` per-thread depth).
- Device model: `struct device` overlay with kobject, devres lists/groups,
  class/device creation through the native sysfs bridge
  (`asc_sysfs_class_dir/device_dir/attr_file`), device groups wired for class
  dirs (P3 chunk 5).  Native PCI sysfs already maintains
  `/sys/bus/pci/devices/<bdf>` and `/sys/devices/pci0000:00/<bdf>` attribute
  dirs (`kernel/src/fs/sysfs_pci.c`).
- DRM core + TTM + drm_sched + vgem/vkms + bochs.  C5 in the working tree:
  bochs binds as `/dev/dri/card2`, does an atomic modeset, the BAR0 readback
  matches the VA pattern, and the 2×128 VRAM loop is PMM/VRAM stable.
- Kernel test harness (`kernel/linuxkpi/src/boot_tests.c`), userland
  `bin/test_kpi_dmabuf`, `bin/test_kpi_drm`, headless serial capture.
- Host VFIO facts: `0000:0e:00.0` = `1002:164e` rev c6 (Raphael), IOMMU
  group 24, bound to `vfio-pci`; `build/vfio/vbios.rom` extraction needs
  root; `make run-vfio` passes `rombar=1` plus `romfile=` when the file
  exists.

Open items that C0 must close before any Phase 5 code lands:

- Phase 4 C5: commit the bochs canary and its kernel/userland evidence;
  the `page_to_phys()` fix (do not walk `compound_head()` for non-compound
  high-order pages; TTM's pool relies on it) and the upstream `__GFP_COMP`
  page test semantics must be in the baseline.
- P2 exit: clean 600 s soak (`bin/test_kpi_dmabuf 600`, `-smp 4`) with
  `closes == iterations` and delta 0/±1 one-time pages, run without the
  desktop.
- P4 C0 user-run checks that were still open (interactive `make run`,
  `/dev/dri`, `bin/test_kpi_drm`).
- P4 C6/C7 (optional virtio-gpu, amdgpu compile spike) explicitly not
  required; record the skip decision.

Not yet present (this phase builds it): IRQ core and MSI bridging, proper
`pci_map_rom`, PCI state save/restore, PM/clk/reset stubs, i2c core, real
sysfs group/bin attributes, `devm_request_irq`, IRQs-on syscalls, and the
VFIO bind/ROM test driver.

---

## 1. Corrections to the Phase 5 sketch (read before coding)

1. **amdgpu 6.6 needs exactly one MSI/MSI-X vector.**
   `amdgpu_irq_init()` calls `pci_msix_vec_count()`, then
   `pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_MSIX)`,
   `pci_irq_vector(pdev, 0)` and `request_irq(..., IRQF_SHARED, ...)`.
   The IRQ chunk is not a multi-vector project; the generic API should work
   for N, but the acceptance path is 1 vector + INTx fallback.  Native MSI-X
   already supports up to 32 routed vectors.
2. **`acpi_get_vbios_image()` does not exist in 6.6.**  VFCT is parsed in
   `amdgpu_bios.c` via `acpi_get_table("VFCT", 1, &hdr)` inside
   `#ifdef CONFIG_ACPI`, and `amdgpu_acpi.o` is only linked when
   `CONFIG_ACPI` (`drivers/gpu/drm/amd/amdgpu/Makefile:284`).  Under VFIO
   the VBIOS arrives through the ROM BAR (`romfile=`), not VFCT.
   **Recommendation: keep `CONFIG_ACPI=n` through Phase 6.**  P5's ACPI
   deliverable is an audit plus `acpi_get_table`/`acpi_put_table`
   groundwork over native `acpi_find_table()`, tested by reading FADT/APIC
   from a kernel test.  Enabling ACPI (and with it `amdgpu_acpi.c`/ATPX) is a
   Phase 8/bare-metal item.
3. **`pci_map_rom()` as shipped reports size 0** (`docs/linuxkpi-gaps.md`,
   C4).  `amdgpu_read_bios()` calls `pci_map_rom()` first
   (`amdgpu_bios.c:135`), so a real BAR probe/save/restore implementation is
   a Phase 5 requirement.  A ROM signature can be validated on virtio-vga
   (has an option ROM) under `run-linuxdrm` before the VFIO hash test in C7.
4. **The `struct dev_pm_ops` overlay placeholder cannot compile amdgpu.**
   `kernel/linuxkpi/include/linux/device.h` defines
   `struct dev_pm_ops { int kpi_unused; }`; `amdgpu_pm_ops`
   (`amdgpu_drv.c:2741`) initializes 12 callbacks
   (prepare/complete/suspend/suspend_noirq/resume/freeze/thaw/poweroff/
   restore/runtime_suspend/runtime_resume/runtime_idle).  P5 must grow the
   type to the 6.6 field set.  Stock `include/linux/pm_runtime.h` under
   `!CONFIG_PM` already provides compiling no-op inlines for
   `pm_runtime_*`; P5 needs the type, the PCI PM entry points, and docs —
   not a runtime-PM implementation.
5. **Do not import `drivers/i2c/i2c-core-base.c` in P5** (2723 LOC; needs a
   real driver core, OF/ACPI, PM, sysfs bus attrs, tracepoints).  Primary
   path: self-authored minimal core (~600 LOC) with a grown overlay.  The
   overlay must grow to the fields amdgpu touches:
   `amdgpu_dm.c:create_i2c()` sets `.owner`, `.class = I2C_CLASS_DDC`,
   `.dev.parent`, `.algo`, `.name` and uses `i2c_set_adapdata()`; the
   adapter also needs `nr`, `retries`, `timeout` and a bus lock.  Stock
   `include/linux/i2c.h` stays shadowed (it pulls acpi/of/irqdomain).
6. **IRQ dispatch needs no per-vector stub table.**  `isr_t` receives
   `struct registers *` whose `int_no` is the vector, so one
   `linuxkpi_irq_dispatch()` trampoline can index the descriptor table.
   The native `pci_irq_request_modes_routed()` `handlers[]` array can be
   filled with that same function for every vector.
7. **The IRQ test device is EDU, not nvme.**  A Linux driver cannot own
   nvme while the native driver binds it.  EDU has deterministic interrupt
   registers in BAR0: status `0x24` (RO), raise `0x60` (WO), acknowledge
   `0x64` (WO), and is MSI-capable.  A raise/ack loop gives exact counts;
   the INTx fallback is testable with `PCI_IRQ_INTX`.
8. **The PCI state/PM helpers amdgpu uses are pure software bookkeeping in
   P5 scope:** `pci_save_state`/`restore_state`/`store_saved_state`/
   `load_saved_state` (config-space buffer) and `pci_set_power_state`/
   `pci_wake_from_d3` (no-op with `current_state` tracking).  No ASPM or
   real D-state transitions.
9. **`pci_alloc_irq_vectors()` and friends have no implementation today**
   (`docs/linuxkpi-gaps.md`, C4).  They only fail at link time when
   referenced; C2 makes them real.
10. **Native sysfs already has `/sys/bus/pci/devices/<bdf>` and
    `/sys/devices/pci0000:00/<bdf>`** (`kernel/src/fs/sysfs_pci.c`), and
    `asc_sysfs_attr_file()` can attach dynamic files to any directory node.
    P5 extends the bridge with a path lookup (e.g.
    `asc_sysfs_dir_by_path("/sys/bus/pci/devices/0000:0e:00.0")`) and
    materializes Linux `dev_groups`/`bin_attributes` there, instead of
    building a second tree.
11. **The 24-hour soak is a user-run gate.**  Define a 1-hour agent-run gate
    (full suites + `bin/test_kpi_dmabuf 3600` at `-smp 4`) and a written
    24-hour user protocol; keep the IA32_FMASK change in its own revertable
    commit.
12. **`pci_dev` wrappers already populate most inline-helper fields.**
    The remaining PCI work is state save/restore, ROM, MSI vectors and a few
    helpers — not a rewrite.  `pci_dev_get/put` are no-ops (wrappers live
    forever); keep and document unless amdgpu hot-unbind demands otherwise.

---

## 2. Chunk status board

| Chunk | Title | Size | Depends on | Exit gate |
|-------|-------|------|------------|-----------|
| C0 | Baseline closeout (P4 exit still green) | S | — | C5 green + soak `closes == iterations`, delta ≈ 0 |
| C1 | Full Linux PCI API (non-IRQ) | L | C0 | extended PCI suite green (state, ROM, PCIe helpers) |
| C2 | IRQ core + MSI/MSI-X bridge | XL | C1 | EDU MSI-X/INTx/threaded IRQ tests, exact counts |
| C3 | ACPI audit + firmware verification | M | C0 | FADT via shim + firmware file test from disk |
| C4 | Minimal I2C core (DDC/EDID) | L | C0 | fake-adapter suite + `drm_edid` over fake DDC |
| C5 | sysfs/devres/device/PM completion | L | C1, C2 | groups/bin attrs live; amdgpu-shaped driver compiles/links |
| C6 | IRQs-on syscalls + context tracking | L | C2 | 1 h agent soak + 24 h user protocol; no hangs |
| C7 | VFIO hardening + Raphael validation | L | C1–C3 | bind/map/MSI-X/request_irq/ROM hash under `run-vfio` |
| C8 | Phase closeout | S | all | docs + full regression + P6 handoff tag |

Sizes are relative: C0/C8 ≈ a short session, C1/C3/C5 ≈ one session each,
C2/C4/C6/C7 ≈ one to two sessions each.  Do chunks in order; every chunk
ends with the full boot regression (all `test_phase*` suites + `make run`
desktop when interactive).  C3 and C4 may be swapped; both only need C0.

Coverage ordering note: C1 and C2 are deliberately separate.  C1 is
non-IRQ PCI surface and can be validated on EDU/virtio-vga without touching
the interrupt controller; C2 then adds vectors/handlers on top of the
state C1 established.

---

### C0 — Baseline closeout (gate; do not skip) — DONE 2026-09-13

Goal: prove the Phase 4 exit criteria are still green and freeze the
baseline before any Phase 5 code lands.

Status: closed by the maintainer.  The bochs canary runs (bind on
`0000:00:03.0`, atomic modeset, VBE match, BAR0 readback, 2x128 loop stable),
`page order alloc/free` is green after the `page_to_phys()` /
`__GFP_COMP` fixes, and all P0–P4 suites pass in the same boot.  The task
list below is kept as the historical checklist; C1 is the first open chunk.

Tasks:
1. C5 bochs canary: commit `test_phase4_bochs.c` and the build plumbing
   (`run-linuxdrm` with `bochs-display`, `CONFIG_DRM_BOCHS`,
   `files.txt`).  The headless boot must show
   `[drm] Initialized bochs-drm ... for 0000:00:03.0 on minor 2`, the
   `[  OK  ]` bochs suite lines (objects, VBE mode match, BAR0 pattern
   readback, 2×128 loop stable) and no `[FAIL]`.
2. Confirm the `page_to_phys()` non-compound fix is in the baseline and the
   Phase 2 page suite is green again (`__GFP_COMP` compound tests plus the
   non-compound pass).
3. P2 soak: fresh boot without the desktop, `bin/test_kpi_dmabuf 600` at
   `-smp 4`; expect `closes == iterations` and a delta of 0/±1
   (warm-up-baseline rule).  If it regresses, stop and diagnose.
4. Re-run the P3/P4 checks: `/dev/dri` lists `card0`, `card1`, `card2`,
   `renderD128`; `bin/test_kpi_drm` and `bin/test_kpi_dmabuf` pass;
   `nm kernel/bin-x86_64/kernel | grep ' T drm_'` shows only imported
   symbols.
5. Record the P4 C6/C7 skip decision and update
   `docs/linuxkpi-progress.md` with the P4 exit evidence and a baseline tag.

Evidence: serial excerpts + exact commands in the progress doc; `make -C
kernel` clean; `make` ISO up to date.

Exit: all P0–P4 kernel suites green at `-smp 4`; desktop boots; soak
`closes == iterations` and delta ≈ 0.

---

### C1 — Full Linux PCI API (non-IRQ)

Goal: complete the driver-facing PCI surface the P6a compile needs that is
not IRQ- or PM-related, without changing native PCI behavior.

Test first: `kernel/src/tests/linuxkpi/linux/test_phase5_pci.c` (a new file;
keep the C4 evidence immutable), wired into `boot_tests.c` after the C4 PCI
suite.  Without the test device it logs `[SKIP]` parts, never fails.

Tasks (census from the 6.6 amdgpu tree; counts are occurrences):
1. State save/restore in `kernel/linuxkpi/src/pci.c`:
   - `pci_save_state` → snapshot 256 bytes of standard config space into a
     per-wrapper buffer (`kzalloc`, freed on `pci_release_dev`/wrapper
     teardown); `pci_restore_state` → write it back.
   - `pci_store_saved_state` returns an opaque `struct pci_saved_state *`
     (owned buffer); `pci_load_saved_state` copies it into the wrapper;
     `pci_load_and_free_saved_state` frees it.
   - Documented divergence: only config space 0x00–0xFF is saved; no
     PMCSR/ASPM/PCIe capability bookkeeping.
2. ROM:
   - decode the ROM BAR (`PCI_ROM_ADDRESS`, config offset 0x30) at scan
     into `pdev->resource[PCI_ROM_RESOURCE]` when enabled and non-zero; do
     **not** size-probe standard BARs or the ROM at scan.
   - `pci_map_rom()`: standard dance — save the ROM BAR, disable it, write
     ~0, read the size mask, restore; if a size results, `ioremap()` the ROM
     address and report `*size`; `pci_unmap_rom()` unmaps.
   - keep `pci_enable_rom()`/`pci_disable_rom()` but fix the enable bit
     semantics if needed.
   - native bridge: no native change expected (config access covers 0x30);
     verify enumeration does not clear the ROM BAR.
3. Inline-helper dependencies the P6a compile will hit (verify against the
   wrapper, add only what is missing): `pci_upstream_bridge`,
   `pci_is_root_bus`, `pci_domain_nr` (domain 0), `pci_dev_id`,
   `pci_revision_id`, `pci_pcie_type`, `pci_address_name`,
   `pci_device_is_present`, `pci_dev_is_disconnected`,
   `pci_wait_for_pending_transaction`, `pci_release_resource`.
4. `pci_p2pdma_distance()` weak returning 0 (amdgpu VRAM/P2P paths); a
   full P2P implementation is not in scope.
5. `pcie_get_speed_cap()`, `pcie_get_width_cap()`,
   `pcie_print_link_status()`.
6. Keep `pci_dev_get()`/`pci_dev_put()` no-ops and document it (wrappers
   live for the kernel lifetime; there is no hot-unbind yet).

Coverage:
1. EDU: state snapshot/restore roundtrip — save, flip a writable command
   bit, restore, compare the 256-byte image; store/load/load-and-free
   roundtrip.
2. virtio-vga (`1af4:1050`, under both `make run` and `run-linuxdrm`):
   `pci_map_rom()` returns non-NULL, size sane, first two bytes `55 aa`;
   unmap; 16 map/unmap cycles with a PMM baseline.
3. PCIe speed/width reads on a PCIe device (nvme or virtio) log sane
   values; `pci_is_pcie`, `pci_pcie_type` consistent.
4. EDU lifecycle + BAR tests unchanged (no C4 regression).
5. `pci_p2pdma_distance()` returns 0 without touching device state.

Evidence: `[  OK  ]` lines for each item; `[SKIP]` and not `[FAIL]` on
plain `make run` for EDU-only parts; PMM stable.

Exit: suite green at `-smp 4` on `run-linuxdrm` (and `make run`);
boot regression clean; every divergence in `docs/linuxkpi-gaps.md`.

Risks: touching the ROM BAR of a live device must never touch the six
standard BARs (native recorded `bar_size[6]`; keep it that way); QEMU
virtio-vga may keep the ROM BAR disabled until `pci_enable_rom()`;
restoring a config snapshot must not re-enable memory decode behind the
native driver's back (save the original, write only during the test on a
device the test owns, or use EDU).

---

### C2 — IRQ core + MSI/MSI-X bridge

Goal: Linux drivers can allocate MSI/MSI-X vectors and run hardirq and
threaded handlers; EDU fires deterministically.

Test first: `kernel/src/tests/linuxkpi/linux/test_phase5_irq.c`.

Design:
1. `kernel/linuxkpi/src/irq.c` (new): descriptor table indexed by native
   vector (0–255):
   `{ handler, thread_fn, dev_id, flags, name, enabled, in_flight, refcount,
   lock }`.  One `linuxkpi_irq_dispatch(struct registers *regs)` trampoline
   reads `regs->int_no` and runs the chain; bracket with
   `linuxkpi_irq_enter/exit` only if the native ISR does not already do it
   (check `kernel/src/cpu/isr.c`; P1 added the hooks there).
2. `request_irq()` / `request_threaded_irq()` / `free_irq()`
   (`IRQF_SHARED`, `IRQF_ONESHOT`, shared chains), `enable_irq()` /
   `disable_irq()` / `disable_irq_nosync()` / `synchronize_irq()`,
   `irq_set_affinity_hint()` no-op, `irq_update_affinity_hint()`.
3. Threaded handlers: a dedicated kthread queue
   (`irq_thread`), per-descriptor work item.  The hard handler returning
   `IRQ_WAKE_THREAD` queues `thread_fn`; the thread runs with IRQs on and
   may sleep.  Document the model (Linux `irq_thread` equivalent, not a
   generic IRQ framework).
4. Native bridge: extend `kernel/linuxkpi/include/linuxkpi/native_irq.h` +
   `kernel/src/linuxkpi/native_irq.c` (new) with
   `linuxkpi_native_irq_alloc(handler)` / `_free(vector)` over
   `interrupt_vector_alloc()/free()`, and MSI/MSI-X allocation through
   `pci_irq_request_modes_routed()` using the opaque native handle from
   `native_pci.h` (`struct pci_irq` stays native; the wrapper stores an
   opaque pointer).
5. `devm_request_irq()` + `devm_free_irq()` as a devres action in
   `kernel/linuxkpi/src/device.c` (the declaration already exists in the
   device.h overlay but has no implementation).
6. PCI API in `kernel/linuxkpi/src/pci.c`:
   `pci_alloc_irq_vectors[_affinity]`, `pci_free_irq_vectors`,
   `pci_irq_vector`, `pci_msi_vec_count`, `pci_msix_vec_count`
   (capability decode), `pci_irq_get_affinity` (NULL).
   amdgpu's `PCI_IRQ_MSI | PCI_IRQ_MSIX` request with min=max=1 must
   succeed on EDU and on the Raphael in C7.
7. `irq_work.c` (P3) stays as-is; document that it is a separate API.

Coverage:
1. EDU: `pci_msix_vec_count() > 0`; allocate 1 MSI-X vector; `pci_irq_vector`
   valid; `request_irq`; write `0x100` to BAR0+0x60; bounded wait for the
   handler; read `0x24` shows `0x100`; ack `0x64`; 256-raise loop with an
   exact count.
2. INTx fallback: free, allocate with `PCI_IRQ_INTX` only; the same
   raise/ack loop (native routes through the I/O APIC).
3. Threaded: hard handler returns `IRQ_WAKE_THREAD`; the thread function
   increments a completion and asserts `!in_interrupt()`.
4. Shared: two handlers on one vector with `IRQF_SHARED`; both run; freeing
   one leaves the other working.
5. `disable_irq`/`enable_irq`/`synchronize_irq` do not lose or duplicate
   interrupts in a bounded loop.
6. `devm_request_irq` on a fake device: unregister frees the handler (no
   fire after).
7. 10k-interrupt stress with an exact count and a PMM baseline.

Evidence: `[  OK  ]` lines; count exact; no lost interrupts; no WARN.

Exit: suite green at `-smp 4` on `run-linuxdrm`; no C1 regression; boot to
login clean.

Risks: hard handlers run in the native ISR (no sleep, no blocking allocs);
`disable_irq` vs an in-flight handler is the classic race — use the
spinlock + in-flight refcount pattern and keep it simple; EOI ordering —
confirm the native ISR EOIs after handler return and that `pci_irq_mask()`
is only used by oneshot/disable paths; native vector pool is shared (128
vectors), so leaks fail later tests.

---

### C3 — ACPI audit + firmware verification

Goal: keep `CONFIG_ACPI=n` viable through P6, make native tables reachable
through the Linux API for later phases, and prove the firmware loader from
the disk image.

Test first: `kernel/src/tests/linuxkpi/linux/test_phase5_acpi.c` and
`test_phase5_firmware.c`.

Tasks:
1. Audit (record in `docs/linuxkpi-gaps.md`): with `CONFIG_ACPI=n`,
   `amdgpu_acpi.o` and `amdgpu_atpx_handler.o` are not built; the
   `amdgpu_bios.c` VFCT path is `#ifdef CONFIG_ACPI`; display ACPI/brightness
   paths are behind `#if defined(CONFIG_ACPI)`; kfd is off.  No compiled
   TU should reference ACPI functions outside the overlay; verify with a
   compile of the P6a subset when it exists (C7 spike may add one TU).
2. Shim groundwork: extend `kernel/linuxkpi/include/linux/acpi.h` with
   `acpi_status`, `ACPI_SUCCESS`, `ACPI_SIG_*` constants needed (FADT/APIC
   for tests, VFCT for later), `acpi_get_table`/`acpi_get_table_with_size`/
   `acpi_put_table` over a new native bridge exposing
   `acpi_find_table()` (`kernel/include/linuxkpi/native_acpi.h`).
   `acpi_put_table` is refcount-free (no-op) and documented.
3. Test: FADT (`FACP`) non-NULL, length sane, `ACPI_SUCCESS`; APIC table
   readable; unknown signature returns an error; `acpi_put_table` safe;
   table memory stays valid after boot (native keeps the RSDT/XSDT root).
4. Firmware: `scripts/linux-firmware-install.sh` stages a known
   `test_fw.bin` (e.g. 4 KB deterministic pattern) into the disk image's
   `/lib/firmware/`; `test_phase5_firmware.c` verifies size, first/last
   bytes and a CRC32, `-ENOENT` for a missing name, and `release_firmware`
   wipe behavior.  Wire the manifest entry.
5. Optional: add the Raphael firmware names (commented) from P6c so the
   manifest is ready; no blobs installed yet.

Evidence: `[  OK  ]` lines; the firmware file's CRC printed by the kernel
matches the host file.

Exit: tests green at `-smp 4`; ACPI audit recorded; boot regression clean.

Risks: `acpi_get_table` name/type collisions if `CONFIG_ACPI` is ever
enabled later — keep the shim minimal and clearly marked; do not let tests
write table memory.

---

### C4 — Minimal I2C core (DDC/EDID)

Goal: `i2c_add_adapter`/`i2c_transfer` work for DRM EDID and the future
amdgpu DM adapter.

Test first: `kernel/src/tests/linuxkpi/linux/test_phase5_i2c.c`.

Decision (see §1.5): self-author the core; keep the overlay; no
`i2c-core-base.c` import in P5.  A timeboxed import spike is a P6
contingency only.

Tasks:
1. Grow `kernel/linuxkpi/include/linux/i2c.h`: adapter
   `{owner, class, algo, algo_data, dev, nr, name[48], retries, timeout,
   quirks, bus_lock}`, `struct i2c_algorithm {master_xfer, smbus_xfer,
   functionality}`, minimal `i2c_client`, `I2C_FUNC_*`, `I2C_CLASS_DDC`,
   `i2c_get_adapdata`/`i2c_set_adapdata`.
2. `kernel/linuxkpi/src/i2c.c` (new): adapter registry (mutex + idr or
   array), `i2c_add_adapter` / `i2c_add_numbered_adapter` /
   `i2c_del_adapter`, `i2c_transfer` / `__i2c_transfer` (adapter mutex,
   `retries` semantics), `i2c_master_send`/`i2c_master_recv`,
   `i2c_get_adapter`/`i2c_put_adapter`, `i2c_verify_adapter`.
   `i2c_smbus_*`: implement only if the P6a census shows a consumer
   (current amdgpu uses only `i2c_transfer`).
3. Remove the weak `i2c_transfer`/`i2c_master_*` stubs from
   `kernel/linuxkpi/src/drm_compat.c` (or leave them weak and note the
   real definitions override).
4. No `/dev/i2c-N`, no clients/instantiation, no OF/ACPI adapter lookup in
   P5; document.

Coverage:
1. Fake adapter with a scripted backend: message roundtrip, address/flag
   handling, `I2C_M_RD`, zero-length messages if the callers need them.
2. Retry: fail once with `-EAGAIN`, succeed on retry, verify `retries`.
3. Serialization: two threads through one adapter; the backend sees
   non-overlapping transfers.
4. Lifecycle: add/del adapter repeated (~1k), id reuse rules, PMM stable.
5. EDID path: a fake DDC adapter (address `0x50`) serving a valid 128-byte
   EDID; drive the same transfer sequence `drm_edid.c` uses.  If the full
   `drm_edid_read_ddc` needs connector plumbing, use the message-level
   sequence and record that the full path is P6e evidence.

Evidence: `[  OK  ]` lines; adapter id/leak checks; PMM stable.

Exit: suite green at `-smp 4`; `drm_edid` message semantics validated;
boot regression clean.

Risks: `i2c_adapter` embeds `struct device`; registering it as a class
device must not disturb the native device tree (make the sysfs side
optional and default off); locking is the subtle part (never hold the
adapter mutex across a sleeping hardirq path).

---

### C5 — sysfs/devres/device/PM completion

Goal: dynamic attribute groups and binary attributes work on registered
devices, devres is complete enough for DRM/amdgpu, and the PM/clk/reset
surface compiles with inert semantics.

Test first: `kernel/src/tests/linuxkpi/linux/test_phase5_sysfs.c` plus a
compile/link "amdgpu-shaped" driver in `test_phase5_devmodel.c`.

Tasks:
1. Native sysfs bridge: add
   `asc_sysfs_dir_by_path(const char *path)` (resolve an existing dir via
   `vfs_resolve_path`) so the device core can attach Linux attributes to
   `/sys/bus/pci/devices/<bdf>`, `/sys/devices/pci0000:00/<bdf>` and class
   dirs.  Keep `asc_sysfs_attr_file()` as the only file-creation primitive.
2. Implement real sysfs calls in `kernel/linuxkpi/src/kobject.c`:
   `sysfs_create_file[_ns]`, `sysfs_create_files`, `sysfs_create_group(s)`,
   `sysfs_remove_*`, `sysfs_create_bin_file`/`sysfs_remove_bin_file`.
   - attribute → dir resolution via the kobject (store a `void *kpi_dir`
     on the kobject overlay or resolve by name);
   - `is_visible` honored; `device_attribute.show/store` bridged to the
     native attr file;
   - `bin_attribute` read/write with `loff_t` offset handling (partial
     reads), `bin_attr.size`; `mmap` returns `-ENOSYS` in P5;
   - free attribute contexts on remove (P3 carry-over).
3. `device_add_groups()`: attach `dev_groups` to the device's native dir
   (PCI wrapper → `/sys/bus/pci/devices/<bdf>`; class device →
   `/sys/class/<class>/<name>`).  `device_del()` removes them.
4. Device/devres completion: `device_create_with_groups`,
   `get_device`/`put_device` (real refcount if it stays simple; otherwise
   documented no-op), `devm_kasprintf`, `devm_kmalloc_array`,
   `devm_platform_ioremap_resource`, `devm_ioremap_resource`,
   `devm_clk_get`/`devm_reset_control_get*` no-ops.
   `devm_request_irq` lands in C2.
5. PM/clk/reset: replace `struct dev_pm_ops` with the 6.6 layout; implement
   `pci_set_power_state` (records `current_state`, returns 0),
   `pci_choose_state`, `pci_wake_from_d3` (returns 0); verify stock
   `pm_runtime.h` no-ops compile against the overlay; verify stock
   `clk.h`/`reset.h` no-op paths; document that real runtime PM/suspend is
   P8.

Coverage:
1. Fake device with a `dev_groups` array (RO/RW attrs, `is_visible` skip):
   create, read/write through the sysfs path, remove, re-create; contexts
   freed (no leak).
2. Binary attribute: partial reads at offsets, full read, write rejected
   (or accepted per mode), removal.
3. Devres: registrations released on device unregister in reverse order;
   `devm_request_irq` freed (from C2).
4. Synthetic amdgpu-shaped driver: `struct pci_driver` with `.dev_groups`,
   `.driver.pm = &fake_pm_ops`, `pm_runtime_get_sync`/`put_autosuspend`/
   `mark_last_busy` calls — compiles and links, probes on EDU, does nothing.
5. `/sys/bus/pci/devices/<bdf>` gains a test attribute visible via the
   existing native sysfs tree.

Evidence: `[  OK  ]` lines; no WARN; PMM stable; the synthetic driver's
probe/remove log.

Exit: suite green at `-smp 4`; P3 sysfs carry-overs (per-attribute contexts,
bin attrs) closed or explicitly re-scoped.

Risks: kobject ↔ dir mapping must survive device_del; attribute memory is
usually statically allocated by drivers — never free the `attribute` itself,
only the bridge context; keep `/sys/kobject` UAFs impossible (remove before
free).

---

### C6 — IRQs-on syscalls + context tracking + soak

Goal: remove the `IA32_FMASK` IF mask on syscall entry, prove context
tracking, and soak the result.

Tasks:
1. `kernel/src/syscalls/syscall.c` (~line 612):
   `wrmsr(IA32_FMASK, 0x200 | (1ULL << 18))` → mask only AC
   (`wrmsr(IA32_FMASK, (1ULL << 18))`).  Keep this as its own commit with
   the revert documented in the commit message and the progress doc.
2. Verify syscall entry/exit: no path may rely on IRQs being off; keep the
   exit reschedule/signal delivery; if needed, track `in_syscall` for
   context tests.
3. `might_sleep()`: WARN-once when `in_atomic()`/`in_interrupt()`/
   `preempt_count() != 0`; warning, not BUG.
4. Test `test_phase5_ctx.c`: preempt count and `in_interrupt()` transitions
   around handlers; `might_sleep` path does not fire in normal syscall
   context.
5. Regression + soak:
   - full kernel suite + `bin/test_kpi_drm` + `bin/test_kpi_dmabuf` at
     `-smp 4`;
   - 1-hour agent gate: `bin/test_kpi_dmabuf 3600` headless with serial
     capture (`run-linuxdrm`, no desktop), expect `closes == iterations`
     and delta ≈ 0;
   - 24-hour user protocol (documented, user-run): boot interactive with
     serial captured to `build/logs/p5-soak-24h.log`, log into the desktop,
     run `bin/test_kpi_dmabuf 86400`, leave the session; after 24 h check
     the log for `delta ≈ 0`, no `[FAIL]`, no hang/WARN.

Evidence: serial excerpts for the 1 h run; user-run 24 h result recorded in
the progress doc (or an explicit "pending" with the protocol reproduced).

Exit: 1 h agent evidence green; 24 h user protocol executed and recorded;
no hangs; all suites green with IRQs on.

Risks: this is the phase's highest-risk chunk.  Latent races that assumed
IRQs off in syscalls (per-CPU data, fd tables, schedulers) surface here.
Keep the change isolated, run the full suite after it, and if instability
appears, revert the one-line commit and reopen C6 rather than debugging
forward under a mixed state.

---

### C7 — VFIO hardening + Raphael validation

Goal: prove the real GPU path under `make run-vfio` with the current
native stack, and produce the VBIOS hash evidence.

Tasks:
1. `make run-vfio` (top-level GNUmakefile): add `SERIAL ?= stdio` and
   `VFIO_EXTRA ?=` (appended to the `-device` list) so headless captures
   and an extra `-device edu` work:
   `make run-vfio SERIAL=file:build/logs/p5-vfio.log VFIO_EXTRA='-device edu'`.
   Keep the `romfile=` auto-detection and `rombar=1`.
2. Kernel test driver
   `kernel/src/tests/linuxkpi/linux/test_phase5_vfio.c`, gated by the
   module parameter `kpi_vfio_test` (default 0, set on the kernel cmdline
   only for this test) so P6a can bind amdgpu on the same boot without the
   test owning the device:
   - locate `1002:164e` (`pci_get_device`), log `pci_name`, class,
     revision, BAR sizes;
   - `pci_enable_device`; `pci_request_regions`; `pci_iomap` BAR5 (MMIO
     registers) and read one safe register (log the value; no writes);
   - `pci_set_master` then `pci_clear_master`;
   - `pci_map_rom` → size + first bytes + CRC32; `pci_unmap_rom`;
   - `pci_msix_vec_count`, `pci_alloc_irq_vectors(1, 1, MSI|MSIX)`,
     `pci_irq_vector`, `request_irq` (installed; the GPU will not raise
     without firmware, so no fire expected), `free_irq`,
     `pci_free_irq_vectors`;
   - release regions; leave the device in the state it was found.
3. Host-side hash compare: the kernel logs
   `rom crc32=0x%08x size=%zu`; the host computes the same with
   `python3 -c 'import zlib; print(hex(zlib.crc32(open("build/vfio/vbios.rom","rb").read())))'`
   (`zlib.crc32` matches the imported `crc32_le` finalization).  A small
   `scripts/vfio-check-rom.sh` helper may wrap
   `scripts/vfio-vbios.sh` + the compare but is optional.
4. Run the same boot with `VFIO_EXTRA='-device edu'` so the C2 EDU IRQ
   suite runs under VFIO.
5. Confirm `make run-vfio` reaches the login prompt with no unrelated WARN
   from imported PCI/IRQ/DRM code.

Evidence: serial lines for bind (`1002:164e`), ioremap, MSI/MSI-X counts,
`request_irq` installed, ROM size/CRC matching the host hash, EDU IRQ
suite green; `make run-vfio` clean to login.

Exit: all of the above at `-smp 4`; P0–P5 regression green; no native
device disturbed (nvme, virtio, native GPU).

Risks: VFIO ROM BAR may be disabled until `pci_enable_rom()`; the test must
not write MAR/MMIO state that later confuses amdgpu; the `kpi_vfio_test`
gate must default off so P6 boots are unaffected; the same IOMMU group
contains the GPU audio function (`0e:00.1`), which stays on the host — only
`0e:00.0` is passed through.

---

### C8 — Phase closeout

Goal: make Phase 5 reproducible for the P6 agent.

Tasks:
1. `docs/linuxkpi.md`: add PCI/IRQ/i2c/sysfs/devres/PM sections, the
   overlay-vs-import rules actually used, and a short "porting the next
   driver" checklist.
2. `docs/linuxkpi-gaps.md`: every P5 divergence in one place (ACPI off and
   why, i2c minimal core, IRQ threaded model, ROM scope, PM stubs, sysfs
   path model, no `pci_dev` refcounting).
3. `docs/linuxkpi-progress.md`: P5 exit matrix with serial excerpts and
   exact commands, open items carried into P6.
4. Start `docs/amdgpu-testing.md`: VFIO setup, VBIOS extraction, firmware
   staging, known issues.
5. Full regression: all `test_phase*` suites, `bin/test_kpi_drm`,
   `bin/test_kpi_dmabuf`, interactive desktop, `make run-vfio`,
   `make -C kernel` clean, symbol check
   (`nm kernel/bin-x86_64/kernel | grep ' T drm_'`).
6. Tag the P6 baseline commit.

Exit: docs complete; clean bootstrap from the tag; P6 agents can start 6a
with no open P5 item.

---

## 3. Cross-chunk rules (project working rules, restated)

- Tests before implementation: new API chunks start with the suite in
  `kernel/src/tests/linuxkpi/linux/`; run under `-smp 4`.
- Never modify `kernel/linux/**`; every divergence is an overlay
  (`kernel/linuxkpi/include/...`) or glue (`kernel/linuxkpi/src/...`,
  `kernel/src/linuxkpi/...`) change, recorded in `docs/linuxkpi-gaps.md` in
  the same change.
- Every chunk ends with the full regression: kernel suite + userland
  `test_kpi_dmabuf` + `test_kpi_drm` + interactive desktop boot.  A chunk is
  not done until the previous chunks' evidence is still green.
- PMM free-page invariants use a warm-up before sampling the baseline and
  must not go backwards.
- Kernel tests run in a kthread via `boot_tests.c` with bounded waits; ioctl
  argument pages are mapped into the PML4 user range as in
  `test_phase3_drm_modeset.c`.
- Update `docs/linuxkpi-progress.md` after every working session (chunk,
  command, evidence, deviation, next step).
- Headless evidence harness (canonical form; quote `DISPLAY_OPT` or make
  eats the value — `DISPLAY_OPT='-display none'`):

  ```
  make run-linuxdrm SERIAL=file:build/logs/p5-cX.log \
       DISPLAY_OPT='-display none'
  ```

  or the explicit QEMU form used from C4 on:

  ```
  qemu-system-x86_64 -M q35 -m 4G -cpu host -enable-kvm -smp 4 \
    -drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \
    -cdrom avoryos-x86_64.iso \
    -drive file=build/disk-test.img,format=raw,if=none,id=nvme0 \
    -device nvme,serial=avoryos0,drive=nvme0 \
    -vga none -device virtio-vga -device bochs-display -device edu \
    -display none -serial file:build/logs/p5-cX.log
  ```

  (Use a scratch disk image when `disk.img` is locked by an interactive
  session.)

## 4. Mapping to the original Phase 5 exit criteria

| Original criterion | Covered by |
|---|---|
| Test PCI driver binds Raphael under VFIO | C1 (driver surface) + C7 (bind) |
| ioremap works on BAR5 | C1 + C7 |
| MSI-X vectors allocated; a Linux `request_irq` handler runs | C2 (EDU exact fire) + C7 (Raphael allocation) |
| VBIOS hash match | C1 (`pci_map_rom`) + C7 (hash compare) |
| Firmware loader reads a test file from the disk image | C3 |
| 24-hour desktop soak with IRQs-on syscalls, no hangs | C6 (1 h agent + 24 h user) |
| Stress suite green | C6 + every chunk regression |
| `make run-vfio` boots with no unrelated kernel logs | C0 + C7 + every chunk |

## 5. Decisions to confirm before C1

1. I2C: self-authored minimal core (recommended) vs a timeboxed
   `i2c-core-base.c` import spike.
2. Keep `CONFIG_ACPI=n` through P6 with the table shim only (recommended)
   vs enabling ACPI now.
3. IRQ numbering: native vector == Linux `irq` number, one dispatcher via
   `regs->int_no` (recommended) vs synthetic irq numbers with a remap.
4. New `test_phase5_*.c` files (recommended; keeps P4 evidence immutable)
   vs extending the P4 suites.
5. VFIO test driver gated by the `kpi_vfio_test` cmdline parameter
   (recommended) vs a separate build flag.
6. ROM/VBIOS test devices: virtio-vga under `run-linuxdrm` and Raphael
   under `run-vfio` (recommended).
7. Soak split: 1 h agent gate + 24 h user protocol (recommended).
8. `pci_dev_get/put` stay documented no-ops for P5 (recommended).

## 6. Out of scope (documented deferrals)

- Enabling `CONFIG_ACPI` / `amdgpu_acpi.c` / ATPX (P8, bare metal).
- Real runtime PM, system suspend/resume, ASPM, clock/power gating (P8).
- Debugfs, tracepoints, jump labels (still off).
- i2c clients/instantiation, `/dev/i2c-N`, full SMBus (only if a P6a
  consumer appears).
- Multi-vector affinity beyond "N vectors work" (amdgpu needs 1).
- HMM/mmu_notifier (P8), P2P DMA, PCIe error recovery (`err_handler` only
  needs to compile in P5).
