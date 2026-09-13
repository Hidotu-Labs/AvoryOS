# LinuxKPI on AvoryOS

AvoryOS runs unmodified upstream Linux drivers through a LinuxKPI layer.  The
first target is the AMD `amdgpu` driver (Linux 6.6 LTS) on the Raphael iGPU
(gfx1036, DCN 3.1.x); the layer is generic and later phases reuse it for more
DRM drivers (nouveau, i915, ...) and other subsystems.

Phase 0–5 built the surface a real PCIe GPU needs.  This document is the
entry point for the next driver/porting session; per-chunk evidence lives in
`docs/linuxkpi-progress.md`, every divergence in `docs/linuxkpi-gaps.md`, and
the VFIO workflow in `docs/amdgpu-testing.md`.

## Layout

| Path | Role |
|---|---|
| `kernel/linux/` | Pinned upstream Linux subset, vendored by `scripts/linux-import.sh`. Gitignored; never edited. |
| `scripts/linux/subset.txt` | Paths copied from the Linux tree (dirs/files). |
| `scripts/linux/files.txt` | Upstream `.c` files compiled into the kernel. |
| `scripts/linux/firmware-manifest.txt` | Firmware blobs staged for `/lib/firmware` by `scripts/linux-firmware-install.sh`. |
| `kernel/linuxkpi/include/` | Overlay headers: same paths as upstream, first on the include path. Additive ones use `#include_next`. |
| `kernel/linuxkpi/include/generated/` | Hand-maintained kernel config (`autoconf.h`) and build identity for imported code. |
| `kernel/linuxkpi/src/` | LinuxKPI implementations: imported-API entry points (`pci.c`, `irq.c`, `i2c.c`, `device.c`, `kobject.c`, `acpi.c`, `firmware.c`, ...). |
| `kernel/src/linuxkpi/` | Native bridges: code that knows both the native types and the Linux API (`native_pci.c`, `native_irq.c`, `native_acpi.c`, `native_vfs.c`, `native_sched.c`, ...). |
| `kernel/src/include/linux/` | Legacy stub headers. Imported code does **not** see them; they are being retired as real Linux headers take over. |

## Build integration

`kernel/GNUmakefile` includes `kernel/linux/Makefile.files` (generated) and
compiles listed sources into `obj-$(ARCH)/linux/...` with their own include
order: `linuxkpi/include`, then the Linux tree, and deliberately not the legacy
stubs in `src/include`.  LinuxKPI sources and Linux-API boot tests are picked
up with `find` from `linuxkpi/src` and `src/tests/linuxkpi/linux`, so adding a
file needs no manifest change.  If the Linux tree has not been imported,
`LINUX_OBJ` is empty and the kernel still builds; the Phase 0 self-test logs
that the import is missing.

Commands:

```sh
scripts/linux-import.sh          # fetch/refresh the pinned v6.6.* subset
make -C kernel                   # build (import optional)
make                             # kernel + ISO + disk.img
make run-linuxdrm SERIAL=file:build/logs/p5.log DISPLAY_OPT='-display none'
make run-vfio KERNEL_CMDLINE=kpi_vfio_test=1 SERIAL=file:build/logs/p5-vfio.log \
     VFIO_EXTRA='-device edu'
```

## Kernel surface implemented through Phase 5

| Area | Entry points (implementation) | Notes |
|---|---|---|
| PCI | `linuxkpi/src/pci.c`, `kernel/src/linuxkpi/native_pci.c` | Config 8/16/32 + extended caps, BAR decoding, `pci_iomap`, region registry, state save/restore, ROM sizing + `pci_map_rom`, PCIe link helpers, direct probe/remove driver registry, inert D3 helpers. |
| IRQ / MSI | `linuxkpi/src/irq.c`, `kernel/src/linuxkpi/native_irq.c` | 256-entry vector table, hard/threaded/shared handlers, enable/disable/synchronize, `devm_request_irq`, MSI/MSI-X through `pci_irq_request_modes_routed()`. |
| ACPI | `linuxkpi/src/acpi.c`, `kernel/src/linuxkpi/native_acpi.c` | `acpi_get_table()`/`acpi_put_table()` over the native RSDT/XSDT walker (FADT/MADT/VFCT, ...). `CONFIG_ACPI` stays off. |
| Firmware | `linuxkpi/src/firmware.c` | Synchronous `request_firmware()` from `/lib/firmware` over the native VFS. |
| i2c | `linuxkpi/src/i2c.c` | Self-authored minimal core: adapter registry, `i2c_transfer` + retries, `i2c_master_send/recv`, EDID/DDC message semantics. |
| Device model | `linuxkpi/src/device.c`, `kobject.c` | `struct device`/driver overlays, classes, `device_create_with_groups`, devres (LIFO), dynamic sysfs attributes (device/class/bin, groups), inert `dev_pm_ops`/pm_runtime no-ops, PCI power bookkeeping. |
| Native bridges | `kernel/src/linuxkpi/native_*.c` | VFS, sysfs, scheduler, IRQ, PCI, ACPI, MM.  Only builtin types cross the bridge headers. |

## Overlay vs import rules (as actually used)

1. **Never modify `kernel/linux/**`.** Every divergence is an overlay
   (`kernel/linuxkpi/include/...`), a shim (`kernel/linuxkpi/src/...`), or a
   native bridge (`kernel/src/linuxkpi/...`) and is recorded in
   `docs/linuxkpi-gaps.md` in the same change.
2. **Config is hand-maintained** in `kernel/linuxkpi/include/generated/autoconf.h`.
   An absent symbol means disabled; never define a symbol as `0`.  Add a
   symbol only when an imported file reads it.
3. **Prefer upstream algorithms** (`lib/` imports, DRM/TTM/sched) over
   reimplementation; author a minimal shim only when the stock file drags in
   too much (i2c) and record why.
4. **Overlay headers are additive where possible.**  Use `#include_next` to
   wrap a stock header (e.g. `linux/kernel.h` for `might_sleep()`,
   `linux/crc32.h`) instead of copying it.
5. **Weak stubs live in `linuxkpi/src/drm_compat.c`** only while the real
   implementation does not exist; the real definition replaces the weak one
   without touching callers.  They are removed when the owning chunk lands.
6. **Tests first**: a new chunk starts with a `kernel/src/tests/linuxkpi/
   linux/test_phaseN_*.c` suite, wired into `linuxkpi/src/boot_tests.c`
   after the previous chunks.  Boot tests run in a kthread with bounded
   waits and a 60 s whole-suite timeout.
7. **Every chunk ends with the full regression** (all `test_phase*` suites,
   userland `bin/test_kpi_drm`/`bin/test_kpi_dmabuf`, desktop boot) and a
   progress-doc entry with the serial evidence.

## Porting the next driver: checklist

1. Census the driver's Linux API usage against what Phase 5 provides
   (`scripts/linux-files` import + compile failure is the authority; helper
   inlines in stock headers can hide missing symbols until link time).
2. Add the `autoconf.h` symbols the driver's Kbuild files select, and list
   the `.c` files in `scripts/linux/files.txt` (or the phase's import list).
3. Expect to extend, not rewrite: PCI/IRQ/i2c/devres/sysfs entries land in
   `linuxkpi/src/*`; only add a new overlay header when a stock one pulls in
   unsupported subsystems.
4. Keep `CONFIG_ACPI=n` and `CONFIG_PM*` off until the bare-metal phase: the
   `#else`/no-op arms are what amdgpu's ACPI/PM paths compile against.
5. Gate hardware tests behind a module parameter parsed from the limine
   `cmdline:` (`KERNEL_CMDLINE=...`), as `kpi_vfio_test` does, so a normal
   boot never owns a device a driver is about to bind.
6. Run the new suite plus the full regression; record every divergence and
   update `docs/linuxkpi-progress.md` with the exact command and serial lines.

## Initcalls

Upstream `module_init()` places pointers in `.initcall6.init`; the linker
script keeps all levels and `kernel/src/linuxkpi/init.c` walks them once, late
in `kmain_high_half()`.  With `CONFIG_MODULES=n` this is exactly how built-in
Linux drivers get started.

## Kernel FPU

The kernel is compiled `-mno-sse`; float-heavy imported files (AMD display DML)
are compiled with SSE per-directory and bracket their use with
`kernel_fpu_begin()/end`.  The implementation saves per-CPU XSAVE state with
interrupts masked; calls nest.  See `kernel/src/linuxkpi/fpu.c`.

## Rules

1. Never modify `kernel/linux/**`; changes belong in overlays or glue.
2. Prefer importing pure algorithms from upstream over reimplementing them.
3. Every phase ends with a bootable kernel, a green self-test, and an entry in
   `docs/linuxkpi-progress.md`.
