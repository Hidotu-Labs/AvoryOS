# LinuxKPI / DRM red-black tree rotation correction — 2026-10-15

A confirmed tree-balancing defect has been fixed in the native `rb_*`
implementation used by imported Linux DRM. This is a real correctness fix,
not another diagnostic-only change. Whether it eliminates the rare GPUVM fault
reported when opening GIMP remains unverified on AMD hardware.

## Defect and reachability

`kernel/src/lib/rbtree.c` supplies the plain exported tree functions, including
`rb_insert_color()` and `rb_erase()`, to both native and imported callers.
Its left/right rotation helpers changed colors as well as parent/child links:
the new subtree root inherited the old root's color, and the old root became
red. However, their insertion/deletion callers already implement a conventional
algorithm that explicitly recolors nodes before rotating. The helpers therefore
overrode the required recoloring. Inserting keys 0, 1, 2, 3, 4 through the
actual old implementation already violates the red-black invariants.

The correction makes both helpers change links and parent pointers while
preserving each node's color. Balancing remains in the existing callers.
Augmentation callbacks still run on the rotated nodes in bottom-up order.
No tree layout, comparison order, allocation policy or locking changes.

This dependency is directly reached by:

- `drm_mm.c`: `insert_hole_size()` calls `rb_insert_color_cached()` and removal
  calls `rb_erase_cached()`. The inline helpers call the native exports.
  AMDGPU's `amdgpu_gtt_mgr.c` and DRM's VMA-offset manager use `drm_mm`.
- `scheduler/sched_main.c`: the default FIFO runqueue calls `rb_add_cached()`
  and `rb_erase_cached()` through the same native exports.
- DRM PRIME and VMA per-file permission trees, which use plain insertion and
  erasure as well.
- The native EEVFD scheduler's augmented-tree API, which shares the corrected
  rotation helpers.

The imported augmented implementation in `linuxkpi/src/rbtree_aug.c` is a
separate implementation following the pinned Linux source. Its rotations were
not changed. Object-symbol inspection of the built `drm_mm.c.o` and
`sched_main.c.o` confirms their unresolved references to the native plain
`rb_insert_color`, `rb_erase` and `rb_next` exports.

Tree imbalance is proven. A specific incorrect GTT address, overlapping BO or
lost fence in the reported GIMP failure has not been traced to it; those are
possible consequences to investigate, not established facts about that fault.

## Regression coverage

`scripts/tests/test_kpi_rbtree.py` compiles the real native implementation in
one translation unit and actual pinned Linux tree layouts/inlines in another.
It executes the imported DRM FIFO update/remove functions and `drm_mm` hole-size
insertion, with only unrelated headers and already-held caller locks shimmed.
An independent oracle checks every mutation for membership, ordering, parent
links, cached minima, red adjacency and equal black heights. This covers
101,660 mutations, ascending/descending and equal keys, random remove/reinsert,
and root/two-child erasures. ASan/UBSan are enabled. The actual native scheduler
basic and augmented-tree self-tests pass too. Restoring the original rotation
behavior fails the regression.

The LinuxKPI phase-1 library boot suite now includes a 32-node native-bridge
guard. It checks every ordered insertion and permuted removal through the
imported Linux header view, so the short failure case is tested on every boot.
The host regression also compiles and runs this exact guard.

The selected GPU/LinuxKPI host suite passes **48/48** tests. The updated kernel
and `avoryos-x86_64.iso` build successfully. Evidence:

- `build/logs/gpuvm-kpi-audit-tests.log`
- `build/logs/gpuvm-kpi-rbtree-build.log`
- `build/gpuvm-kpi-rbtree-boot/kvm8-serial.log`

An eight-CPU private KVM guest, with no host GPU assigned and its own root disk,
passes the new bridge check, TTM rollback and DRM scheduler suites. Its native
syscall regression passes and it powers off normally. That boot also reports a
sysfs lifecycle PMM-accounting failure (46 pages); it is not counted as an
entirely clean boot suite. The expected absent SATA/ATA-controller messages
are separate from test failures.

A controlled baseline was linked from the same objects with only the original
rotation behavior restored, in a private kernel/ISO. It fails the new in-kernel
bridge guard, while the fixed kernel passes it. The baseline also reports the
sysfs accounting failure (59 pages), establishing that the latter is not
introduced by the rotation correction. Its log is
`build/gpuvm-kpi-rbtree-baseline/kvm8-serial.log`; the fixed boot reports 46
pages. These counts depend on the concurrent boot allocation environment and
do not identify the reason for the accounting failure. No sysfs test threshold
or unrelated code was changed to hide it. The baseline additionally reports
a semaphore test failure; the fixed boot passed it, but one comparison does
not establish whether that difference was caused by this correction.

Private boot disks, copied firmware/root trees, ISOs and the baseline kernel
were removed after completion; both serial logs and build/test evidence remain.

No passed-through GPU was started, stopped, reset or rebound for this work.
The main guest disk was not opened by the boot test. Use the rebuilt ISO on
the next normal hardware boot; the earlier opt-in fault capture remains
available if the symptom recurs.
