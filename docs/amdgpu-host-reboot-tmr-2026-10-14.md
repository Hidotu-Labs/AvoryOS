# Host reboot after PSP ring creation: 2026-10-14 follow-up

Scope is fatal initialization and physical host reboot on a subsequent VM run.
The latest user trace shows PSP ring-create returning zero, then the TMR
reservation line. That is partial initialization, not successful GPU init.
The host reboot root cause remains unverified.

## Current evidence

In `amdgpu_psp.c:845`, `psp_tmr_load()` prepares `SETUP_TMR`, prints the
reservation, then submits the command. The printed `0xf41e000000` is the GPU MC
address. The command also carries `system_phy_addr`, calculated by upstream as
`MC - gmc.vram_start + vm_manager.vram_base_offset` (`amdgpu_gmc.c:832`). It is
GPU-view physical memory, not a guest CPU physical address and not simply the
PCI BAR address. No evidence yet shows that this calculation or TMR allocation
is wrong. The posted 10 MiB region ends at `0xf41ea00000` (exclusive), within the
reported VRAM range ending at `0xf420000000` (exclusive).

The retained appended serial capture in
`build/logs/prime-runs/20261014T175408Z-9s5glmdp/serial.log` includes multiple
initialization attempts. One failed ring creation with status `0x80020115`;
another passed TMR and reached AUTOLOAD_RLC response `0xFFFF000D`, SMU success,
and KIQ test failure `-110`. Its newest tail stops at the TMR reservation line.
These statuses are retained without guessing their meaning. Neither the last
serial line nor the earlier KIQ failure proves which operation reset the host.

Read-only host evidence is retained in
`build/logs/psp-host-evidence-20261014-2056/`. The previous journal records VFIO
reset completion at 20:54:09 Istanbul and ends at 20:54:47; the current boot
starts at 20:56:09. The new boot reports CF9 value 0xE / ACPI transition reset
reasons. No panic or IOMMU fault was found at the previous kernel journal tail.
Unprivileged dmesg and pstore access were unavailable; absence of a captured
panic is not proof that one never occurred. Reset completion earlier in the
journal does not identify who caused the later host reset.

## Implemented diagnostic change

`kernel/linuxkpi/src/psp_submit_trace.c` records the first 128 PSP initialization
command publications. The scoped forced include in `amdgpu_psp.c` redirects
only its `psp_ring_set_wptr` macro to this wrapper. The wrapper logs:

- Command ID, command/fence GPU addresses, expected fence and write pointer.
- For SETUP_TMR/VMR: the exact submitted GPU/system addresses, size, flags,
  VRAM bounds and software VRAM base offset.
- For LOAD_IP_FW: firmware type, GPU address and size.
- A separate `publish returned` line when the original family's write-pointer
  callback returns. This does NOT assert firmware completion or acceptance.

The original callback runs exactly once. No payload changes, retries, extra
MMIO reads/writes, physical resets, power changes, or firmware substitutions
were added. The hook is after the existing HDP flush. If the reboot occurs
before that hook, no publication record will be produced; logging cannot
promise persistence through abrupt platform reset or prove the final executed
instruction. Logging can alter timing, so any later comparison must account
for the diagnostic build.

No corrective hardware-policy change is justified by the evidence so far.
The older `gfx_v10_0.c` KIQ/HQD modifications remain pre-existing and untouched.
New fixes must continue to reside in LinuxKPI or scoped wrappers.

## Validation

- `test_kpi_psp_submit_trace.py`: PASS under ASan/UBSan, compiling the owned
  wrapper plus extracted imported TMR preparation and ring submission.
  Checks GPU/system-address distinction using modeled translation, exact
  payload/callback preservation, publication order, ring wrap, firmware type,
  TMR/VMR cases, logging limits, and original invalid-write-pointer failure.
  AMD-disabled compilation passes. These are CPU tests, not PSP execution.
- PSP ring, resource-hook, doorbell cleanup, MMIO ordering and launch-recorder
  regressions all pass. Results: `build/logs/psp-submit-trace-regressions.log`.
- Kernel and ISO builds pass; imported PSP object relocation targets
  `linuxkpi_psp_ring_set_wptr`. Build logs:
  `build/logs/psp-submit-trace-build.log`, `psp-submit-trace-iso.log`, and
  `psp-submit-trace-final-build.log`.
- `git diff --check` passes. No physical GPU test was run.

The rebuilt main ISO is prepared for a separately authorized run. Compare
`publish enter` / TMR or firmware payload / `publish returned` and any later
upstream messages to distinguish publication from subsequent firmware loading.
Do not automatically retry by closing/reopening QEMU or forcing a reset. Both
the ring rejection and host reboot remain unresolved, and CPU test success
does not establish a hardware fix.

Xerj refresh again failed on the existing amdgpu-testing.md document-family
mismatch; investigation used bounded current-source reads and exact rg searches.
