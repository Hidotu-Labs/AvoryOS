# PSP/VCN reinitialization investigation

## Findings from retained hardware traces

Compared the last AMDGPU initialization in:

- `build/logs/prime-runs/20261014T180507Z-fdc15rt5/serial.log` (continues through
  initialization and userspace).
- `build/logs/prime-runs/20261014T182027Z-8oh1nhl7/serial.log` (ends at VCN
  firmware publication in the user-reported host reboot sequence).

The successful boot has 21 traced publications; the failing boot has 16.
All 16 overlapping publication records and payload metadata are identical,
including command/fence GPU addresses, TMR GPU/system addresses and flags,
firmware types, addresses and sizes. Comparison is saved in
`build/logs/psp-vcn-publication-comparison.json`. This compares metadata only,
not memory contents, PSP acceptance, retained engine state or DMA execution.

The failed tail is LOAD_IP_FW, type 0xd (VCN), source GPU address 0xb82000,
size 0x8bb50, fence 16, wptr 256. Its write-pointer callback returned. The next
command in the successful trace is DMUB firmware type 0x33. The captured tail
therefore places the uncertainty after VCN publication and before the next
publication, not necessarily inside VCN firmware execution. The prior
AUTOLOAD_RLC publication is also present. Neither establishes its status:
upstream can warn on nonzero PSP responses and continue on a PF.

`build/firmware/lib/firmware/amdgpu/vcn_3_1_2.bin` has a valid file length and
payload bounds; its header payload size is exactly 0x8bb50, matching the trace.
The VCN source range ends at 0xc0db50 and the subsequent page-aligned firmware
allocation begins at 0xc0e000 in the successful trace. No metadata overlap or
size mismatch was found. Header information and SHA-256 are saved in
`build/logs/psp-vcn-firmware-validation.json`. This does not verify bytes that
the physical GPU fetched or the image in the already terminated VM.

## Source-path investigation

PSP firmware loading occurs before VCN's hardware initialization/ring tests.
`psp_load_non_psp_fw()` submits VCN through `psp_execute_ip_fw_load()` and
`psp_cmd_submit_buf()`. On success it proceeds to later firmware commands.
The current imported completion loop invalidates HDP, polls a CPU-mapped
fence for equality with its incremented counter, then copies response fields.
Its PF nonzero-status handling intentionally warns rather than always failing.
No change to that upstream policy was made, and status values are not decoded.

`vcn_v3_0_sw_init()` initializes new software objects and calls
`amdgpu_vcn_resume()`. With PSP firmware loading, the fresh VCPU buffer is
cleared, while the firmware is staged separately for PSP. This software
initialization does not itself establish that an engine surviving a terminated
VM has been quiesced. That remains a hardware-state hypothesis, not a confirmed
LinuxKPI defect.

The ordinary PF firmware BO is allocated in GTT by
`amdgpu_ucode_create_bo()`; its source addresses are GART addresses rather than
CPU physical addresses. The kernel BO creation flags do not request CPU_GTT_USWC,
and the imported TTM creation path selects cached TT storage in that case.
No new WC-drain correction was justified from this firmware-upload path. Existing
DMA-mask and MMIO-ordering regressions pass; they cannot verify warm GPU state.

## Implemented completion diagnostics

`kernel/linuxkpi/src/psp_completion_trace.c` supplies a scoped HDP-invalidation
wrapper. The existing forced include in imported `amdgpu_psp.c` redirects only
that translation unit's invalidation calls. The wrapper delegates to the
original HDP helper exactly once, then samples the software-mapped fence.
Only on equality with the expected fence does it record:

```text
LinuxKPI: PSP fence observed cmd=6 expected=16 observed=16 status=... fw_gpu=...
```

This observes the same fence condition as the imported completion loop. The
loop stops after a matching fence, so it does not log every pending poll.
A completion arriving between the hook's sample and the imported loop's check
can be missed. Absence of a record is not proof of an uncompleted command.
Tracing is limited to the first 128 initialization commands. It records the
raw response status and returned firmware GPU address, even when the status
is nonzero; matching a fence is not presented as successful firmware loading.

No extra MMIO, waits, reset commands, retries, allocations, error-policy changes
or feature disabling were added. Imported source is unchanged. Automatic MODE2
remains disabled after failed hardware validation. The additional memory reads
and logging can change timing, which must be considered in future hardware
comparisons. These diagnostics do not promise capturing the final instruction
through an abrupt platform reset.

## Validation

- New `test_kpi_psp_completion_trace.py`: PASS under ASan/UBSan. Compiles the
  actual imported submission loop with the owned hook and modeled hardware.
  Checks immediate/delayed completion, nonzero PF status preserved, VF errors,
  20,000-poll timeout, RAS exit, submission error, no-hardware early exit,
  disabled tracing, the 128-command bound, and completion arriving between
  the diagnostic sample and the original loop check (successful command with
  no record, proving that missing diagnostics are not a failure verdict). Original HDP call counts and
  sleep counts are checked. AMD-disabled compilation passes.
- Existing PSP publication/ring, MODE2 scoping/default-off, doorbell cleanup,
  DMA-mask and MMIO-ordering regressions: PASS. Seven-script transcript:
  `build/logs/psp-vcn-completion-regressions.log`.
- Kernel and ISO builds: PASS. Imported PSP object has both invalidation-call
  relocations to the scoped wrapper. ISO kernel matches current stripped ELF
  byte-for-byte. Build logs: `build/logs/psp-vcn-completion-build.log` and
  `build/logs/psp-vcn-completion-iso.log` and
  `build/logs/psp-vcn-completion-final-iso.log`. The final sampling-race test
  result is in `build/logs/psp-vcn-completion-final-test.log`.
- `git diff --check`: PASS.

No physical guest was launched or reset tested. No supported recovery fix was
identified in this investigation. PSP ring rejection, retained-state recovery
and the physical host reboot remain unresolved.

## Next evidence boundary

On a separately authorized hardware run, preserve the publication and matching
fence record for VCN fence 16, along with raw status/fw_gpu and subsequent DMUB
publication. A missing record leaves completion uncertain, including the sampling race; equality with a nonzero status establishes a reported
firmware failure; equality with zero followed by later records moves the target
forward. None of these alone identifies the platform reset actor. Do not use
repeated X-close/relaunch attempts as an automatic evidence-collection loop.

## One approved physical baseline completed

After explicit approval for exactly one initialization attempt, the agent ran
`python3 scripts/amdgpu-psp-boot.py --run` once. MODE2 was explicitly disabled.
No existing Raphael QEMU process was present. There was no X-close, retry,
forced termination or host setting change. The disposable guest reached
userspace and performed orderly AMDGPU/NVMe shutdown; QEMU exited with status 0.
The host boot ID was unchanged throughout the attempt.

The captured completion records establish for this run:

- AUTOLOAD_RLC: fence 15 matched, raw status 0.
- VCN: fence 16 matched, raw status 0, returned firmware GPU address
  0xf41e000000.
- DMUB: fence 17 matched, raw status 0, returned firmware GPU address
  0xf41e300000.
- DRM logged `Initialized amdgpu 3.54.0 ... for 0000:00:03.0`.
- PCI AMDGPU shutdown callback completed before normal guest power-off.

The run also logged the existing BIOS connector construction failure and
`registering pm sysfs failed (-19)`; it was not warning/error-free. Neither
aborted driver registration in this attempt. These are not being silently
reported as fixed.

Artifacts:

- `build/logs/psp-vcn-approved-physical-attempt.log`
- `build/logs/psp-vcn-approved-physical-serial.log` (only this attempt's bytes)
- `build/logs/psp-vcn-approved-physical-summary.json` (digest and results)
- `build/amdgpu-psp-boot/launch-evidence/20261014T194505Z-3b58irgr/launch.json`

This is a successful initialization/shutdown baseline, not an abrupt-exit
recovery test. It shows that the recorded command metadata and firmware payload
can complete on this device, without establishing which retained state caused
the prior failure. The one-run approval is consumed; no additional physical
attempt is authorized by it. X-close recovery and recurring host reboots remain
unresolved, and no new recovery command has been enabled.
