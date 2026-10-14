# Raphael MODE2 initialization reset

Current policy: MODE2 is enabled together with checked PSP ring replacement
for the scoped Raphael PF passthrough path. Physical X-close and Ctrl-C pairs
passed with both enabled. See [combined recovery evidence](amdgpu-combined-recovery-2026-10-14.md).
The earlier standalone MODE2 failure described below remains valid evidence;
it was not a successful fix. Both recovery switches can be disabled.

## Historical MODE2-only attempt and withdrawal

Implemented at the user's explicit request to add MODE2 for recovery after
immediate QEMU termination. It is an experimental hardware recovery path,
not established to fix abrupt-exit initialization failures or host reboots.

Automatic mode was enabled at the user's request, then withdrawn after actual
X-close/relaunch attempts still failed initialization and rebooted the host.
MODE2 returning zero did not establish recovery. At that stage the normal launcher used
`kpi_amdgpu_mode2_reset=0`.

Experimental opt-in remains available for a separately authorized trial:

```sh
make run-c7-prime KPI_AMDGPU_MODE2_RESET=1
```

This adds `kpi_amdgpu_mode2_reset=1` to the guest command line. It requests a
reset on every matching probe, including the first boot; it does not guess
whether prior firmware state is clean. Both the kernel parameter and launcher
at that stage defaulted to disabled. Running with
`KPI_AMDGPU_MODE2_RESET=0` restores the prior initialization decision.

## Implementation

`kernel/linuxkpi/src/amdgpu_init_reset.c` implements a boot parameter and scoped
compatibility hooks. `kernel/linuxkpi/include/linuxkpi/amdgpu_init_reset.h` is
forced into imported `amdgpu_device.c` only. No imported source was edited.

The experimental reset policy requires all of:

- PCI AMD 1002:164e and AMD_IS_APU.
- Passthrough detection, no SR-IOV VF, no physical XGMI hive.
- PSP/MP0 and SMU/MP1 versions both 13.0.5.

On a match, the normal `need_reset_on_init` boundary returns true rather than
nv's default APU exclusion. Upstream device initialization temporarily selects
AUTO (`AMD_RESET_METHOD_NONE`), and the imported nv selector chooses MODE2 for
MP1 13.0.5. The imported MODE2 callback performs scratch-register bookkeeping,
bus-master disable, PCI state cache, the SMU reset, PCI restore and readiness
polling. Raphael's SMU implementation sends `GfxDeviceDriverReset` with mode 2.
The original module reset-method value is restored by the imported init path,
even on error. No MODE1, BACO, PCI-function or bridge-reset fallback was added.

The imported nv MODE2 readiness loop can expire while returning zero. The
scoped reset wrapper therefore checks the same `get_memsize` register once on
successful return; if it still reads 0xffffffff, the wrapper returns -ETIME
(-62). Existing SMU reset errors propagate unchanged. Reset failure aborts
initialization through the existing imported error path; it is never reported
as successful recovery. The wrapper adds enter/exit records:

```text
LinuxKPI: Raphael MODE2 init reset requested
LinuxKPI: Raphael MODE2 init reset enter
MODE2 reset
LinuxKPI: Raphael MODE2 init reset exit ret=...
```

`ret=0` means the reset callback and added readiness check succeeded; it does
not prove subsequent PSP/TMR/GFX initialization will succeed or that the host
is safe. The explicitly disabled path does not perform the added readiness read.

## Validation

`test_amdgpu_mode2_reset.py` compiles the owned policy and extracted imported
nv reset-selector, dispatcher and MODE2 callback under ASan/UBSan. Hardware
and PCI operations are modeled. It now checks default-enabled policy and explicit disable, eight scope
exclusions, actual AUTO-to-MODE2 dispatch, bus-master/PCI/SMU call ordering,
exactly one reset attempt, backend error propagation, restoration of the prior
module parameter, and a stuck readiness register returning -62. AMD-disabled
compilation passes. These tests do not execute hardware reset or PSP firmware.

PSP publication, PSP ring and doorbell-cleanup regressions also pass. Results:
`build/logs/amdgpu-mode2-regressions.log`.

Kernel and ISO builds pass. Linked symbols include both compatibility hooks
and the boot parameter descriptor; imported device-init relocations target the
hooks. The ISO kernel matches the current stripped ELF byte-for-byte.
The eight-CPU private GPU-free TTM rollback guest also passes and shuts down
normally (`build/logs/amdgpu-mode2-software-boot.log`). AMDGPU is disabled in
that guest, so it does not validate the physical reset.
Build logs: `build/logs/amdgpu-mode2-build.log` and
`build/logs/amdgpu-mode2-iso.log`.

The agent did not launch a physical guest or execute a reset during development.
The user subsequently tested physical boots and reported failed recovery and
repeated host reboots. No
host VFIO/reset-method/bridge/power/firmware settings were changed. The added
code will send the SMU hardware reset on every matching
physical-device boot. Actual abrupt-termination recovery failed user hardware validation; the host
reboot cause remains unresolved. See the follow-up below.

## Hardware failure follow-up

Retained latest launch: `build/logs/prime-runs/20261014T182027Z-8oh1nhl7/`.
The guest records MODE2 enter/exit ret=0, successful PSP ring creation, and
TMR GPU `0xf41e000000` / GPU-view system `0x45e000000`, size `0xa00000`,
flags 2. It publishes several firmware commands and AUTOLOAD_RLC; the last
captured publication is LOAD_IP_FW type `0xd` (VCN), GPU address `0xb82000`,
size `0x8bb50`, fence 16, wptr 256. The publication callback returned; no
completion record for this command exists in that tail. This is a captured
boundary, not proof that VCN caused a platform reset. Earlier retained runs
continued beyond the same VCN command and initialized the GPU.

Host evidence records VFIO reset completion at 21:20:28 Istanbul; the next
host boot begins at 21:21:23. Evidence and bounded guest extracts are saved
in `build/logs/amdgpu-mode2-hardware-failure/`. The user reports guest shutdown
reliably permits reinitialization, while abrupt termination followed by repeated
boots causes failures and host resets. Do not automatically repeat these tests.

After disabling both defaults, four CPU regressions passed. The replacement
ISO built successfully. Logs: `build/logs/amdgpu-mode2-disabled-regressions.log`
and `build/logs/amdgpu-mode2-disable-iso.log`. This withdrawal does not fix the
original X-close recovery failure; it removes the unsuccessful automatic reset
experiment. Continue examining firmware completion and VCN/SMU state, without
adding an unverified reset fallback or disabling VCN to conceal the failure.
