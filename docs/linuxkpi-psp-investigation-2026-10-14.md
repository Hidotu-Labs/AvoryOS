# PSP investigation and native rollback validation

The real PSP ring failure and host reboot are **not established fixed**.
The current corrections fix independently demonstrated compatibility and
cleanup defects. Diagnostics and simulated responses are not hardware success.
Imported AMDGPU/TTM sources remain unchanged.

## Confirmed corrections

- Managed resource ordering: `devres_alloc()` resources and managed actions
  formerly occupied different lists. Mixed registration released actions first,
  then raw resources in forward order. The native regression recorded three
  failures for mixed order, nested group bounds, and resources outside groups.
  [device.c](/home/offihito/AvoryOS/kernel/linuxkpi/src/device.c) now uses one
  device-owned list, an IRQ-safe device lock, and detached reverse-order release.
  Callbacks execute outside the lock. Groups have distinct IDs and actual closing
  boundaries; nested and crossing boundaries are preserved. Action removal finds
  the latest match. Allocated resource data is zeroed and size overflow rejected.
  PCI wrapper construction initializes the device's list and lock together.
  These are real lifetime defects; their connection to the original GPU fault
  is unproven. Contract comparison:
  [Linux 6.6 devres](https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/base/devres.c).
- Sysfs lifecycle: dynamic removal retained owned VFS lookup references, and
  directories constructed through `ramfs_mount_on()` lacked a payload destructor.
  The native 64-cycle test previously lost about 133 pages. Lookup references are
  balanced in removal/existence paths; existing directory handles follow the
  borrowed-tree-handle contract; mounted directories register their destructor.
  The unchanged two-page PMM tolerance now passes on the 1-CPU and 8-CPU guests.
  The PMM failure path also increments the suite failure counter instead of
  printing a failure followed by a successful suite summary.
- Fault reporting: an unguarded kernel access to a low address formerly printed
  one line and terminated its thread. A synchronous register/stack record now
  precedes ordinary lock-taking logging. It does not change thread termination
  or consume the independent fatal-panic report guard. The old ring-0 stack
  reconstruction pointed inside the interrupt frame. In a deliberate fake-TTM
  manager-removal control, GDB captured RSP `...7E48`, while the old reporter used
  `...7E30`. All exception reports now use the CPU-saved RSP. This matches the
  [AMD64 system programming manual](https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content):
  64-bit interrupts save SS:RSP even without a privilege change. Stack scan values
  remain candidate return addresses, not a guaranteed unwind.
- The emulated PCI power test expected the old fabricated D3 transition. It now
  checks that unsupported transitions/wake requests are rejected and the observed
  state is preserved. Hardware PM support was not added or faked to make it pass.

## Real native VM tests

[ttm-rollback-boot.py](/home/offihito/AvoryOS/scripts/ttm-rollback-boot.py) builds a
private root disk/ISO and passes no physical devices. It stages the manifest's
firmware files and boots an emulated EDU device for actual PCI/IRQ tests.

- 1-CPU and 8-CPU KVM runs passed all enabled native boot suites with no `[FAIL]`
  records, reached the userland regression, and powered off normally.
- The TTM rollback fixture uses actual imported TTM objects/managers/workers.
  Failures after 1..4 pinned allocations unwind buffers while managers remain
  alive. Holding a reservation forces real delayed-delete work; the fixture
  verifies destruction counts, resource usage and the pinned list after draining.
  This tests a lifetime requirement, not the PSP firmware protocol or the actual
  original AMDGPU caller.
- A separate disposable CPU guest uses GDB to deliberately remove a fake SYSTEM
  manager and set BO priority 2. This produces CR2 `0x60`, RSI zero, RAX `0x20`
  at the manager-LRU instruction. The report includes registers and stack
  candidates, and its interrupted RSP matches GDB. The control confirms this
  fault mechanism; it does not prove why the real GPU lost its manager.
- [44 CPU regression scripts](linuxkpi-psp-protocol-tests-2026-10-14.txt) passed.
  The new devres test executes the real compatibility implementation under
  ASan/UBSan, including 8,000 concurrent raw/action releases, group boundaries,
  allocation failures, isolation and callback registration during teardown.
- The initial minimal VM lacked staged firmware and correctly reported missing
  files. Staging the actual 11 manifest files makes their size/CRC tests pass.
  Missing-firmware failures were fixture deficiencies, not a PSP-ring diagnosis.

Native serial evidence is in `build/ttm-rollback-boot/kvm{1,8}-serial.log`; the
negative control is in `build/ttm-fault-control/kvm8-serial.log`. Build artifacts
and raw hardware evidence are ignored, not committed.

## PSP evidence boundaries

Raphael PSP 13.0.5 selects `psp_v13_0_funcs` in the pinned import, rather than
the distinct 13.0.4 function table. The supplied original log reports ring-create
failure without the preceding "trust OS ready" error. That is consistent with
failure after submitting the ring command, but the excerpt lacks the mailbox
value and is insufficient to distinguish timeout from firmware rejection.

[amdgpu_trace.c](/home/offihito/AvoryOS/kernel/linuxkpi/src/amdgpu_trace.c) records
ring-command entry/exit, GPU address, ring size, timeout and original result.
A scoped forced-include macro forwards the original family callback without
editing the import. Failed family waits record register, mask, expected value,
original return, and one read taken after failure. There are no extra successful
wait reads, command writes, retries or firmware-state changes in these wrappers.

The CPU protocol regression executes the pinned `psp_wait_for()`, 13.x ring
create/stop functions and compatibility wrappers on fake MMIO. It verifies
40-bit address programming/write order, delayed response, missing SOS, rejected
and stuck commands, and SR-IOV stop/create. This passing simulation is **not**
evidence that the real PSP accepts the command.

## Host reboot and retained launch evidence

The user provided privileged host logs and confirmed the 19:05 Istanbul restart
was unexpected during AMDGPU initialization. The previous journal ends at 19:04;
the current boot starts at 19:05:44. The last retained VFIO reset/reset-done pair
is at 18:29:15, not immediately before the reboot. No panic trace or IOMMU fault
was found in the inspected saved kernel log, and pstore was empty. This does not
exclude an unpersisted fault, firmware/platform reset, or VFIO involvement.
The CF9/ACPI reset-reason messages do not identify the responsible actor.

The GPU remains vfio-pci, group 24, with bus-only reset availability. Host audio,
CCP and USB functions retain their own drivers under the shared `00:08.1` bridge.
No host binding, reset method, bridge or power setting was changed.

The latest original prime serial file was zero bytes, modified at 19:04:45.
QEMU's default log opening truncates its prior contents. `run-c7-prime` now sets
`logappend=on` and uses
[qemu-prime-log.py](/home/offihito/AvoryOS/scripts/qemu-prime-log.py). A private
per-launch directory records the host boot ID, ISO digest, serial bytes and
QEMU stderr. The launch marker and metadata are flushed before device opening;
captured bytes are periodically flushed. This improves persistence but cannot
guarantee retaining the last instruction before an abrupt platform reset.
The wrapper preserves child exit status, forwards requested termination, rejects
concurrent launches against the same log, and never retries or issues resets.
Its CPU-process regression and a real GPU-free QEMU boot passed.

Privileged host evidence was copied to the protected ignored directory
`build/gpu-corruption-audit/host-reset-2026-10-14/host-evidence-1905`.

## Physical probe preparation and first attempt

[amdgpu-psp-boot.py](/home/offihito/AvoryOS/scripts/amdgpu-psp-boot.py) prepares a
private 128-MiB root disk and ISO. Preparation is the default. `--software-control`
boots without VFIO and passed all enabled boot tests and orderly power-off with
the current kernel. `--run` attaches only Raphael, once, and requests guest
power-off after kernel initialization. It does not touch `disk.img`, retry,
reset through a helper, or force-close a guest that cannot power off.

The user explicitly authorized and ran the first physical attempt at 20:14
Istanbul. It returned QEMU status 0 and powered off normally, but AMDGPU rejected
the BIOS signature (`d8 bb`) before PSP ring creation. The probe initially omitted
the saved ROM supplied by `run-c7-prime`; this was a test-setup mistake. The probe
now supplies the saved 65,536-byte 55AA/PCIR ROM identifying vendor/device
1002:164e. Its SHA-256 is
`927ded4b5fa3fa46f34f3435fb061a7dac97b1833846381797bfd80165865733`.
Default preparation verified the corrected command without opening the GPU.
Result evaluation uses this attempt's serial offset, not accumulated prior errors.

The first attempt is **not** a reproduction or resolution of PSP ring failure.
A further physical attempt needs authorization under the original restriction
on automatic potentially destructive physical GPU reset tests.
