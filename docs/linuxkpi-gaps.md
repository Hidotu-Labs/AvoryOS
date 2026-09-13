# LinuxKPI gap log

Missing, stubbed, or deliberately divergent Linux APIs, as encountered while
compiling/running upstream code.  Every entry names the imported file that
needed it, the current workaround, and what a complete implementation needs.

Update this file in the same change that introduces or closes a gap.

## Phase 5 gaps (full I/O foundations)

### C1 — full Linux PCI API (non-IRQ)

- **Saved config state is standard config space only**
  (`linuxkpi/src/pci.c`): `pci_save_state()` snapshots the 16 dwords at
  0x00..0x3F and `pci_restore_state()` writes them back;
  `pci_store_saved_state()`/`pci_load_saved_state()` copy that software
  snapshot (upstream semantics: loading does not touch hardware; call
  `pci_restore_state()` to program it).  Upstream also saves PCIe/PCIx/LTR/
  DPC/AER/PTM/VC capability state, which amdgpu's reset path uses on real
  hardware; implementing that needs the capability save/restore machinery
  from drivers/pci/pci.c and the `pci_cap_saved_state` hlist.  Phase 6 reset
  testing will show whether the reduced form is enough under VFIO.
- **Expansion ROM BAR is sized and assigned on demand**
  (`linuxkpi/src/pci.c:pci_map_rom`): upstream maps the resource decoded at
  boot and calls `pci_assign_resource()` when unassigned.  AvoryOS sizes the
  ROM BAR with the standard all-ones write (like `pci_read_bases()`), then,
  when the readback is an unassigned all-ones pattern (OVMF leaves ROM BARs
  this way) or zero, assigns a 64K-aligned slot by scanning the low 32-bit
  MMIO hole (0x80000000..0xFEC00000) downwards against every decoded BAR.
  This is deliberately shim-local: there is no bridge-window/ACPI `_CRS`
  resource tree, so it cannot know the real windows.  Works on QEMU q35 and
  VFIO boots; real hardware with firmware-assigned ROM BARs takes the plain
  path.  No `IORESOURCE_ROM_SHADOW` and no PCIR length refinement.  An
  unassigned 64K BAR reads back `0xFFFF0000` after the sizing write, so the
  unassigned test compares against the sized readback, not a fixed mask.
- **`pci_wait_for_pending_transaction()` is a bounded 100 ms poll**
  (`linuxkpi/src/pci.c`): reads `PCI_EXP_DEVSTA.TRPND` every 1 ms, returns 1
  when clear, 0 on timeout; non-PCIe devices return 1.  Upstream uses the
  same semantics with a jiffies deadline.
- **`pci_release_resource()` has no resource tree to release from**
  (`linuxkpi/src/pci.c`): BAR resources are decoded snapshots with
  `parent == NULL`, so the function clears the resource and only calls
  `__release_region()` when a parent was recorded (none are today).
- **`pci_device_is_present()` reads the vendor ID directly**
  (`linuxkpi/src/pci.c`) instead of `pci_bus_read_dev_vendor_id()`; there is
  no `pci_bus` config accessor in the shim.
- **PCIe link helpers implemented from Link Capabilities 2 / Link
  Capabilities** (`linuxkpi/src/pci.c`): `pcie_get_speed_cap()`,
  `pcie_get_width_cap()`, `pcie_print_link_status()`.  They do not implement
  the "capable faster than current link" advice text.
- **`pci_revision_id`/`pci_address_name` were census false positives**: the
  amdgpu hits are a local variable around `pci_name()` and a `dc_types.h`
  struct field, not Linux PCI APIs.
- **`pci_p2pdma_distance()` comes from the stock inline stub**
  (`linux/pci-p2pdma.h`, `CONFIG_PCI_P2PDMA` unset → returns -1), which is
  what `amdgpu_dma_buf.c` checks for.  No implementation needed until real
  P2P support.
- **`pci_dev_get()`/`pci_dev_put()` remain documented no-ops** (carried from
  C4): wrappers live for the kernel's lifetime.

### C2 — IRQ core + MSI/MSI-X bridge

- **The descriptor spinlock is held while hard handlers run**
  (`linuxkpi/src/irq.c`): action nodes are allocated by
  `request_threaded_irq()` and freed by `free_irq()`, so dispatch serializes
  against unlink rather than using RCU/refcounts.  Consequence: a hard
  handler must not call `request_irq()`/`free_irq()`/`disable_irq()` on its
  own IRQ (deadlock).  amdgpu's IH handler only schedules work, so this is
  safe for P6; revisit if a driver needs more.
- **`IRQF_ONESHOT` does not mask in hardware**
  (`linuxkpi/src/irq.c`): `disable_irq()` runs the per-vector mask callback
  (MSI/MSI-X only); legacy INTx has no mask callback, so a disabled INTx
  action only drops interrupts in the dispatcher until the device is
  acknowledged.  Documented for drivers that rely on ONESHOT masking.
- **Threaded handlers use the system workqueue**
  (`linuxkpi/src/irq.c`): `request_threaded_irq()` queues `thread_fn` with
  `schedule_work()` instead of a dedicated per-IRQ kthread.  The thread
  function runs in normal process context (`in_interrupt()` false) and may
  sleep; it is not pinned to the interrupting CPU.  A dedicated IRQ-thread
  queue can replace this when the workqueue grows CPU affinity.
- **Legacy INTx uses IDT vector 32 + line as the Linux IRQ number**
  (`linuxkpi/src/irq.c`, `linuxkpi/src/pci.c`,
  `kernel/src/linuxkpi/native_irq.c`): the native `irq_install_handler()`
  routes legacy line N to vector 32 + N and runs the LinuxKPI trampoline
  there; MSI/MSI-X use the native vector (0x60..0xDF) directly.  One
  dispatcher reads `regs->int_no`, so no per-vector stubs are needed.
- **`pci_msi_vec_count()` returns 1**
  (`linuxkpi/src/pci.c`): the native MSI path enables exactly one vector.
  `pci_msix_vec_count()` decodes the real MSI-X table size (EDU has MSI only,
  so it returns 0).  `pci_alloc_irq_vectors()` allocates exactly `min_vecs`
  (it does not grow towards `max_vecs`), all vectors target the current CPU
  via `pci_irq_request_modes_routed()`, and `pci_alloc_irq_vectors_affinity()`
  ignores the affinity descriptor.  `pci_irq_get_affinity()` returns NULL.
- **`devm_request_irq()` is a devres action that calls `free_irq()`**
  (`linuxkpi/src/device.c`); `devm_free_irq()` walks the devres list and
  unregisters the matching action.  The fake-device test path relies on
  `device_initialize()` + `devres_release_all()`.

### C3 — ACPI table access + firmware loader

- **`CONFIG_ACPI` stays unset with the table-accessor subset only**
  (`linuxkpi/include/linux/acpi.h`, `linuxkpi/src/acpi.c`): the native ACPI
  layer parses MADT/FADT/MCFG/HPET and `acpi_get_table()` returns the first
  table with a matching 4-character signature via `acpi_find_table()`.
  `acpi_put_table()` is a no-op (tables live for the kernel's lifetime) and
  instance 0 and 1 both mean "the first table" (real callers such as amdgpu's
  VFCT lookup use 1; the native walker keeps no instance list).  There is no
  AML interpreter, no `acpi_evaluate_*`, no notify/hotplug: enabling ACPI and
  building `amdgpu_acpi.o`/`amdgpu_atpx_handler.o` remains Phase 8/bare metal.
  The audit confirmed 6.6 amdgpu's ACPI references are all gated by
  `CONFIG_ACPI` (`amdgpu_bios.c` VFCT, display brightness, kfd off), so a
  `CONFIG_ACPI=n` build is viable through P6.
- **The firmware loader moved to the Linux-API tree with
  `CONFIG_FW_LOADER=1`** (`linuxkpi/src/firmware.c`,
  `linuxkpi/include/generated/autoconf.h`): `<linux/firmware.h>` now declares
  the real externs instead of the `!CONFIG_FW_LOADER` inline stubs, and the
  implementation reads `/lib/firmware/<name>` through
  `asc_vfs_kernel_open/size/read/close`.  The old
  `kernel/src/linuxkpi/firmware.c` (legacy header, `struct firmware` without
  `priv`) is retired.  Synchronous `request_firmware()`/`release_firmware()`
  only: `request_firmware_nowait`/`_direct`/`_into_buf` and the cache/uevent
  paths are not implemented until a compiled driver needs them (6.6 amdgpu
  uses plain `request_firmware`).  Names are validated as before (no absolute
  paths, `.`/`..`, backslashes), size capped at 16 MiB, `priv` stays NULL.
- **The self-test blob is staged through the rootfs overlay**
  (`GNUmakefile`): `build/test_fw.bin` (4 KB, byte i = `(i*7+3)&0xff`) is
  copied to `build/alpine/rootfs/lib/firmware/test_fw.bin` before
  `populate-ext2-dir.sh` syncs the rootfs, so it lands in `/lib/firmware` in
  `disk.img`.  Rebuilding `disk.img` now requires the test blob; close any
  running VM before `make run*` rebuilds the image.  P7 replaces this with the
  linux-firmware manifest flow.

### C4 — minimal I2C core (DDC/EDID)

- **Self-authored minimal core, no `i2c-core-base.c` import**
  (`linuxkpi/src/i2c.c`): about 230 LOC behind
  `linuxkpi/include/linux/i2c.h`.  Scope is adapter registration plus master
  transfers; there are no i2c clients/instantiation, no OF/ACPI/fwnode
  adapter lookup, no `/dev/i2c-N`, no class/bus matching and no driver
  binding.  Drivers publish a `master_xfer` algorithm, call `i2c_transfer()`
  and nothing else is required for P6a.  Importing the stock core stays a P6
  contingency (phases plan §1 item 5).
- **`struct i2c_adapter` is the overlay type; `bus_lock` is a plain mutex**
  (`linuxkpi/include/linux/i2c.h`): upstream's `rt_mutex` plus
  `i2c_lock_operations`, `I2C_LOCK_SEGMENT`, mux and `locked_flags` are not
  modeled and `i2c_lock_bus()`/`i2c_trylock_bus()` do not exist.  Adapters
  are embedded in driver allocations as upstream expects; the core
  initializes `bus_lock` in `i2c_add_adapter()`.
- **Registry is a 64-slot array, not an idr** (`linuxkpi/src/i2c.c`):
  `i2c_add_adapter()` takes the lowest free bus number,
  `i2c_add_numbered_adapter()` fails `-EINVAL` when `adap->nr < 0` and
  `-EBUSY` when the number is taken; bus numbers are reused immediately
  after `i2c_del_adapter()`, and buses ≥ 64 fail with `-ENOSPC`.
- **No adapter reference counting** (`linuxkpi/src/i2c.c`):
  `i2c_put_adapter()` is a no-op and `i2c_get_adapter()` returns the
  registered pointer without `get_device()`.  Safe while adapters outlive
  their deregistration (the P5 model); revisit with hot-unplug.
- **`i2c_verify_adapter()` checks a real overlay `i2c_adapter_type`**
  (`linuxkpi/src/i2c.c`): `i2c_add_adapter()` stamps `adap->dev.type`, so a
  device pointer that is not an adapter returns NULL like upstream.
- **Retries re-run the whole transfer; `timeout` is ignored**
  (`linuxkpi/src/i2c.c`): `__i2c_transfer()` loops on `-EAGAIN` up to
  `adap->retries` extra attempts (negative retries clamp to 0);
  `adap->timeout` is carried for shape but never bounds the loop, and the
  6.6 per-message retry loop/jiffies deadline is not replicated.  The DRM
  EDID path adds its own 5-attempt loop on top anyway.
- **`adap->quirks` is carried but not enforced** (`linuxkpi/src/i2c.c`):
  `struct i2c_adapter_quirks` exists so driver initializers compile;
  `i2c_check_for_quirks()` (max messages, combined-message and length
  limits) is not implemented because no P5 consumer sets quirks.
- **No SMBus emulation** (`linuxkpi/include/linux/i2c.h`):
  `union i2c_smbus_data` is forward-declared only, `struct i2c_algorithm`
  carries `smbus_xfer` for driver initializers, and no `i2c_smbus_*` entry
  points exist.  amdgpu DM uses only `i2c_transfer`, so the census keeps
  them out of P5 scope.
- **`i2c_master_send`/`i2c_master_recv` are real functions, not the stock
  inlines** (`linuxkpi/src/i2c.c`): both route through
  `i2c_transfer_buffer_flags()` (one message, `count` bytes,
  `I2C_CLIENT_TEN` honored) and return `count` on success, matching upstream
  semantics.
- **The DRM EDID path is validated at message level**: the C4 test drives
  the exact `drm_do_probe_ddc_edid()` sequences (2 messages for the base
  block, 3 with the 0x30 segment write for block 2) against a synthetic EDID
  and checks the fetched block with the imported `drm_edid_block_valid()`.
  The connector-level `drm_edid_read_ddc()` plumbing stays P6e evidence.

### C5 — sysfs/devres/device/PM completion

- **`struct dev_pm_ops` is the 6.6 layout but there is no PM core**
  (`linuxkpi/include/linux/pm.h`): the overlay carries `pm_message_t`, the
  full callback set, `enum rpm_status`/`rpm_request`, the
  `SYSTEM/LATE/NOIRQ/RUNTIME_PM_OPS` field macros and `pm_ptr`/`pm_sleep_ptr`.
  With `CONFIG_PM`/`CONFIG_PM_SLEEP` unset, `SET_*_PM_OPS` expand to nothing
  (upstream's #else arms), so drivers publish a callback table nobody calls;
  stock `<linux/pm_runtime.h>` no-op inlines are what drivers actually link.
  Real runtime/system PM remains Phase 8.  The overlay intentionally replaces
  stock `pm.h`, which redefines `pm_message_t` and would fight the device
  overlay.
- **Dynamic sysfs nodes require a native directory handle**
  (`linuxkpi/src/kobject.c`): `kobj->sd` is the opaque `vfs_node_t` of the
  kobject's directory.  Class devices get one in `device_add()`; PCI wrappers
  resolve `/sys/bus/pci/devices/<bdf>` in `kpi_pci_dev_new()`.  A kobject
  without a handle accepts `sysfs_create_*` as a no-op instead of failing.
- **`sysfs_create_file()` assumes a device attribute**
  (`linuxkpi/src/kobject.c`): the bridge extracts `container_of(attr, struct
  device_attribute, attr)` and the owning device from the kobject, so plain
  `attribute`s backed by `kobj_type->sysfs_ops` are not supported (no
  in-tree consumer).  `sysfs_create_link()` stays a no-op: the native tree
  has no dynamic symlinks for Linux code.
- **Named groups become subdirectories; EEXIST is success**
  (`linuxkpi/src/kobject.c`): `grp->name` creates/uses a child directory and
  removal deletes the whole subtree (releasing contexts).  Re-adding an
  existing attribute is tolerated as success rather than `-EEXIST`.
- **Binary attributes are offset-aware; `mmap` is unsupported**
  (`linuxkpi/src/kobject.c`, `kernel/src/fs/sysfs.c`): reads/writes receive
  the file offset and bounds, so partial reads and EOF work.  A
  `bin_attribute.mmap` is never invoked (`mmap` returns `-ENOSYS` at the
  native file layer), which no P5 consumer needs.
- **Per-attribute contexts are released by the native removal path**
  (`kernel/src/fs/sysfs.c`): every dynamic record carries a release callback;
  `asc_sysfs_remove()`/`sysfs_remove_children()` free the record and context
  before unlinking.  While bringing this up, `sysfs_remove_children()` was
  found to reuse the shared static `dirent` returned by `ramfs_readdir()`
  across a recursive call, so `vfs_rmdir()` saw a clobbered name, failed, and
  the loop spun forever; the name is now copied before recursion.
- **Devres releases LIFO and devices unlink on unregister**
  (`linuxkpi/src/device.c`): `devres_release_all()` and
  `devres_release_group()` release in reverse registration order (the old
  FIFO order broke `devm` dependencies), and `device_unregister()` removes
  the device from `created_devices` so manual unregister + `kfree()` (tests)
  cannot leave dangling list nodes.
- **`get_device()`/`put_device()`/`kobject_get()`/`kobject_put()` are still
  no-ops** (`linuxkpi/src/device.c`, `linuxkpi/src/kobject.c`): there is no
  device reference count or release-on-last-put; devices live as long as
  their owner.  Unchanged from P3 and documented for the P6 compile.
- **PCI power helpers do bookkeeping only** (`linuxkpi/src/pci.c`):
  `pci_set_power_state()` records `current_state` and returns 0,
  `pci_choose_state()` answers `PCI_D3hot`, `pci_wake_from_d3()` is a no-op.
  No PMCSR writes, ASPM, or D-state transitions until Phase 8.
- **Driver `dev_groups` attach on bind, detach on unbind**
  (`linuxkpi/src/pci.c`): the direct probe registry calls
  `device_add_groups()` after a successful probe and
  `device_remove_groups()` before `remove()`, mirroring the driver core
  enough for amdgpu's `dev_groups`.
- **`CONFIG_HAS_IOMEM` is now set** (`linuxkpi/include/generated/autoconf.h`):
  x86 always selects it; this makes stock `<linux/platform_device.h>`
  declare `devm_platform_ioremap_resource()` extern (implemented in
  `linuxkpi/src/platform.c`) instead of the `-EINVAL` inline.
  `devm_ioremap_resource[_wc]()` rejects resources with `IORESOURCE_UNSET`
  and otherwise maps through `devm_ioremap[_wc]()`.
- **The boot self-test wait is 60 s** (`linuxkpi/src/boot_tests.c`): the
  growing suite list needed more than the old 30 s bound as a safety net;
  a normal headless boot completes the tests in roughly ten seconds.

### C6 — IRQs-on syscalls + context tracking

- **IRQs are enabled for the syscall dispatch from the entry code, not by
  clearing FMASK** (`kernel/src/syscalls/syscall_entry.asm`,
  `syscall.c`): the original sketch proposed masking only AC in
  `IA32_FMASK`, but an interrupt between the SYSCALL instruction and the
  kernel-stack/GS setup would run with kernel CS and user GS (or push its
  frame on the user RSP), so IF stays masked by FMASK.  The entry now runs
  `sti` once the kernel GS, stack and register frame are in place and the
  return paths already `cli` before swapgs/sysret.  `fork_return.asm` gained
  the missing `cli` before its swapgs/sysret; without it, IRQs-on syscalls
  let an ISR observe user GS in that window.  The scheduler punt
  (`sched_check_resched(true)`) stays as a deterministic reschedule point.
- **`might_sleep()` is an additive kernel.h overlay**
  (`linuxkpi/include/linux/kernel.h`, `linuxkpi/src/preempt.c`): an
  `#include_next` wrapper keeps stock `<linux/kernel.h>` (so all of its
  other definitions stay intact) and replaces the no-op `might_sleep()`
  with `__kpi_might_sleep()`.  It warns when `preempt_count() != 0`,
  `in_interrupt()`, or IRQs are masked; it is a WARN, never a BUG.
- **The warning is once per caller site, capped at 8 sites**
  (`linuxkpi/src/preempt.c`): stock `WARN_ONCE` would have a single
  once-flag at the `preempt.c` call site, so the ctx self-test's deliberate
  warning (one per boot) would silence a real violation reported later from
  a different file/line.  The table compares `__FILE__` pointers, so the
  ctx test consumes only its own site.  Every atomic call increments
  `kpi_might_sleep_warnings()` (the tests assert on it) even when the print
  is suppressed.
- **One `might_sleep()` WARNING per boot is expected evidence**
  (`kernel/src/tests/linuxkpi/linux/test_phase5_ctx.c`): the C6 suite
  deliberately calls `might_sleep()` from `preempt_disable()` and from an
  EDU MSI handler, so every boot logs exactly one
  `might_sleep() from atomic context (...)` line and the corresponding
  `WARNING: at linuxkpi/src/preempt.c:NN (imported WARN)`.  Any other WARN
  in a C6 boot is a finding.
- **The 1 h agent soak was waived by the maintainer (2026-09-13)**: C6's
  kernel-side regression is green (249 OK, no FAIL), but there is no
  hour-long userland stress evidence for this chunk; the 24 h protocol in
  the progress doc remains the user-run gate.

### C7 — VFIO hardening + Raphael validation

- **The Raphael VBIOS comes from VFCT, not a ROM BAR**
  (`scripts/vfio-vbios.sh`): on this APU `/sys/bus/pci/devices/<bdf>/rom`
  does not exist (no option ROM resource is assigned), so the original
  ROM-node extraction path cannot work.  The script now falls back to the
  system firmware's `VFCT` ACPI table (root-only), walks
  `uefi_acpi_vfct.vbiosimageoffset` → `vfct_image_header` entries (PCI
  bus/device/function, vendor/device, image length; Linux 6.6
  `atomfirmware.h` layout), extracts the image matching the BDF, verifies
  the `55 AA` signature and pads to the next power of two with `0xFF`.
  The padding matters: QEMU sizes the ROM BAR to a power of two, and C7's
  host/guest CRC32 comparison only matches if the file has the BAR size.
- **Gated VFIO validation boot** (`GNUmakefile`, `test_phase5_vfio.c`): the
  ISO can carry a limine `cmdline:` (`KERNEL_CMDLINE=...`), and
  `kpi_vfio_test` (module parameter, default 0) makes the validation suite
  run only when explicitly requested.  A normal P6a boot therefore never
  has the test touch the passed-through GPU.  `run-vfio` also gained
  `SERIAL` (headless captures), `VFIO_EXTRA` (e.g. `-device edu`) and uses
  the shared `DISPLAY_OPT`, so `DISPLAY_OPT='-display none'` is headless
  while the default opens a window.
- **The default std VGA changes the P4 bochs canary outcome**: run-vfio
  without `-vga none` leaves QEMU's std VGA (also `1234:1111`, a bochs VGA)
  in the guest, so the bochs suite runs on it and fails the BAR0 pattern
  readback (the canary was written for `-device bochs-display`).  Adding
  `-vga none -device virtio-vga` makes the suite skip and gives the C1 ROM
  test its virtio-vga device.  Environmental, not a driver regression.
- **fastfetch's GPU line is native now** (`GNUmakefile`): the image used to
  bake in a custom `VirtIO-GPU ... / Mesa llvmpipe` string.  It now uses
  fastfetch's `"gpu"` module (PCI ID scan), which lists AMD Raphael as soon
  as the passed-through device is enumerated and keeps working when amdgpu
  binds in P6.  Renderer details (llvmpipe vs radeonsi) are no longer part
  of the line.

## Phase 4 C5 gaps (bochs TTM canary, 2026-09-13)

- **`page_to_phys()` must not walk `compound_head()`** (fixed in
  `linuxkpi/src/page.c`): TTM's pool allocates high-order blocks **without**
  `__GFP_COMP` and treats every page independently, but `alloc_pages()` used
  to mark every order-N allocation compound.  `page_to_phys()` then resolved
  every page of a split block to the block head's pfn, so `vmap()` aliased all
  511 pages of a 2 MB block onto one physical page (observed as a 0x1FF000
  pattern shift through bochs' BAR0).  `page_to_phys()` now returns
  `page->pfn << PAGE_SHIFT` (upstream semantics) and `alloc_pages()` only
  builds compound bookkeeping when `__GFP_COMP` is set.  The Phase 2 page
  test asserts both compound (`__GFP_COMP`) and plain high-order behavior.
- **bochs is a simple-pipe driver** (`drivers/gpu/drm/tiny/bochs.c`):
  `drm_simple_kms_plane_atomic_check()` uses `can_position=false`, so the
  primary plane's destination must cover the **entire CRTC**.  The kernel and
  userland tests therefore size the modeset dumb buffer to the committed mode
  and use full-CRTC rectangles; a 64x64 buffer is still used for non-commit
  GEM loops.
- **bochs has no vblank engine**: `drm_dev_has_vblank()` is false, so the
  atomic enable uses `DRM_MODE_ATOMIC_ALLOW_MODESET` only and both tests treat
  a missing `DRM_EVENT_FLIP_COMPLETE` as an explicit skip rather than a
  failure (no `WAIT_VBLANK` use).
- **`inb_p()`/`outb_p()`** (`linuxkpi/include/asm/io.h`): `<video/vga.h>`
  uses the "slow" ISA I/O variants; the overlay now aliases them to the plain
  HAL accesses (no `slow_down_io()` delay exists).  Same for `inw_p`/`inl_p`/
  `outw_p`/`outl_p`.
- **Minimal power-management types** (`linuxkpi/include/linux/device.h`):
  bochs publishes `.driver.pm`, so the overlay gained an empty
  `struct dev_pm_ops`, `SET_SYSTEM/LATE/NOIRQ_SYSTEM/RUNTIME_PM_OPS` (all
  compile to nothing with `CONFIG_PM_SLEEP` unset) and `device_driver.pm`.
  Stock `<linux/pm.h>` is not included because it redefines `pm_message_t`
  (same conflict class as `<linux/device/bus.h>`).
- **`video_firmware_drivers_only()` is a weak no-op**
  (`linuxkpi/src/drm_compat.c`): `drm_module_*_driver_if_modeset()` calls it;
  AvoryOS always allows the modeset driver.  Weak so the imported
  `drivers/video/aperture.c` wins if it is ever compiled.
- **modetest cross-check**: Alpine's `libdrm-tests` package (provides
  `modetest`) is now installed into the Alpine rootfs by
  `scripts/setup-alpine.sh`; the primary C5 evidence remains the kernel suite
  plus `bin/test_kpi_bochs` (same fallback P3 used for vkms).

## Phase 4 gaps (TTM + scheduler + minimal PCI, 2026-09-12)

### C4 — minimal Linux PCI API

- **Native PCI symbols renamed into the `asc_pci_*` namespace**
  (`kernel/src/drivers/pci/pci.{c,h}` and all native call sites): stock
  `<linux/pci.h>` declares the global `pci_bus_type`, plus
  `pci_get_device(vendor, device, from)` and `pci_find_capability(dev, cap)`,
  which collided with the native `pci_bus_type()` bus getter, the index-based
  `pci_get_device(i)` and the native-type `pci_find_capability(dev, cap)` as
  soon as `linuxkpi/src/pci.c` defined the Linux versions.  They are now
  `asc_pci_bus_type()`, `asc_pci_get_device(idx)` and
  `asc_pci_find_capability(dev, cap)`; `pci_find_device()` and
  `pci_get_device_count()` have no stock counterpart and keep their names.
- **`struct bus_type` is a minimal overlay type**
  (`linuxkpi/include/linux/device.h`): only `name` exists.  Upstream's
  `<linux/device/bus.h>` cannot be included because it redefines
  `pm_message_t`, which the device.h overlay already provides.  Nothing built
  today includes bus.h; a future import that does must reconcile the two.
- **PCI `struct pci_dev` wrappers live for the kernel's lifetime**
  (`linuxkpi/src/pci.c`): one wrapper per native `pci_device`, built before
  initcalls by `linuxkpi_pci_scan()` (called from the `kpi/initcalls` thread
  path) from `linuxkpi/native_pci.h` snapshots.  There is no kobject
  refcounting, so `pci_dev_get()`/`pci_dev_put()` are no-ops and
  `pci_get_device()`-style lookups never drop references; `dev.release` is a
  no-op placeholder.  `pci_get_domain_bus_and_slot()` only matches domain 0.
- **PCI driver binding is a direct registry** (`linuxkpi/src/pci.c`):
  `pci_register_driver()` appends to a list and probes every unbound matching
  wrapper synchronously through the id table (vendor/device/subvendor/
  subdevice/class/class_mask); `pci_unregister_driver()` calls `.remove`.
  No generic driver core, no deferred probe, no `pci_add_dynid()`, no
  OF/ACPI matching; `dev.driver`/`driver.bus` are assigned by hand.
- **Region requests are a conflict registry** (`linuxkpi/src/pci.c`):
  `__request_region()`/`__release_region()` check overlap over a
  singly-linked child list of `struct resource` and allocate/free the child
  record.  Native port I/O is raw `in`/`out`, so a successful request is
  bookkeeping only.  Managed (`pcim_enable_device()`) devices register a
  devres action and release is idempotent.  `ioport_resource` (0..0xFFFF) is
  defined here; `iomem_resource` still comes from `link_stubs.c`.  The
  earlier weak `__devm_request_region()` stub in `link_stubs.c` was replaced
  by the real implementation.
- **`pci_iomap()` returns raw port addresses for I/O BARs**
  (`linuxkpi/src/pci.c`): MMIO BARs go through `ioremap()`/`ioremap_wc()`;
  I/O BARs return `(void __iomem *)start` because there is no ioport_map, and
  `pci_iounmap()` skips addresses <= 0xFFFF.  The declarations come from
  `<asm-generic/pci_iomap.h>`, now included by the `<linux/io.h>` overlay
  (the asm/io.h overlay drops `asm-generic/io.h`, through which upstream
  reaches them).
- **PCI MSI/MSI-X is not implemented** (C4 scope): `CONFIG_PCI_MSI` is set so
  `struct pci_dev` has the MSI fields and `pci_dev_msi_enabled()` compiles,
  but `pci_alloc_irq_vectors()` and friends have no implementation; EDU's MSI
  capability is left untouched by the test.  Drivers needing MSI must wait
  for the Phase 5 interrupt work.
- **ROM mapping is minimal** (`linuxkpi/src/pci.c`): `pci_enable_rom()`/
  `pci_disable_rom()` toggle the ROM BAR enable bit, but `pci_map_rom()` only
  maps a resource decoded at scan time and scan decodes the six standard BARs
  only, so it reports size 0.  A real implementation needs the BAR
  save/size/restore sequence.
- **`drm_aperture_remove_conflicting_pci_framebuffers()` is a weak no-op**
  (`linuxkpi/src/drm_compat.c`): bochs calls it in probe and AvoryOS has no
  framebuffer hand-over registry (`drm_aperture.c` is not imported).  Weak so
  the imported implementation wins later without edits.


- **`current` is the `task_struct` shadow everywhere**
  (`linuxkpi/include/asm/current.h`): the overlay used to cast the native
  `struct thread *` to `task_struct *`, while `sched.h` returned the shadow.
  Imported code that dereferences `current->…` (`drm_sched`'s
  `last_user`/`group_leader`) and dma-fence's `cb.task = current` therefore
  operated on a native thread (whose first field is `rsp`), so
  `wake_up_state()` woke a garbage pointer and every fence wait timed out.
  Both definitions now return `linuxkpi_current_task()`.
- **`timer_setup()` memsets the timer** (`linuxkpi/include/linux/timer.h`):
  the overlay did not initialize `running`, so an `INIT_DELAYED_WORK()` on
  stack/heap memory that was not already zero (e.g. `drm_sched_init()` on a
  stack `drm_gpu_scheduler`) made `del_timer_sync()`/`cancel_delayed_work_sync()`
  wait forever on a stale `running=1`.  Stock `__init_timer()` memsets; the
  overlay now does too.
- **`schedule_timeout()` has a pending-wake rendezvous**
  (`linux/src/linuxkpi/native_sched.c`, `kpi_wake_pending`/`kpi_timeout_active`
  in `kernel/src/sched/sched.h`): `sched_wakeup()` ignores running threads, so
  a wake that arrived after the waiter committed to sleeping but before it
  set `THREAD_SLEEPING` was lost and `schedule_timeout()` returned 0 at its
  deadline.  `linuxkpi_wake_thread()` now arms a per-thread pending flag while
  a KPI timeout is active; the sleeper consumes it under the run-queue lock.
  Waitqueue and kthread-start wakes are deliberately excluded (arming them
  made every later `msleep` return early).
- **`struct rb_node` layout matched to stock Linux** (`kernel/src/lib/rbtree.h`,
  C3): the native struct had `rb_left` before `rb_right` while stock
  `<linux/rbtree_types.h>` has `rb_right` first.  Imported code embeds the
  stock layout but calls native `rb_insert_color`/`rb_next`/`rb_first`, so
  every imported rbtree silently corrupted itself.  The first real user was
  drm_sched's FIFO run queue (`rb_add_cached` in `drm_sched_rq_update_fifo`):
  entities were never selected, so jobs sat unsignaled.  Lesson for future
  imports: any type whose operations are shared between native and imported
  code must have an identical layout; putting the operations in a renamed
  native namespace (`asc_*`, as done for radix-tree) is the alternative.
- **Imported `WARN()`/`BUG()` are decoded from `__bug_table`**
  (`kernel/src/cpu/bug_table.c`, hook in `kernel/src/cpu/isr.c`): stock
  `<asm/bug.h>` emits `ud2` plus a table entry; the native invalid-opcode
  handler had no decoder, so any imported WARN panicked.  WARNs now print
  `WARNING: at file:line (imported WARN)` and resume at RIP+2;
  `BUG()`/genuine bad opcodes still panic.  This required
  `CONFIG_DEBUG_BUGVERBOSE` in `autoconf.h` (12-byte entries with file:line;
  without it entries are 8 bytes and the decoder would not match).
  `BUGFLAG_ONCE` is honoured with a 64-entry address registry; the table
  lives in `.rodata`, which is not written to.
- **`ww_mutex_trylock()` returns 1/0, not 0/-EBUSY** (see the corrected
  Phase 3 entry above).  Found by the first C2 boot: TTM's
  `WARN_ON(!dma_resv_trylock())` fired on every successful trylock.
- **Page cache-mode API is inert** (`linuxkpi/src/x86_stubs.c`): TTM calls
  `set_pages_wb()`, `set_pages_array_uc()`, `set_pages_array_wc()`,
  `set_pages_array_wb()`, `cachemode2protval()` and `pgprot_writecombine()`.
  All RAM is HHDM write-back and AvoryOS has no PAT/MTRR programming, so the
  cache flips are accepted and ignored; `cachemode2protval()` returns the
  correct x86 PAT bits (so the pgprot values are well formed) but the kmap
  paths that consume them are plain HHDM lookups.  `clear_page_orig/rep/erms`
  are `memset()` over the HHDM (`clear_page()` in stock x86 `page_64.h`
  references them through the alternatives machinery).
- **`struct page::private` is `unsigned long`**, matching upstream (it was
  `void *` through Phase 3).  TTM stores an allocation order and a helper
  pointer there; only `page.c` wrote the field before, and it now writes 0.
- **`<linux/highmem.h>` includes `<linux/mm.h>`** again, as upstream does.
  The Phase 2 overlay had the dependency reversed, which hid `PFN_UP`,
  `struct shrinker`, `fault_flag_allow_retry_first()` and friends from TUs
  such as TTM's `ttm_pool.c`.  `mm.h` now also pulls `<linux/pfn.h>`,
  `<linux/shrinker.h>` and `<linux/mmap_lock.h>` like upstream.
- **`<linux/mmap_lock.h>` is a no-op overlay**: `mmap_read_lock/unlock`,
  `mmap_write_*`, trylock and assert variants are inert inlines.  AvoryOS
  keeps VMA state under native locks and nothing walks a Linux mm; TTM's
  fault path uses `mmap_read_unlock()` around `dma_resv` waits.
- **`pagefault_disable/enable()` and `migrate_disable/enable()` are no-ops**
  (`linuxkpi/include/linux/{uaccess,preempt}.h`): there is no highmem and no
  page migration, and the stock `<linux/io-mapping.h>` inlines call them.
- **`drain_workqueue()` equals `flush_workqueue()`**
  (`linuxkpi/src/workqueue.c`): upstream's "draining" state that blocks new
  submissions does not exist; the call waits for pending/running work only.
- **`struct mm_struct` is still a placeholder** but now carries `pgd`,
  `init_mm` is a zeroed instance, and `swp_entry_t`/`enum fault_flag` are
  defined in the `mm_types.h` overlay because stock `<linux/pgtable.h>` needs
  them for its inline helpers.  No imported code walks an mm.
- **`task_struct::group_leader` is the task itself and `exit_code` stays 0**
  (`linuxkpi/src/task.c`): there is no shared `signal_struct`, so per-process
  identity is approximated by per-thread identity.  `drm_sched` uses it only
  for its per-user submission bookkeeping and compares it against itself;
  `SIGKILL` now comes from `<uapi/linux/signal.h>` through the `sched.h`
  overlay.
- **`shmem_read_mapping_page_gfp()` implemented** in `linuxkpi/src/shmem.c`
  (page-returning wrapper over the existing folio store); TTM's
  shmem-backed `ttm_tt` path uses it.  The `shmem_fs.h` overlay now declares
  it next to the folio form.

## Deliberate divergences

- **`struct page`** (`linuxkpi/include/linux/mm_types.h`): trimmed layout with
  an extra `pfn` field; no fields/flags area, no folio embedding.  `PageTail`
  is an AvoryOS-only flag bit backed by a plain `compound_head` pointer.
  `linuxkpi/include/linux/page-flags.h` is a compact overlay, not upstream's.
- **`asm-generic/memory_model.h`** is shadowed with an empty file:
  `pfn_to_page()`/`page_to_pfn()` are real functions over AvoryOS's sparse
  mem_map instead of the FLATMEM/SPARSEMEM macros.
- **`struct page::_refcount` is authoritative** for pages handed to Linux
  code; the native PMM reference is dropped exactly once when it reaches zero.
  Native CoW/refcount paths must not touch such pages (documented in page.c).
- **`struct vm_area_struct`** is a Linux-facing view allocated by the mmap
  bridge; the native `struct vma` remains the authoritative mapping record.

## Configuration gaps

- `CONFIG_PHYS_ADDR_T_64BIT` / `CONFIG_ARCH_DMA_ADDR_T_64BIT` were missing
  from `linuxkpi/include/generated/autoconf.h`.  Without them
  `<linux/types.h>` typedefs `phys_addr_t`/`dma_addr_t` as u32, and GCC
  compiled `page_to_phys()` with a 32-bit shift that truncated every
  physical address above 4 GB.  Fixed in Phase 2 (found by the page
  roundtrip self-test).
- `CONFIG_DYNAMIC_MEMORY_LAYOUT` added so upstream `__pa()`/`__va()` use
  `page_offset_base`, which AvoryOS pins to the runtime HHDM offset.

## Phase 3 gaps (DRM core bring-up, 2026-09-12)

- **shmem backing is a shim, not tmpfs** (`linuxkpi/src/shmem.c`):
  `shmem_file_setup()` returns a bridged kernel file whose address_space is an
  xarray of zeroed order-0 PMM pages; `shmem_read_folio_gfp()`,
  `shmem_truncate_range()` and `invalidate_mapping_pages()` operate on that.
  No swap, no reclaim, no `shmem_read_mapping_page_gfp` page-cache semantics
  beyond allocate/lookup/free.
- **`struct folio` is order-0 only** (`linuxkpi/include/linux/mm_types.h`,
  `page-flags.h`, `pagevec.h`): a folio wraps one page; `folio_batch` is the
  upstream layout with a no-op unevictable move.
- **kobject/sysfs is dynamic for class devices** (`linuxkpi/src/kobject.c` +
  `kernel/src/fs/sysfs.c`): `class_create()` creates `/sys/class/<name>`;
  `device_add()`/`device_del()` create/remove
  `/sys/class/<class>/<dev-name>`; `device_add_groups()` materializes
  `struct device_attribute` files with real show/store (and honours
  `is_visible`); `class_create_file()` materializes class attributes (DRM's
  `version`).  Still inert: `sysfs_create_file()` on a non-device kobject
  (needs `ktype->sysfs_ops`), `bin_attribute`s (EDID), symlink creation and
  uevent broadcast.  `device_add()` also wires `kobj.parent`/`kobj.name` for
  device-hierarchy reads.  The per-attribute show/store contexts are not
  freed on removal.
- **Dynamic devnode registry is a fixed table** (`linuxkpi/src/native_vfs.c`,
  `KPI_DEVNODE_REGISTRY_MAX 32`): nested devnodes below /dev are matched by
  dir/leaf name, enumerated by the owning directory's readdir, and removed by
  `device_del()`; `device_add()`'s devnode hook registers DRM minors.  No
  refcounting beyond the metadata node's persistent reference; each open
  descriptor owns its own per-open node.
- **Platform bus is synchronous and minimal** (`linuxkpi/src/platform.c`):
  name-match only, no deferred probe, no OF/ACPI enumeration, no refcounts.
- **`register_chrdev()` is a successful no-op** (`drm_compat.c`): the native
  devfs owns /dev nodes; DRM major 226 has no registry.  `file_clone_open()`
  returns -ENODEV.
- **`struct task_struct` is a per-thread shadow** (`linuxkpi/src/task.c`) with
  `kpi_thread` first; `task_tgid()` returns an opaque token built from the
  native tgid, not a `struct pid`.  Shadows are leaked with their thread until
  a thread-exit hook exists.
- **Initcalls run in a kthread with a bounded wait**
  (`linuxkpi/src/initcalls.c`): the walker itself stays native
  (`kernel/src/linuxkpi/init.c`), but it is invoked from a `kpi/initcalls`
  thread like Linux's `kernel_init`, capped at 30 s.  A wedged initcall is
  reported and boot continues, so a partially initialized driver must still
  fail safely at use time.
- **i2c/regulator/component/panel-quirk/aperture stubs** (`drm_compat.c`,
  `link_stubs.c`): EDID DDC returns -EIO, regulators are absent, component
  add/del are no-ops, panel orientation quirk is UNKNOWN.  `_printk` now
  routes imported messages through `vklogf` (512-byte line buffer);
  `kvasprintf` truncates at 512 bytes.
- **`ksize()` is real** (`linuxkpi/src/slab.c` → `heap_ksize()` in
  `kernel/src/mm/heap.c`): slab allocations report their cache object size and
  big allocations their page-payload capacity, so DRM's
  `drmm_add_final_kfree()` capacity check can pass.  The exact requested size
  is still not tracked, only the usable capacity (same contract as upstream).
- **`__sw_hweight32/64` are hand-written asm**
  (`kernel/src/arch/x86_64/hweight.asm`): imported x86 `hweight*()` emits a
  bare `call __sw_hweight*` with an empty clobber list and relies on the
  upstream register-preserving convention; a C implementation silently
  corrupts live caller registers.
- **ww_mutex cannot wound** (`linuxkpi/src/ww_mutex.c`): recursive
  acquisition with the same acquire context returns `-EALREADY` (as
  upstream, which DRM's `modeset_lock()` treats as success), but multi-lock
  acquire sequences still cannot back off under contention; they rely on the
  plain mutex FIFO.  **Correction (C2, 2026-09-12):** `ww_mutex_trylock()`
  must return `1` on success and `0` when busy, not `0`/`-EBUSY`: upstream
  `kernel/locking/mutex.c` and the bool casts in `<linux/dma-resv.h>` /
  `drm_modeset_lock.c` all rely on the truthy convention.  The Phase 3
  "0/-EBUSY convention" note was wrong and caused
  `WARN_ON(!dma_resv_trylock())` to fire in TTM's BO init.
- **rbtree augmented internals are an excerpt** (`linuxkpi/src/rbtree_aug.c`,
  verbatim upstream `lib/rbtree.c`): native `kernel/src/lib/rbtree.c` still
  owns the base `rb_*` API; importing upstream `lib/rbtree.c` requires
  renaming the native symbols first.

## Phase 2 gaps

- **File/VFS bridge is single-level and shim-shaped** (`linuxkpi/src/file.c` +
  `kernel/src/linuxkpi/native_vfs.c`).  `struct file` owns a native `vfs_node_t`
  (`f_asc_node`) and each node's `device` points back at the file.  There is no
  inode/dentry cache, no mount tree and no fs open/release dispatch beyond the
  pseudo-fs path dma-buf uses (`alloc_file_pseudo` + dentry `d_release`).
  `fget()` resolves a descriptor through the native fd table, so only fds
  installed by `fd_install()` are visible; kernel-internal files without an fd
  are not findable.  fd-owned nodes are released by the vfs when the descriptor
  closes; kernel-internal files (shmem backing files, etc.) release their node
  from `kpi_file_free()` (`asc_vfs_node_release_kernel()`), and
  `linuxkpi_file_close()` clears `f_asc_node` before `fput()` so the fd path
  cannot release it twice.  Drivers that expect `fget_raw`/`fdget` to see
  kernel fds or that keep a `struct file` beyond its node's lifetime need this
  to grow.
- **`poll` bridging wakes event-driven consumers** (`linuxkpi_file_poll`):
  the poll table's qproc records the file's native wait queue in the Linux
  `wait_queue_head`, and `__kpi_wake_up()` calls `linuxkpi_wake_poll_queue()`
  so a device's `poll_wait()`/`wake_up()` pair reaches `sys_poll()` waiters.
  Limits: one native queue is recorded per `wait_queue_head` (the last poller
  wins if several files poll the same head), and a head can retain a stale
  queue pointer if its file closes while another file still polls that head.
  The Phase 3 DRM bridge is the first user; the vkms `PAGE_FLIP_EVENT` path
  exercises the wake end to end (atomic commit -> `drm_poll()` POLLIN ->
  `drm_read()`).
- **`unmap_mapping_range()` is a no-op stub** (`linuxkpi/src/mmap.c`).  It
  ignores the address_space because the native VMA tree, not the page cache,
  owns PTE teardown.  TTM's `ttm_bo_vm` will call it when buffers are
  invalidated; until it walks every process mapping, userspace can keep
  touching stale BOs after a move.  `struct inode` now carries an `i_mapping`
  (`alloc_anon_inode()` allocates an address_space), but the mapping walk
  itself is still unimplemented.
- **`mmu_notifier` is a type-only overlay**
  (`linuxkpi/include/linux/mmu_notifier.h`): `CONFIG_MMU_NOTIFIER` is not set
  in `autoconf.h`, so imported code takes the `#else` arms.  dma-resv.c only
  includes the header for `struct mmu_notifier_range`; TTM (v6.6) does not use
  the API.  HMM/userptr-style users will need real register/release hooks.
- **VM split/remap paths drop the Linux wrapper** (`kernel/src/mm/vma.c`):
  `vma_mprotect()`, `sys_mremap` and the grows-down path re-add native VMAs
  with plain `vma_add()`, so only the sys_mmap/teardown/clone paths call
  `vma_attach_linux()`.  A dma-buf mapping that is mprotected or mremapped can
  therefore lose its `vm_ops` (close fires via the old wrapper reference while
  the new VMA has none).  Phase 6/7 (amdgpu VM) must route every re-add through
  `vma_attach_linux()` to preserve exactly-once close semantics.
- **Shrinker API is inert** (`linuxkpi/src/shrinker.c`): registration succeeds
  and the callback is never invoked, so TTM's pool never proactively evicts
  under memory pressure.  Reclaim currently has no shrinker caller at all; the
  native PMM does not run the Linux reclaimer.
- **`vm_ops->fault()` bridge installs PTEs on demand** but only for VMAs created
  through the Linux mmap bridge; `vmf_insert_page()` uses the native PMM's
  mapping flags and takes a page reference, while `remap_pfn_range()` does not
  (pfn mappings stay the exporter's responsibility).  page_mkwrite/COW and
  `VM_PFNMAP`-style tracking (`vm_normal_page`) are not modeled.
