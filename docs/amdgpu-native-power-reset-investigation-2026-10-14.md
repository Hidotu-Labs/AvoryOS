# Native power-cut reset investigation

User requirement: recover after immediate QEMU termination, without modifying
QEMU. Investigated the proposed power-cut alternative without executing a
reset, rebinding a device, modifying power controls or evaluating ACPI methods.

## Observed host capabilities

Read-only inspection of 0000:0e:00.0 found:

- Driver vfio-pci; reset_method is `bus` only.
- d3cold_allowed=1, power_state=D0, runtime_status=active, control=auto.
- Physical path is under shared bridge 0000:00:08.1.
- ACPI companion is `\\_SB_.PCI0.GP17.VGA_`.
- No power_resources directory is exported on the GPU's ACPI companion.
- The two registered ACPI power-resource children of the shared bridge are
  `\\_SB_.PCI0.GP17.XHC0.P0U0` and `\\_SB_.PCI0.GP17.XHC1.P0U1`. These belong
  to USB controllers; they do not establish a separate GPU power switch.
- PCI capabilities and DSDT/SSDT table reads require privilege. A noninteractive
  sudo read was attempted and rejected because a password is required. The
  empty redirected output was removed; no firmware dump was obtained.

These facts do not prove that firmware lacks every possible GPU reset method.
They do mean that an independent GPU power-cut mechanism is not established.
`d3cold_allowed` grants permission; it is not proof of actual D3cold power
removal or reset-domain isolation.

## Journal reset messages are platform-wide evidence

The AMD reboot-reason register records previous system reset classes. CF9
value 0xE and the ACPI-transition bit are not a device-local GPU reset API.
Writing platform reset controls would reboot the machine and would not fulfill
GPU-only recovery. The journal also does not identify the responsible actor.

Primary documentation:
https://docs.kernel.org/arch/x86/amd-debugging.html#random-reboot-issues

True D3cold means device supply power was removed; D3hot is a software-programmed
PCI low-power state and is not interchangeable with a physical cold start.
Platform firmware power-resource control must account for other consumers.

Primary documentation:
https://docs.kernel.org/power/pci.html

AvoryOS currently rejects unsupported PCI power transitions rather than faking
success (`kernel/linuxkpi/src/pci.c:756`). The guest has no existing independent
physical GPU rail-control implementation. A killed guest cannot run a final
cleanup callback: any power-cycle recovery must occur in a surviving host
component or the next initialization, through a supported hardware interface.

## Concrete preparation and remaining evidence

Extended `scripts/collect-vfio-host-evidence.sh` to capture PCI capabilities,
d3cold permission and actual state, ACPI companion/resource descriptions, and
raw DSDT/SSDT tables when readable. It performs only reads and copies: no method
execution, sysfs/config writes, binding, resets or QEMU launch.

Syntax validation and the unprivileged collector run completed. Output:
`/tmp/avory-host-evidence.ok6f68NH`; unavailable records are explicitly listed.
`git diff --check` passes.

Next required input is a privileged read-only firmware/capability snapshot:

```sh
sudo bash scripts/collect-vfio-host-evidence.sh
```

Inspect VGA_ and its ancestors for _PR0/_PR3, _PS0/_PS3 and _RST, and inspect
referenced resources and all consumers. Do not execute those methods merely
because their names exist. If an independent GPU power domain is demonstrated,
a device-scoped host lifecycle recovery can be prepared without changing QEMU.
If the only domain is shared with platform/USB/CCP functionality, it cannot be
used as an isolated GPU reset. No new physical power/reset implementation has
been enabled; MODE2 remains disabled by default after failed hardware validation.

## Privileged firmware capture reviewed

The user supplied `/tmp/avory-host-evidence.pIuyXPFd`. PCI capabilities confirm
FLReset- and PM capability version 3 with NoSoftRst-. The host still advertises
only `bus` as a reset method. No power or reset method was executed.

All 13 DSDT/SSDT files passed byte-length and ACPI checksum verification.
ACPICA iasl 20251212 was extracted into `build/tools/acpica` from the configured
package repository without installing or changing system packages. Offline
individual disassembly of DSDT and all twelve SSDTs succeeded. An initial
combined-namespace disassembly failed on duplicate firmware names; it was not
used as evidence of a successful full namespace load. Individual disassembly
requires no AML method execution. Its external references and runtime NVS
fields are not claimed to have been evaluated.

Artifacts are under `build/logs/gpu-power-domain/`: raw AML copies, DSL outputs,
compiler logs, `table-validation.json` with digests, and `gpu-scope-summary.txt`.
Explicit GP17.VGA scopes occur in DSDT, SSDT5 and SSDT6; none declares _PR0,
_PR3, _PS0, _PS3 or _RST. DSDT's base GP17 device declares the GPU address, and
its later GP17 scope supplies wake information, not a power switch. USB XHC0
and XHC1 have their own P0U0/P0U1 resources. Wireless/Bluetooth reset resources
and the GPP0 discrete-graphics power resource belong to other domains.

SSDT6 declares ATPX/ATIF under the iGPU node, including PX02 power-control
code. Its namespace placement does not prove that it powers this iGPU:
`amdgpu_atpx_set_discrete_state()` in the imported driver explicitly describes
and implements ATPX as discrete-GPU power control. Firmware PX02 invokes PCI
link/configuration and platform helpers using runtime NVS target coordinates.
Those targets and support flags have not been evaluated; ATPX must not be
invoked as an assumed isolated Raphael reset, especially with the NVIDIA host
GPU in use. There may be proprietary hardware methods beyond this inspection;
none has been established as a supported full iGPU power cycle.

Upstream Linux 6.18 also explains why NoSoftRst- is not sufficient proof of
supported PM reset. `quirk_no_pm_reset()` marks non-root-bus AMD/ATI VGA devices
with PCI_DEV_FLAGS_NO_PM_RESET, despite their advertised PM-reset bit, because
some such devices retain state after D3hot/D0. Raphael behind GP17 matches that
class/topology; this is consistent with the observed bus-only method list. It
is not a hardware measurement proving that D3hot is ineffective on this exact
chip, and does not justify bypassing the host quirk.

Primary source:
https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/pci/quirks.c

Result: no supported isolated power-cut reset was established. CF9/platform
power changes would affect the host, shared-bridge reset affects other functions,
and ATPX is not verified to target the iGPU. No native power-cut implementation
was enabled. Continue device-specific recovery investigation at PSP/VCN firmware
completion boundaries; preserve the original error responses and do not turn an
unsupported PM operation into success. X-close recovery remains unresolved.
