# Abrupt QEMU termination recovery requirement

This file preserves the initial audit. Subsequent combined recovery experiments
and current policy are recorded in [combined recovery evidence](amdgpu-combined-recovery-2026-10-14.md).
The earlier statement that no functional patch was justified describes that
audit stage, before the later authorized physical tests.

The user explicitly requires the next boot to recover after immediate QEMU
termination. Changing X-close to request shutdown does not satisfy this
requirement. No launcher/UI changes were applied for graceful close.

## Established execution-path differences

Guest shutdown executes `linuxkpi_pci_shutdown()` from the reboot/poweroff
syscalls. It invokes the bound AMDGPU driver's shutdown callback while scheduler
waits and interrupts still operate. `amdgpu_pci_shutdown()` calls
`amdgpu_device_ip_suspend()`. Once QEMU terminates immediately, guest execution
has ended; AvoryOS cannot execute a cleanup callback in that terminated VM.
Recovery must therefore happen in the host device lifecycle or the next probe.

The pinned import has an explicit next-probe reset check at
`amdgpu_device.c:3827`. For Raphael, `nv_need_reset_on_init()` (`nv.c:543`)
returns false for AMD_IS_APU before reading the PSP sign-of-life register.
Thus the ordinary dirty-driver-init reset branch is skipped for this APU.
This is confirmed source behavior, not a confirmed LinuxKPI defect or proof
that a reset is safe/required in the observed failure. Current upstream keeps
the same APU exclusion:
https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/nv.c

If invoked, the default reset selection for MP1 13.0.5 is MODE2. This path
clears bus mastering, caches PCI state, calls amdgpu_dpm_mode2_reset(), restores
PCI state and polls memory-size readiness. The Raphael SMU implementation
sends SMU_MSG_GfxDeviceDriverReset with SMU_RESET_MODE_2. This is firmware/hardware
behavior, not a purely software compatibility fix. Removing the APU exclusion
would introduce a reset that is not currently performed; it must not be done
speculatively given the host reboots.

The PSP PF ring-create path also differs from SR-IOV: it waits for ready and
creates the ring without first calling ring_stop. This alone does not establish
that an old ring should be stopped; the PF stop command is DESTROY_RINGS, not a
software-object cleanup operation. It must not be silently added as a retry.

## Recovery acceptance criteria

A valid fix must tolerate immediate termination, preserve actual error results,
and establish that prior GPU work/firmware references cannot interfere with new
ring, command, fence and TMR allocations. It must pass repeated abrupt-exit and
reinitialization hardware checks without host reboot. CPU protocol tests cannot
establish these properties. Failure to obtain a safe, supported reset boundary
may require host/VFIO changes; guest driver code alone cannot guarantee recovery
from every retained hardware state.

Current artifacts already trace ring creation and PSP command publication,
including TMR GPU/system addresses and firmware types. The next concrete evidence
needed is the last publication boundary and payload from an explicitly authorized
warm-start run, correlated with the host lifecycle logs. Determine whether the
failure is stale PSP ring state, TMR/system-address state, firmware load, or later
GFX queue recovery before selecting a hardware operation.

No physical test, reset command, firmware setting, shared bridge change, or
launcher behavior change was performed during this audit. Abrupt-exit recovery
and the host reboot remain unresolved. The older gfx_v10_0.c workarounds were
left untouched. No new functional reset patch is justified yet.
